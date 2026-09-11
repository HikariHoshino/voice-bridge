#include "InputInjector.h"

#include "Errors.h"

#include <UIAutomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <new>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace
{
    using Microsoft::WRL::ComPtr;

    constexpr size_t MaxEventsPerBatch = 96;
    constexpr WORD CleanupMarker = L'x';

    INPUT KeyboardInput(WORD virtualKey, WORD scanCode, DWORD flags)
    {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = virtualKey;
        input.ki.wScan = scanCode;
        input.ki.dwFlags = flags;
        return input;
    }

    bool NativeEditClass(HWND window)
    {
        wchar_t className[256]{};
        if (!GetClassNameW(window, className, static_cast<int>(std::size(className))))
        {
            return false;
        }
        std::wstring name(className);
        std::transform(name.begin(), name.end(), name.begin(), [](wchar_t character)
        {
            return static_cast<wchar_t>(std::towlower(character));
        });
        return name == L"edit" || name.find(L"richedit") != std::wstring::npos ||
            name.find(L".edit.") != std::wstring::npos;
    }

    std::wstring Lower(std::wstring value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t character)
        {
            return static_cast<wchar_t>(std::towlower(character));
        });
        return value;
    }

    std::vector<std::vector<INPUT>> UnicodeEventGroups(std::wstring const& text)
    {
        std::vector<std::vector<INPUT>> groups;
        for (size_t index = 0; index < text.size(); ++index)
        {
            auto character = text[index];
            if (character == L'\r')
            {
                if (index + 1 < text.size() && text[index + 1] == L'\n')
                {
                    ++index;
                }
                character = L'\n';
            }
            if (character == L'\n')
            {
                groups.push_back({
                    KeyboardInput(VK_SHIFT, 0, 0),
                    KeyboardInput(VK_RETURN, 0, 0),
                    KeyboardInput(VK_RETURN, 0, KEYEVENTF_KEYUP),
                    KeyboardInput(VK_SHIFT, 0, KEYEVENTF_KEYUP),
                    KeyboardInput(0, CleanupMarker, KEYEVENTF_UNICODE),
                    KeyboardInput(0, CleanupMarker, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP),
                    KeyboardInput(VK_SHIFT, 0, 0),
                    KeyboardInput(VK_HOME, 0, KEYEVENTF_EXTENDEDKEY),
                    KeyboardInput(VK_HOME, 0, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP),
                    KeyboardInput(VK_HOME, 0, KEYEVENTF_EXTENDEDKEY),
                    KeyboardInput(VK_HOME, 0, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP),
                    KeyboardInput(VK_SHIFT, 0, KEYEVENTF_KEYUP),
                    KeyboardInput(VK_BACK, 0, 0),
                    KeyboardInput(VK_BACK, 0, KEYEVENTF_KEYUP),
                });
            }
            else
            {
                auto unit = static_cast<WORD>(character);
                groups.push_back({
                    KeyboardInput(0, unit, KEYEVENTF_UNICODE),
                    KeyboardInput(0, unit, KEYEVENTF_UNICODE | KEYEVENTF_KEYUP),
                });
            }
        }
        return groups;
    }
}

namespace VoiceBridge::Core
{
    void InputInjector::PasteText(std::wstring const& text) const
    {
        auto target = RequireTextInputTarget();
        auto owner = CreateWindowExW(
            0,
            L"STATIC",
            L"",
            0,
            0,
            0,
            0,
            0,
            nullptr,
            nullptr,
            GetModuleHandleW(nullptr),
            nullptr);
        if (!owner)
        {
            throw std::system_error(
                static_cast<int>(GetLastError()),
                std::system_category(),
                "Cannot create clipboard owner window");
        }

        try
        {
            SetClipboardText(owner, text);
            if (RequireTextInputTarget() != target)
            {
                throw NoTextInputFocusError(
                    "电脑端输入光标已变化，请重新点击输入框后再发送");
            }
            SendInputs(BuildPasteInputs());
        }
        catch (...)
        {
            DestroyWindow(owner);
            throw;
        }
        DestroyWindow(owner);
    }

    void InputInjector::PressEnter() const
    {
        RequireTextInputTarget();
        SendInputs(BuildEnterInputs());
    }

    std::vector<INPUT> InputInjector::BuildEnterInputs()
    {
        return {
            KeyboardInput(VK_RETURN, 0, 0),
            KeyboardInput(VK_RETURN, 0, KEYEVENTF_KEYUP),
        };
    }

    std::vector<INPUT> InputInjector::BuildPasteInputs()
    {
        return {
            KeyboardInput(VK_CONTROL, 0, 0),
            KeyboardInput(L'V', 0, 0),
            KeyboardInput(L'V', 0, KEYEVENTF_KEYUP),
            KeyboardInput(VK_CONTROL, 0, KEYEVENTF_KEYUP),
        };
    }

    std::vector<INPUT> InputInjector::BuildUnicodeInputs(std::wstring const& text)
    {
        std::vector<INPUT> inputs;
        for (auto& group : UnicodeEventGroups(text))
        {
            inputs.insert(inputs.end(), group.begin(), group.end());
        }
        return inputs;
    }

    std::vector<std::vector<INPUT>> InputInjector::BuildUnicodeInputBatches(
        std::wstring const& text)
    {
        std::vector<std::vector<INPUT>> batches;
        std::vector<INPUT> current;
        for (auto& group : UnicodeEventGroups(text))
        {
            if (!current.empty() && current.size() + group.size() > MaxEventsPerBatch)
            {
                batches.push_back(std::move(current));
                current.clear();
            }
            current.insert(current.end(), group.begin(), group.end());
        }
        if (!current.empty())
        {
            batches.push_back(std::move(current));
        }
        return batches;
    }

    std::optional<InputInjector::InputTarget> InputInjector::TextInputTarget()
    {
        auto foreground = GetForegroundWindow();
        if (!foreground)
        {
            return std::nullopt;
        }

        DWORD processId{};
        auto threadId = GetWindowThreadProcessId(foreground, &processId);
        GUITHREADINFO info{ sizeof(info) };
        if (threadId && GetGUIThreadInfo(threadId, &info) && info.hwndFocus)
        {
            if (info.hwndCaret || NativeEditClass(info.hwndFocus))
            {
                return InputTarget{ foreground, info.hwndFocus, processId, {} };
            }
        }

        if (auto runtimeId = UiaTextInputRuntimeId(processId))
        {
            return InputTarget{ foreground, nullptr, processId, std::move(*runtimeId) };
        }
        return std::nullopt;
    }

    std::optional<std::vector<int>> InputInjector::UiaTextInputRuntimeId(DWORD processId)
    {
        if (!processId)
        {
            return std::nullopt;
        }

        auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        auto uninitialize = SUCCEEDED(initialized);
        if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
        {
            return std::nullopt;
        }

        std::optional<std::vector<int>> result;
        try
        {
            ComPtr<IUIAutomation> automation;
            if (FAILED(CoCreateInstance(
                CLSID_CUIAutomation,
                nullptr,
                CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&automation))))
            {
                throw std::runtime_error("UI Automation is unavailable");
            }
            ComPtr<IUIAutomationElement> element;
            if (FAILED(automation->GetFocusedElement(&element)) || !element)
            {
                throw std::runtime_error("No focused UI Automation element");
            }

            int elementProcess{};
            BOOL enabled{};
            CONTROLTYPEID controlType{};
            if (FAILED(element->get_CurrentProcessId(&elementProcess)) ||
                FAILED(element->get_CurrentIsEnabled(&enabled)) ||
                FAILED(element->get_CurrentControlType(&controlType)) ||
                elementProcess != static_cast<int>(processId) || !enabled ||
                (controlType != UIA_EditControlTypeId &&
                    controlType != UIA_DocumentControlTypeId))
            {
                throw std::runtime_error("Focused element is not editable");
            }

            ComPtr<IUnknown> pattern;
            if (SUCCEEDED(element->GetCurrentPattern(UIA_ValuePatternId, &pattern)) && pattern)
            {
                ComPtr<IUIAutomationValuePattern> valuePattern;
                BOOL readOnly{ TRUE };
                if (FAILED(pattern.As(&valuePattern)) ||
                    FAILED(valuePattern->get_CurrentIsReadOnly(&readOnly)) || readOnly)
                {
                    throw std::runtime_error("Focused value is read-only");
                }
            }
            else
            {
                BSTR ariaRole{};
                if (FAILED(element->get_CurrentAriaRole(&ariaRole)))
                {
                    throw std::runtime_error("Focused document has no editable role");
                }
                auto role = Lower(ariaRole ? ariaRole : L"");
                SysFreeString(ariaRole);
                ComPtr<IUnknown> textPattern;
                if ((role != L"textbox" && role != L"searchbox") ||
                    FAILED(element->GetCurrentPattern(UIA_TextPatternId, &textPattern)) ||
                    !textPattern)
                {
                    throw std::runtime_error("Focused document is not an editable text box");
                }
            }

            SAFEARRAY* runtimeId{};
            if (FAILED(element->GetRuntimeId(&runtimeId)) || !runtimeId)
            {
                throw std::runtime_error("Focused element has no runtime id");
            }
            LONG lower{};
            LONG upper{-1};
            if (FAILED(SafeArrayGetLBound(runtimeId, 1, &lower)) ||
                FAILED(SafeArrayGetUBound(runtimeId, 1, &upper)))
            {
                SafeArrayDestroy(runtimeId);
                throw std::runtime_error("Focused element runtime id is invalid");
            }
            std::vector<int> values;
            values.reserve(static_cast<size_t>(upper - lower + 1));
            for (LONG index = lower; index <= upper; ++index)
            {
                int value{};
                if (FAILED(SafeArrayGetElement(runtimeId, &index, &value)))
                {
                    values.clear();
                    break;
                }
                values.push_back(value);
            }
            SafeArrayDestroy(runtimeId);
            if (!values.empty())
            {
                result = std::move(values);
            }
        }
        catch (...)
        {
            result = std::nullopt;
        }

        if (uninitialize)
        {
            CoUninitialize();
        }
        return result;
    }

    InputInjector::InputTarget InputInjector::RequireTextInputTarget()
    {
        auto target = TextInputTarget();
        if (!target)
        {
            throw NoTextInputFocusError(
                "电脑端未检测到输入光标，请先在电脑输入框中点击一下");
        }
        return std::move(*target);
    }

    void InputInjector::SetClipboardText(HWND owner, std::wstring const& text)
    {
        bool opened{};
        for (int attempt = 0; attempt < 20; ++attempt)
        {
            if (OpenClipboard(owner))
            {
                opened = true;
                break;
            }
            if (attempt != 19)
            {
                Sleep(10);
            }
        }
        if (!opened)
        {
            throw std::system_error(
                static_cast<int>(GetLastError()),
                std::system_category(),
                "Cannot open the clipboard");
        }

        HGLOBAL memory{};
        try
        {
            if (!EmptyClipboard())
            {
                throw std::system_error(
                    static_cast<int>(GetLastError()),
                    std::system_category(),
                    "Cannot clear the clipboard");
            }
            auto size = (text.size() + 1) * sizeof(wchar_t);
            memory = GlobalAlloc(GMEM_MOVEABLE, size);
            if (!memory)
            {
                throw std::bad_alloc();
            }
            auto destination = GlobalLock(memory);
            if (!destination)
            {
                throw std::system_error(
                    static_cast<int>(GetLastError()),
                    std::system_category(),
                    "Cannot lock clipboard memory");
            }
            memcpy(destination, text.c_str(), size);
            GlobalUnlock(memory);
            if (!SetClipboardData(CF_UNICODETEXT, memory))
            {
                throw std::system_error(
                    static_cast<int>(GetLastError()),
                    std::system_category(),
                    "Cannot publish clipboard text");
            }
            memory = nullptr;
        }
        catch (...)
        {
            if (memory)
            {
                GlobalFree(memory);
            }
            CloseClipboard();
            throw;
        }
        CloseClipboard();
    }

    void InputInjector::SendInputs(std::vector<INPUT> const& inputs)
    {
        auto sent = SendInput(
            static_cast<UINT>(inputs.size()),
            const_cast<INPUT*>(inputs.data()),
            sizeof(INPUT));
        if (sent != inputs.size())
        {
            auto error = GetLastError();
            if (error)
            {
                throw std::system_error(
                    static_cast<int>(error),
                    std::system_category(),
                    "Windows rejected simulated input");
            }
            throw std::runtime_error("Windows 阻止了向当前光标输入文字");
        }
    }
}

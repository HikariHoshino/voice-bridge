#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

namespace VoiceBridge::Core
{
    class InputInjector
    {
    public:
        void PasteText(std::wstring const& text) const;
        void PressEnter() const;

        static std::vector<INPUT> BuildEnterInputs();
        static std::vector<INPUT> BuildPasteInputs();
        static std::vector<INPUT> BuildUnicodeInputs(std::wstring const& text);
        static std::vector<std::vector<INPUT>> BuildUnicodeInputBatches(
            std::wstring const& text);

    private:
        struct InputTarget
        {
            HWND foreground{};
            HWND focus{};
            DWORD processId{};
            std::vector<int> runtimeId;

            bool operator==(InputTarget const&) const = default;
        };

        static std::optional<InputTarget> TextInputTarget();
        static std::optional<std::vector<int>> UiaTextInputRuntimeId(DWORD processId);
        static InputTarget RequireTextInputTarget();
        static void SetClipboardText(HWND owner, std::wstring const& text);
        static void SendInputs(std::vector<INPUT> const& inputs);
    };
}

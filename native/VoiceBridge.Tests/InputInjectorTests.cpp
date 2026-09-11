#include "../VoiceBridge/Core/InputInjector.h"

#include <filesystem>
#include <stdexcept>

namespace
{
    using VoiceBridge::Core::InputInjector;

    void Require(bool condition, char const* reason)
    {
        if (!condition)
        {
            throw std::runtime_error(reason);
        }
    }

    void TestRemoteEnterEvents()
    {
        auto inputs = InputInjector::BuildEnterInputs();
        Require(inputs.size() == 2, "remote Enter must contain exactly one key-down and key-up");
        Require(inputs[0].ki.wVk == VK_RETURN && inputs[0].ki.dwFlags == 0,
            "remote Enter must start with one Return key-down");
        Require(inputs[1].ki.wVk == VK_RETURN && inputs[1].ki.dwFlags == KEYEVENTF_KEYUP,
            "remote Enter must end with one Return key-up");
    }

    void TestClipboardPasteEvents()
    {
        auto inputs = InputInjector::BuildPasteInputs();
        Require(inputs.size() == 4, "clipboard insertion must send one Ctrl+V chord only");
        Require(inputs[0].ki.wVk == VK_CONTROL && inputs[0].ki.dwFlags == 0,
            "clipboard insertion must press Control first");
        Require(inputs[1].ki.wVk == L'V' && inputs[1].ki.dwFlags == 0,
            "clipboard insertion must press V once");
        Require(inputs[2].ki.wVk == L'V' && inputs[2].ki.dwFlags == KEYEVENTF_KEYUP,
            "clipboard insertion must release V once");
        Require(inputs[3].ki.wVk == VK_CONTROL && inputs[3].ki.dwFlags == KEYEVENTF_KEYUP,
            "clipboard insertion must release Control last");
    }

    void TestUnicodeUnitsRemainExact()
    {
        auto inputs = InputInjector::BuildUnicodeInputs(L"A中🙂");
        Require(sizeof(INPUT) == 40, "x64 SendInput ABI must remain 40 bytes");
        WORD expected[] = { 0x0041, 0x4e2d, 0xd83d, 0xde42 };
        Require(inputs.size() == std::size(expected) * 2,
            "each UTF-16 code unit must have one down and one up event");
        for (size_t index = 0; index < std::size(expected); ++index)
        {
            Require(inputs[index * 2].ki.wScan == expected[index],
                "Unicode input must preserve every UTF-16 code unit");
            Require(inputs[index * 2].ki.dwFlags == KEYEVENTF_UNICODE &&
                inputs[index * 2 + 1].ki.dwFlags ==
                    (KEYEVENTF_UNICODE | KEYEVENTF_KEYUP),
                "each Unicode unit must retain its down/up flags");
        }
    }

    void TestNewlinesAndBatchLimits()
    {
        auto inputs = InputInjector::BuildUnicodeInputs(L"甲  \r\n\n\t乙");
        Require(inputs.size() == 38,
            "CRLF and LF must each use the existing smart-editor newline event group");
        for (auto start : { size_t{ 6 }, size_t{ 20 } })
        {
            Require(inputs[start].ki.wVk == VK_SHIFT &&
                inputs[start + 1].ki.wVk == VK_RETURN &&
                inputs[start + 12].ki.wVk == VK_BACK,
                "newline groups must retain Shift+Enter and cleanup behavior");
        }

        std::wstring longText(80, L'字');
        auto batches = InputInjector::BuildUnicodeInputBatches(longText);
        Require(batches.size() == 2,
            "long Unicode input must split without exceeding the old 96-event boundary");
        for (auto const& batch : batches)
        {
            Require(batch.size() <= 96, "no SendInput batch may exceed 96 events");
        }
    }
}

void RunInputInjectorTests(std::filesystem::path const&)
{
    TestRemoteEnterEvents();
    TestClipboardPasteEvents();
    TestUnicodeUnitsRemainExact();
    TestNewlinesAndBatchLimits();
}

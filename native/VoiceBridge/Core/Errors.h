#pragma once

#include <stdexcept>

namespace VoiceBridge::Core
{
    class NoTextInputFocusError : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };
}

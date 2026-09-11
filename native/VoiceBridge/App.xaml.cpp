#include "pch.h"
#include "App.xaml.h"
#include "MainWindow.xaml.h"
#include "App.xaml.g.hpp"

using namespace winrt;
using namespace Microsoft::UI::Xaml;

namespace winrt::VoiceBridge::implementation
{
    App::App()
    {
#if defined _DEBUG && !defined DISABLE_XAML_GENERATED_BREAK_ON_UNHANDLED_EXCEPTION
        UnhandledException([](IInspectable const&, UnhandledExceptionEventArgs const& args)
        {
            if (IsDebuggerPresent())
            {
                auto message = args.Message();
                __debugbreak();
            }
        });
#endif
    }

    void App::OnLaunched(LaunchActivatedEventArgs const&)
    {
        m_window = make<MainWindow>();
        m_window.Activate();
    }
}

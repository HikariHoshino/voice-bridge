#pragma once

#include "MainWindow.g.h"

namespace winrt::VoiceBridge::implementation
{
    struct MainWindow : MainWindowT<MainWindow>
    {
        MainWindow();

        void ThemeButton_Click(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);
        void TransferToggleButton_Click(
            Windows::Foundation::IInspectable const&,
            Microsoft::UI::Xaml::RoutedEventArgs const&);

    private:
        bool m_dark{};
        bool m_transferExpanded{};

        void ApplyTheme();
        void ConfigureWindow();
        void AnimateTransfer(bool expand);
    };
}

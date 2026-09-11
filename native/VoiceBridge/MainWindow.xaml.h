#pragma once

#include "Core/SettingsStore.h"
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
        ::VoiceBridge::Core::SettingsStore m_settingsStore;
        ::VoiceBridge::Core::Settings m_settings;

        void ApplyTheme();
        void ConfigureWindow();
        void AnimateTransfer(bool expand);
    };
}

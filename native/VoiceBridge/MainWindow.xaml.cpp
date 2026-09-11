#include "pch.h"
#include "MainWindow.xaml.h"
#include "MainWindow.xaml.g.hpp"

#include <microsoft.ui.xaml.window.h>

using namespace winrt;
using namespace Windows::Foundation;
using namespace Windows::Graphics;
using namespace Microsoft::UI;
using namespace Microsoft::UI::Composition;
using namespace Microsoft::UI::Windowing;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Hosting;

namespace winrt::VoiceBridge::implementation
{
    MainWindow::MainWindow()
    {
        InitializeComponent();
        Title(L"声桥 · Windows 原生版");
        ConfigureWindow();
        ApplyTheme();
    }

    void MainWindow::ConfigureWindow()
    {
        auto windowNative = this->try_as<::IWindowNative>();
        HWND window{};
        check_hresult(windowNative->get_WindowHandle(&window));
        WindowId id{ reinterpret_cast<uint64_t>(window) };
        auto appWindow = AppWindow::GetFromWindowId(id);
        appWindow.Resize(SizeInt32{ 648, 540 });

        auto icon = std::filesystem::current_path() / L"Assets" / L"voice-bridge.ico";
        if (std::filesystem::is_regular_file(icon))
        {
            appWindow.SetIcon(icon.c_str());
        }
    }

    void MainWindow::ApplyTheme()
    {
        RootGrid().RequestedTheme(m_dark ? ElementTheme::Dark : ElementTheme::Light);
        ThemeIcon().Glyph(m_dark ? L"\xE706" : L"\xE708");
    }

    void MainWindow::ThemeButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        m_dark = !m_dark;
        auto visual = ElementCompositionPreview::GetElementVisual(RootGrid());
        auto compositor = visual.Compositor();
        auto fade = compositor.CreateScalarKeyFrameAnimation();
        fade.InsertKeyFrame(0.0f, 0.82f);
        fade.InsertKeyFrame(1.0f, 1.0f);
        fade.Duration(std::chrono::milliseconds(224));
        ApplyTheme();
        visual.StartAnimation(L"Opacity", fade);
    }

    void MainWindow::TransferToggleButton_Click(IInspectable const&, RoutedEventArgs const&)
    {
        AnimateTransfer(!m_transferExpanded);
    }

    void MainWindow::AnimateTransfer(bool expand)
    {
        m_transferExpanded = expand;
        auto content = TransferContent();
        if (expand)
        {
            content.Visibility(Visibility::Visible);
            content.Height(172.0);
        }

        auto visual = ElementCompositionPreview::GetElementVisual(content);
        auto compositor = visual.Compositor();
        visual.CenterPoint({ 0.0f, 0.0f, 0.0f });

        auto opacity = compositor.CreateScalarKeyFrameAnimation();
        opacity.InsertKeyFrame(0.0f, expand ? 0.0f : 1.0f);
        opacity.InsertKeyFrame(1.0f, expand ? 1.0f : 0.0f);
        opacity.Duration(std::chrono::milliseconds(200));

        auto offset = compositor.CreateVector3KeyFrameAnimation();
        using Windows::Foundation::Numerics::float3;
        offset.InsertKeyFrame(0.0f, expand ? float3{ 0.0f, -12.0f, 0.0f } : float3{});
        offset.InsertKeyFrame(1.0f, expand ? float3{} : float3{ 0.0f, -12.0f, 0.0f });
        offset.Duration(std::chrono::milliseconds(200));

        auto batch = compositor.CreateScopedBatch(CompositionBatchTypes::Animation);
        visual.StartAnimation(L"Opacity", opacity);
        visual.StartAnimation(L"Offset", offset);
        batch.End();
        batch.Completed([weak = get_weak(), expand](IInspectable const&, CompositionBatchCompletedEventArgs const&)
        {
            if (auto self = weak.get())
            {
                if (!expand)
                {
                    self->TransferContent().Height(0.0);
                    self->TransferContent().Visibility(Visibility::Collapsed);
                }
            }
        });

        TransferToggleIcon().Glyph(expand ? L"\xE70E" : L"\xE70D");

        auto windowNative = this->try_as<::IWindowNative>();
        HWND window{};
        check_hresult(windowNative->get_WindowHandle(&window));
        auto appWindow = AppWindow::GetFromWindowId(WindowId{ reinterpret_cast<uint64_t>(window) });
        appWindow.Resize(SizeInt32{ 648, expand ? 763 : 540 });
    }
}

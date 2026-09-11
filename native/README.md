# 声桥 Windows 原生重构

本目录为独立分支中的 C++/WinRT + WinUI 3 + Composition 实现。原 Python 文件、手机页面和图标保持基线字节，恢复点见源码中的 BASELINE.md。实际验收范围见 VALIDATION.md。

## 使用交付包

将整个文件夹放到固定位置，双击 `VoiceBridge.exe`。不需要 Python 或 Visual Studio，也不需要单独安装 Windows App SDK。请保留 EXE 同目录的 DLL、资源和子目录。

手机和电脑连接同一局域网，在电脑点击二维码，用手机浏览器打开扫码所得地址并收藏。手机输入法的语音转文字仍由手机输入法提供。手机向电脑发送前，先点击电脑目标输入框。电脑向手机发送时，展开“电脑 → 手机”，输入文字并发送。

如果 Windows 首次询问防火墙权限，局域网连接需要允许此程序访问当前专用网络。目标程序若以管理员身份运行，Windows 可能阻止普通权限声桥向其输入。

## 构建

需要 Windows 10 2004 或更新版本、Visual Studio 2022 的 MSVC v143 C++ 工具、Windows SDK 10.0.26100.0。首次构建需访问 NuGet。

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File native/build.ps1
```

输出为 `native/bin/Release/VoiceBridge.exe` 和同目录的 Windows App SDK 自包含运行库。必须整体保留目录，不能仅复制 EXE。编译依赖缓存在 `native/.packages/`，不进入 Git。

## 运行与原版兼容

普通启动使用 `%LOCALAPPDATA%/VoiceBridge/config.json`、8765 端口和原版单实例命名对象，继承原版配对令牌、固定地址、设置和累计计数。启动前退出原版；如果原版仍运行，二次启动会请求唤醒已有实例。

隔离验收命令使用项目内测试配置、独立单实例名称和 127.0.0.1:18765，不修改日常配对或开机自启项：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File native/verify.ps1 -Launch
```

## 回归测试

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File native/verify.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File native/verify.ps1 -UI
```

协议测试对真实 C++ HTTP 服务发起回环请求，使用临时端口、项目内唯一配置及可控输入回调。UI 测试创建真实 WinUI 窗口，验证实际剪贴板粘贴、回车、两种快捷键、按钮、设置和托盘；输入目标必须是它自己的窗口，失去前台焦点时拒绝输入并报告失败。运行 UI 测试期间请保留测试窗口的焦点。测试不会修改日常配置或开机自启项，但实际粘贴验收会使用系统剪贴板。

报告和 PNG 在 `native/test-output/`。交付包附带从交付目录运行的报告与预览，位于 `verification/`。

可直接验证交付包：

```powershell
.\VoiceBridge.exe --self-test "C:\你的验收目录\protocol-tests.json"
.\VoiceBridge.exe --ui-self-test "C:\你的验收目录\ui-tests.json"
```

## 实现位置

- BridgeCore.h：设置、HTTP 服务、长轮询/确认队列、网卡选择、剪贴板、SendInput 和 UI Automation 焦点检测。
- Main.xaml：主窗口配色、字体、圆角卡片和控件布局。
- NativeApp.h：C++/WinRT 应用、主窗口交互、主题及 Composition 动画。
- SecondaryWindows.h：设置、二维码、关闭询问、托盘和窗口行为。
- NativeTests.h：原生协议和配置回归。
- UiTests.h：真实窗口、输入、快捷键、设置及托盘验收与 PNG 导出。
- Main.cpp：命令行、单实例和启动。

手机页面仍由原 `mobile.html` 提供，保持发送、回车二次确认、接收插入、复制、本地撤销和主题等全部浏览器侧功能。

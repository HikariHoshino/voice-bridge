# 声桥 Windows 原生重构

本目录为独立分支中的 C++/WinRT + WinUI 3 + Composition 实现。原 Python 文件、手机页面和图标保持基线字节，恢复点见 BASELINE.md。当前验收状态见 VALIDATION.md。

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
```

测试对真实 C++ HTTP 服务发起回环请求，使用临时端口、项目内唯一配置及可控输入回调；不会向用户当前窗口注入文字。报告位于 `native/test-output/protocol-tests.json`。该测试覆盖协议和持久化，不代表真实输入目标、手机浏览器或视觉一致性已全部验收。

## 实现位置

- BridgeCore.h：设置、HTTP 服务、长轮询/确认队列、网卡选择、剪贴板、SendInput 和 UI Automation 焦点检测。
- Main.xaml：主窗口配色、字体、圆角卡片和控件布局。
- NativeApp.h：C++/WinRT 应用、主窗口交互、主题及 Composition 动画。
- SecondaryWindows.h：设置、二维码、关闭询问、托盘和窗口行为。
- NativeTests.h：原生协议和配置回归。
- Main.cpp：命令行、单实例和启动。

手机页面仍由原 `mobile.html` 提供，保持发送、回车二次确认、接收插入、复制、本地撤销和主题等全部浏览器侧功能。

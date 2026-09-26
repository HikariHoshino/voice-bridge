# 声桥 Windows 版（WebView2 界面）

## 使用

把整个文件夹放到固定位置，双击 `VoiceBridge.exe`。需要系统自带的 Microsoft Edge WebView2 运行时（Windows 10/11 通常已安装）。沿用原来的配置文件 `%LOCALAPPDATA%/VoiceBridge/config.json`、配对令牌和 8765 端口，手机收藏的地址不用换。启动前请先退出旧版声桥。

手机和电脑连接同一局域网，点主窗口的二维码按钮，用手机浏览器扫码并收藏页面。手机发送前先点击电脑上的目标输入框。目标程序以管理员身份运行时，Windows 会阻止自动输入，文字会留在剪贴板，按 Ctrl+V 即可。

## 结构

- `Main.cpp`：Win32 窗口（去掉系统标题栏、系统圆角、最小宽度、高度动画）、WebView2 宿主、托盘、单实例、各窗口之间的消息。
- `ui/`：主窗口、设置、手机配对、关闭询问四个页面，共用 `app.css` 与 `shell.js`。
- 核心逻辑（HTTP 服务、输入注入、焦点检测、设置）直接复用 `../native/BridgeCore.h`，未修改协议。

## 构建与验证

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File web/build.ps1 -OutputName Release
powershell -NoProfile -ExecutionPolicy Bypass -File web/package.ps1 -OutputName Release -Destination <新目录>
<新目录>\VoiceBridge.exe --self-test <报告路径>.json
powershell -NoProfile -ExecutionPolicy Bypass -File web/check-close.ps1 -Executable <新目录>\VoiceBridge.exe
```

`--self-test` 对真实 HTTP 服务跑协议测试；`check-close.ps1` 用隔离配置和 18765 端口实测三种关闭行为。截图：`--screenshot <png> [--shot-page settings|qr|close] [--shot-state expanded,connected,dark]`，同样需配合 `--config` 使用隔离配置。

# 原生版验收记录

日期：2026-09-11。实现采用 C++/WinRT、WinUI 3、Microsoft.UI.Composition，x64 自包含部署。源码位于独立 worktree 的 `native/`，未替换原 Python 项目。

用户确认可用的检查点为 `checkpoint/native-usable-20260911`（`d083ece`）。原便携目录 `outputs/VoiceBridge-WinUI3-x64-20260911/` 和 ZIP 保留；检查点通过协议 32/32、真实界面 29/29。位置和哈希见 CHECKPOINTS.md。

本次仅更换标题栏和移除暂停按钮。自绘标题栏使用 WinUI XAML，最小化、最大化/还原、关闭按钮不使用系统标题栏控件。拖动区域通过 InputNonClientPointerSource 注册并排除按钮区域。主窗口及三个辅助窗口统一配色。系统窗口边框、调整大小与窗口管理行为继续由 Windows 负责。

原有内容高度、展开收起时长与缓动参数保持不变；窗口尺寸换算单独包含自绘标题栏高度。暂停按钮及其点击、图标更新代码已移除，手机输入保持启用。通信、配对、设置格式、手机页面未修改。

最终交付目录：`outputs/VoiceBridge-CustomTitleBar-x64-20260911-R2/`。直接运行此目录中的 EXE，界面测试 39/39、协议测试 32/32 全部通过。EXE SHA-256：`7F032AFDBDC60B45584E7D179DF5A59A37335629934A475AC3115659CDDC1336`。报告与七张真实窗口渲染 PNG 位于 `verification/`。标题栏拖动区域已验证注册和按钮排除范围；多显示器 DPI 切换及人工拖动/吸附尚未实测。

## 已修复的问题

- WinUI 启动失败：补齐 XAML 元数据提供器，调整资源加载顺序，避免同一 ResourceDictionary 被两个对象重复拥有。
- DPI 尺寸：按原版物理像素间距、独立字体缩放计算主窗口和卡片，修正隐藏面板留下空行的问题。
- Enter 发送失效：通过 PreviewKeyDown 在多行输入控件处理回车前执行发送；Ctrl+Enter 继续按配置发送或换行。
- 设置与配对窗口：避免使用尚未完成 WinUI 布局的 DesiredSize 决定窗口高度，恢复原版大小的开关、按钮及模态关闭询问。
- 网络地址刷新：保存新地址失败时关闭候选服务、保留旧服务；新会话保留待接收消息。
- 分发：加入 EXE 图标和版本资源，随目录复制 MSVC 与 Windows App SDK 运行库。

## 验收方法

协议报告 `verification/protocol-tests.json`：真实本机 HTTP 服务，包含认证、原手机页面、Unicode/空白/Markdown 保真、暂停、无输入焦点拒绝、回车、无效请求、100KB 限制、确认设置、会话、ACK、长轮询、50 条队列、重新配对及网络更新失败回退。协议测试的输入回调可控。

界面报告 `verification/ui-tests.json`：实际显示 WinUI 窗口，通过原生自动化按钮和真实 HTTP 请求运行；粘贴和按键使用真实剪贴板与 SendInput，并检查实际 TextBox 内容。包含展开收起、调整大小、两种发送快捷键、深浅色主题、五个设置开关、保存同步、二维码、托盘保持服务、单实例唤醒及关闭询问。测试只允许向自己的窗口输入。前台被切走会记录失败，不能当作通过。

`verification/` 中保留实际窗口渲染 PNG 与尺寸数据。144 DPI 下，检查点主窗口内容为 626 × 484 像素，连接卡和折叠传输卡均为 118 像素。新版在内容上方增加 36 DIP 的自绘标题栏，不挤占内容。字体抗锯齿、控件细节和动画的逐像素一致性尚未完成目视验收；尺寸相同不能证明视觉完全一致。

## 验收边界

本机原生窗口与通信测试可以复现；尚未在真实手机、其他电脑或多个 DPI 显示器上完成完整往返验收。开机自启的注册表路径已实现，但自动验收为保护日常环境未写入真实 Run 项。系统输入法语音识别由手机系统负责。

不能据此声称“所有机器、全部输入目标、全部视觉细节均已验证”。最终测试计数以交付目录生成的 JSON 为准，构建成功本身不算功能验收。

## 保护与恢复

原件哈希和位置见源码 `BASELINE.md`。`55b0c2e` 保存原工作区基线；`167c142` 保存已通过启动、31 项协议与 21 项真实 UI 测试的修复。后续变更只涉及 `native/`。旧交付物与原配置未清理。

## 2026-09-26 瘦身与小修（基于诊断版 `checkpoint/focus-diagnostics-20260912`）

- 交付目录：`outputs/VoiceBridge-Slim-20260926/`，60.2 MB（原 148 MB）。EXE SHA-256：`31E5C25EEC16C2C2F61F0378FB9176D307256204D5B01ACA5423DBDBBA436367`。
- `package.ps1` 不再复制 PDB/ILK、Windows AI/ML 运行库（onnxruntime、DirectML、Microsoft.Windows.AI/Workloads/Vision 等）和除 zh-CN、en-us 外的语言目录。
- 剪贴板写入附加 `CanIncludeInClipboardHistory=0`、`CanUploadToCloudClipboard=0`：文字仍留在剪贴板，不进 Win+V 历史和云剪贴板。已在真实剪贴板上确认两种格式存在。
- `/receive` 同时挂起上限 8 个，超出返回 503，手机页面按原有逻辑 1.5 秒后重试；保证 `/send`、`/enter` 始终有空闲线程。
- 窗口标题改为"声桥"；单实例唤醒由 100ms 定时器轮询改为 `RegisterWaitForSingleObject`。
- 从交付目录运行：协议测试 38/38（新增 3 项长轮询上限测试），真实界面测试 59/59。
- 未改：暂停接收的残留代码（`accepting` 与 423 分支及其测试）仍保留，待用户决定。

## 2026-09-26 移除暂停接收残留（用户确认不再需要）

- 删除 `accepting` 标志、423 分支及两项暂停测试；暂停按钮此前已移除。
- 交付目录：`outputs/VoiceBridge-NoPause-20260926/`，EXE SHA-256：`A77C77CAAE37A21402A4ECB444BE84A9BCF41AD99DCBC0E7AFEFFB640017E9C9`。
- 从交付目录运行协议测试 36/36（38 项减去两项暂停测试）。真实界面测试已运行，结果未读取；用户决定以日常使用为准。

## 2026-09-26 Chromium 类程序的输入焦点识别

依据 `%LOCALAPPDATA%/VoiceBridge/diagnostics` 日志：
- claude.exe（Electron）首次发送时 UIA 只返回窗口外壳 Pane（View，无键盘焦点），约 1 秒后才出现 ProseMirror 输入框，4 次首发失败均属此类。现改为：遇到这种外壳最多等待 1 秒、每 50ms 重查。
- HrASMain.exe（Chrome_WidgetWin_0，CEF）完全不提供无障碍树，焦点只落在外壳 Pane 上（有键盘焦点），12 次全部被拒。现改为：外壳本身持有键盘焦点时直接粘贴，与旧版行为一致。
- 按钮、非 Chromium 窗口仍按原有严格规则判断。
- 交付目录：`outputs/VoiceBridge-ChromiumFocus-20260926/`，EXE SHA-256：`D92B7AABAD1430FB5D8212BD65AA736428C3043DB4470FB96C55212503B7A940`。协议测试 39/39（新增 3 项判断规则测试）。未在 claude.exe / HrASMain.exe 上做实机验证，以日常使用为准。

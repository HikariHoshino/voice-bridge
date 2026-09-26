# StatusCopyFix 输入诊断版

基线目录：D:/Coding/Codex/Voice Bridge/outputs/VoiceBridge-StatusCopyFix-20260911。
原 EXE SHA-256：D46C13A82EEC62C991DBCB5F0D83096A5692775BCED2D52E23207B2679313D52。
匹配的输入核心基线：baseline/StatusCopyFix-BridgeCore.h，SHA-256 为 679C3A5BBDD9E8440E013C5EBA57C99A6477EE12B1F4814220D20BDA9CD6012C，与先前调查记录一致。
工作副本：source/。交付副本：../VoiceBridge-StatusCopyFix-Diagnostic-20260912/。
没有将另一个版本的四次重试和宽松控件判定合入；原目录与正在运行版本的文件均保持不变。

## 使用与日志

退出旧版声桥，再启动交付副本里的 VoiceBridge.exe。沿用原设置。
日志位置：%LOCALAPPDATA%/VoiceBridge/diagnostics/focus-*.jsonl，每次启动独立文件，首次输入请求时创建。
复现后提供该次运行的日志，以及大致发生时间和软件名称。成功与失败都保留，便于对比。
不自动删除旧日志。JSONL 为 UTF-8，每行一个操作记录，operation_id 和 parent_operation_id 关联请求与输入。

记录：时间、耗时、版本、线程、应用进程与 EXE 名称及提权状态、Win32 焦点/光标句柄和位置、控件类名、UIA 框架/类型/焦点/可用/屏幕外/密码标志、被实际查询的 Value/Text 接口 HRESULT、只读和 ARIA 角色、RuntimeId、具体拒绝分支、初检/复查/剪贴板/按键阶段、SendInput 返回数量和错误、HTTP 状态。
未执行的检测路径不会额外执行，缺少的字段不代表 false；属性读取失败需结合对应 HRESULT 判断。
不读取窗口标题、控件 Name/Value、正文、选中文字、剪贴板内容、令牌或请求 URL。
completed 仅表示该函数没有抛出异常；SendInput 数量完整也不证明目标应用实际接收了文字。
日志缓存在内存中，操作结束再写入；额外只读属性查询仍可能轻微影响时序，不能保证不影响偶发故障频率。无主动重试、抢焦点或试输。
目录不可写时不改变输入行为，输出调试错误；若没有新日志，需要先检查日志目录是否可写。

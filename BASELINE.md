# 浏览器版重启基线

- 用户指定原件：`../voice-bridge/dist-final/VoiceBridge/VoiceBridge.exe`
- 原件 SHA-256：`A22424B2656E0E65AA74FE0C6961B712D96C06BBA5ECECA2AD8D97E9546A3506`
- 工作副本来源：`../../backups/voice-bridge/20260812-v1-windows-web-baseline/source/`
- 建立日期：2026-08-26（Asia/Shanghai）

指定 EXE 和既有备份保持不修改。本目录只维护 Windows＋浏览器版本；不包含 Android 原生端或对等协议。

- 本次固定地址改动前恢复点：`../../backups/voice-bridge/20260826-before-pinned-address/`
- 本次工作副本：当前目录；固定地址发布包位于 `release-pinned-address-20260826/`
- 本次后台唤醒修复前恢复点：`../../backups/voice-bridge/20260901-before-activation-fix/`

## 2026-09-10 双端确认与输入焦点修复

- 用户指定原件目录：`C:/Users/ACER/Documents/Codex/2026-07-20/wo-x/outputs/voice-bridge-web-restart/release-network-stable-20260826/VoiceBridge-Windows-browser-network-stable-20260826/`
- 原件 ZIP SHA-256：`FCDDF3129D736DBA79CD26762555B9053CA1440FA8305C4F515A8FD296FDC8CA`
- 原件 EXE SHA-256：`8EE8379A12786D2BE2C8E18CD8E71CA245130F213573BDD14250A61DBFCB32DF`
- 原件内 `mobile.html` SHA-256：`22562541D44EE393609856D9984861E6968BC44FE6460F7F7549B44DC2214243`
- 修改前工作副本 `mobile.html` 与原件内文件哈希一致。
- 本次工作副本：当前目录。
- 修改前源码恢复点：Git 提交 `aca8783`（`windows-activation-fix-20260901`）。

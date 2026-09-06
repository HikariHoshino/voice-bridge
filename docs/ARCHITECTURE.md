# 架构与数据流

## 节点模型

声桥 2.0 不再要求电脑充当中心服务器。每个 Android 或 Windows 实例都是独立节点：监听本机随机 TLS 端口，通过 `_voicebridge._tcp.local` mDNS 和 `224.0.0.167:53318` 受限组播广播自动发现同一局域网节点。发现报文不含正文或密钥。

- `android/`：Android 12+ Kotlin/Compose 应用、前台服务、发现、协议和无障碍注入。
- `desktop_peer.py`：Windows 对等身份、发现、TLS 服务、配对、心跳和消息回执。
- `app.py`：原有 Windows UI、旧网页服务和 Windows 输入层；以最小接线使用对等模块。
- `mobile.html`：旧 HTTP 浏览器兼容入口，不参与 Android↔Android 对等传输。

## 身份与配对

1. 节点生成长期 P-256 身份和自签名证书；Android 私钥保存在 Keystore，Windows 私钥使用 DPAPI。
2. 发起端通过 TLS 连接目标端，并把自己的证书与 ECDSA 请求签名放入协议帧。
3. 两端根据双方证书指纹和随机 nonce 计算同一组六位短码，各自确认。
4. 配对完成后保存目标证书指纹和独立 256 位共享密钥；密钥不进入发现报文或 URL。

## 消息与状态

- `PAIR_REQUEST/PAIR_RESULT`：一次性双端确认。
- `PING/PONG`：五秒心跳；连续三次失败后显示“断开，正在重试”。
- `TEXT_PUSH/TEXT_RESULT`：原始 UTF-8 正文与逐消息结果。
- 每条认证消息包含 UUID、HMAC-SHA256 和客户端证书签名；接收端维护 256 条防重放窗口。
- 发送端只有收到 `injected` 才清空输入框；`no_focus`、`accessibility_off`、`password_field`、`unsupported_editor`、`paused` 等结果均保留原文。

连接状态只反映已认证心跳，不再根据旧网页长轮询是否刚成功进行推测。

## Android 光标注入

无障碍服务读取当前获得输入焦点的可编辑节点、原始 UTF-16 文本和选区，在选区处拼接新文字后调用 `ACTION_SET_TEXT`，最后恢复新光标。无法可靠读取选区、目标为密码框或控件拒绝写入时立即失败，不排队、不碰剪贴板、不切换输入法。

正文不写入磁盘。Android 输入框只在内存和 Compose 状态恢复范围内存在；节点协议不保存消息历史。

## 旧网页兼容

原有 `/send`、`/enter`、`/receive` 和 `/settings` 接口继续存在，使用查询令牌和 HTTP。它与对等 TLS 协议隔离，仅用于暂未安装 Android 应用的浏览器。风险见 [SECURITY.md](../SECURITY.md)。

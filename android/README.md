# 声桥 Android

Android 12+ 原生节点。应用不包含语音识别、不实现输入法服务、不使用剪贴板；用户可以继续使用自己信任的输入法在声桥文本框中输入。

## 构建

```powershell
$env:JAVA_HOME = "D:\Java\jdk-21.0.10.7-hotspot"
.\gradlew.bat testDebugUnitTest lintDebug assembleDebug
```

调试 APK 位于 `app/build/outputs/apk/debug/app-debug.apk`。

## 使用权限

- 通知：显示 Android 要求的常驻接收前台服务。
- 网络/Wi-Fi 多播：发现并连接同一局域网中的声桥节点。
- 无障碍：仅在已配对消息到达时读取当前普通编辑框文本和选区并写回；密码框被拒绝。

没有联系人、短信、麦克风、存储、剪贴板或输入法权限。

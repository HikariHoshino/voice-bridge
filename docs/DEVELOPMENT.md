# 开发、测试与打包

## 环境

- Windows 10 或 Windows 11
- Python 3.13
- Git

程序使用 Windows 专用 API，测试和构建应在 Windows 上运行。

## 安装

```powershell
py -3.13 -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install -r requirements-dev.txt
```

## 启动源码

```powershell
python app.py
```

启动后运行时配置写入 `%LOCALAPPDATA%\VoiceBridge`，不会写入仓库。

## 自动化测试

```powershell
python -m unittest -v
```

重点回归范围：

- 未配对请求被拒绝；
- 暂停接收时文字和回车均被拒绝；
- 手机与电脑双向消息；
- 配对令牌与设置持久化；
- 回车只发送一组按下和抬起事件；
- 换行、连续空格、制表符、Markdown、emoji 和生僻 Unicode 原样保留；
- 单实例、高 DPI、标题栏颜色和开机自启命令。

GitHub Actions 使用 `windows-latest` 和 Python 3.13 运行同一组测试。

## 打包

```powershell
python -m PyInstaller --noconfirm --clean VoiceBridge.spec
```

成功后产物位于：

```text
dist\VoiceBridge\
├── VoiceBridge.exe
└── _internal\
```

发布时必须整体分发 `VoiceBridge` 文件夹，不能只复制可执行文件。文件夹模式用于缩短冷启动时间并保持单一应用进程。

## 发布检查

1. 自动化测试全部通过。
2. 打包命令成功完成。
3. 从全新目录运行完整便携文件夹。
4. 验证浅色和深色模式、150% DPI、托盘和单实例。
5. 验证手机发送包含多级 Markdown、空行、连续空格、Tab、emoji 的长文本。
6. 验证电脑到手机、远程回车、暂停接收和重新配对。
7. 确认提交中不包含 `%LOCALAPPDATA%` 配置、配对二维码、配对 URL 或构建目录。

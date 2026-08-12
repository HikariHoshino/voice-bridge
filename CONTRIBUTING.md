# 参与开发

声桥目前是个人使用的小工具。提交修改时请坚持最小改动，避免顺手重构无关代码。

## 修改前

1. 阅读 [架构说明](docs/ARCHITECTURE.md) 和 [安全说明](SECURITY.md)。
2. 新建独立分支。
3. 确认修改不会把运行时配置、配对链接、构建目录或打包产物提交到仓库。

## 提交要求

- 每个提交只处理一个明确问题。
- 修复行为缺陷时补充能够复现该缺陷的测试。
- 文本传输相关修改必须覆盖换行、连续空格、制表符、Markdown 列表、emoji 和生僻 Unicode。
- 不要改变剪贴板行为、注入方式或安全边界，除非对应需求明确要求。

## 验证

```powershell
python -m unittest -v
python -m PyInstaller --noconfirm --clean VoiceBridge.spec
```

测试和打包均成功后，再提交变更。

# 固定依赖

| 组件 | 版本 | 来源 | 许可 |
|---|---|---|---|
| Windows App SDK | 1.8.260804001 | Microsoft NuGet | 包内 license.txt |
| C++/WinRT | 2.0.250303.1 | Microsoft NuGet | 包内许可 |
| cpp-httplib | 0.22.0 | https://github.com/yhirose/cpp-httplib/tree/v0.22.0 | vendor/httplib.LICENSE |
| JSON for Modern C++ | 3.12.0 | https://github.com/nlohmann/json/tree/v3.12.0 | vendor/json.LICENSE.MIT |
| QR Code generator | 1.8.0 | https://github.com/nayuki/QR-Code-generator/tree/v1.8.0 | 源文件头部 MIT 许可 |

下载源码 SHA-256：

- httplib.h: F81548031DB8543A46170431AFAC1D2D9B305FC52CFD356A672DE6688C2BF697
- json.hpp: AAF127C04CB31C406E5B04A63F1AE89369FCCDE6D8FA7CDDA1ED4F32DFC5DE63
- qrcodegen.cpp: 1F3B3FCDAC6954C32CF583CCD02EC9B5901F756A38C461ACEDC70BE4A77D3757
- qrcodegen.hpp: B779C3B156CF7A57CE789D6FEE4FC991CCC2913774D26C909D22BB8F26B2A793

运行时无 Python、Tk、WebView 桌面壳或外部云服务。Microsoft SDK 的自包含文件由官方构建目标收集，其中有 SDK 自带但本应用未调用的组件。

# 版本说明书

## 版本配套说明

本文说明`v3.0.0`的配套环境、交付文件和主要变更，并在“开发分支后续变更”中单列尚未包含在该版本中的修改。正式发布信息见[CinderX Releases](https://gitcode.com/openeuler/cinderx/releases)。构建和用户指南按本页列出的开发基线编写；使用发布包时，应同时核对对应版本的要求。

| 产品 | 版本信息 |
|---|---|
| 产品名称 | CinderX鲲鹏／ARM优化版 |
| 正式版本 | `v3.x.x` |
| 发布日期 | 2026-11-10 |

发布Tag与Python包版本采用不同规则。源码构建默认生成日期型版本`YYYY.MM.DD.PP`：日期可由`SOURCE_DATE_EPOCH`固定，末段补丁号由`CINDERX_VERSION_PATCH`指定；从源码分发包构建时则沿用其包元数据版本。因此`v3.0.0`安装后查询到的包版本是`2026.11.10.0`。

## 软件版本配套信息

本节分别列出CPython 3.11和3.14的制品及配套环境。CP311提供wheel和RPM两种安装方式，CP314使用wheel；两条Python支持线不能互换安装包，均面向Linux AArch64。

### CPython 3.11

| 项目 | 配套信息 |
|---|---|
| Python | CPython 3.11.6 |
| 操作系统 | openEuler 24.03 LTS-SP3 AArch64，Python环境按下列方式二选一 |
| 系统Python方式 | `python3-3.11.6-34.oe2403sp3`，可创建venv；源码构建使用同NVR的`python3-devel` |
| Conda方式 | CPython `3.11.6`，用于安装wheel；系统仍需满足上述配套环境和glibc要求 |
| wheel | `cinderx-2026.11.10.0-cp311-cp311-linux_aarch64.whl` |
| 校验文件 | 同名wheel加`.sha256`后缀 |
| 启动方式 | 插件开关、`CINDERX_EVAL_MODE=cinder`、`CINDERX_JIT_MODE=execute`和数值阈值 |
| 能力限制 | 不支持`auto`／`auto:N`行为分类、OSR和轻量级帧执行 |

该wheel为普通Linux wheel，依赖glibc 2.38及以上。Python环境可按安装指南选择系统Python＋venv或Conda。系统动态库要求与所选Python环境分别检查。

### CPython 3.11 RPM

| 项目 | 配套信息 |
|---|---|
| Python | 发行版提供的非调试CPython 3.11.6 |
| 操作系统与架构 | openEuler 24.03 LTS-SP5，AArch64／ARM64 |
| RPM包名 | `python3-cinderx` |
| RPM文件 | `python3-cinderx-2026.11.10.0-1.aarch64.rpm` |
| Python包版本 | `2026.11.10.0` |
| RPM发行号 | `1` |
| 获取方式 | 发布方提供的RPM软件源，地址待提供；也可安装取得的本地RPM |
| 本地文件校验 | 同名RPM加`.sha256`后缀 |
| 安装位置 | 系统`/usr/lib64/python3.11/site-packages` |
| 使用的解释器 | `/usr/bin/python3.11`；普通venv默认不包含系统RPM |
| 启动方式与能力限制 | 与上述CP311 wheel相同 |

RPM的版本由Python包版本和打包发行号组成，`2026.11.10.0-1`对应Python包版本`2026.11.10.0`、RPM发行号`1`。RPM安装与卸载由`dnf`管理，步骤见[安装部署指南](installation_guide.md#cpython-311-rpm)。

### CPython 3.14

| 项目 | 配套信息 |
|---|---|
| Python | CPython 3.14.0—3.14.3，推荐3.14.3；可使用已有Python＋venv或Conda |
| 操作系统 | Linux AArch64，推荐openEuler 24.03 LTS-SP3；需满足wheel的平台标签和动态库依赖要求 |
| wheel | `cinderx-2026.11.10.0-cp314-cp314-manylinux_2_24_aarch64.manylinux_2_28_aarch64.whl` |
| 校验文件 | 同名wheel加`.sha256`后缀 |
| 包形态 | fat wheel，包含四个补丁版本对应的本地扩展及加载器 |
| 能力限制 | CPython 3.14.0的直接调用路径绕过AutoJIT分类准入；OSR与轻量级帧互斥 |

这里的补丁版本（micro）指3.14.0、3.14.1等版本。加载器只选择包内已经提供的扩展，因此该wheel不支持3.14.4及后续版本。Python 3.15、自由线程版本（free-threaded Python）及表外构建形态不在本次配套范围内。

## 版本更新说明

### 更新说明

以下摘要整理自正式Release的变更记录，便于用户了解升级影响；完整提交列表见发布页。

- 完成CPython 3.11 JIT性能优化

- 完成CPython 3.11.6属性访问路径优化

### 已解决的问题

无。

### 遗留问题与使用限制

无。

## 版本配套文档

| 文档名称 | 内容简介 |
|---|---|
| [快速入门](quick_start.md) | 安装并运行最小JIT示例，检查结果和编译状态 |
| [编译构建指南](compilation_build_guide.md) | 双线开发环境、源码构建和发布打包 |
| [安装部署指南](installation_guide.md) | 选包、校验、安装、升级、回退和卸载 |
| [用户指南](user_guide.md) | 功能、原理、场景、配置、运行维护和故障处理 |

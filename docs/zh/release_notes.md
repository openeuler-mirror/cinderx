# 版本说明书

## 版本配套说明

本文说明`v3.0.0`的配套环境、交付文件和主要变更，并在“开发分支后续变更”中单列尚未包含在该版本中的修改。正式发布信息见[CinderX Releases](https://gitcode.com/openeuler/cinderx/releases)。构建和用户指南按本页列出的开发基线编写；使用发布包时，应同时核对对应版本的要求。

| 产品 | 版本信息 |
|---|---|
| 产品名称 | CinderX鲲鹏／ARM优化版 |
| 正式版本 | `v3.0.0` |
| 发布日期 | 2026-11-10 |
| 发布源码提交 | `cebb4ca7c5c46e9d9e034a9da4d7222bb6292724` |
| wheel包版本 | `2026.11.10.0` |
| CP311 RPM包版本 | `2026.11.10.0-1` |
| 发布前一版本 | `v2.2.3` |
| 资料核对日期 | 2026-11-13 |

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

### v3.0.0更新说明

以下摘要整理自正式Release的变更记录，便于用户了解升级影响；完整提交列表见发布页。

- 扩展CP311 JIT的调用和参数绑定、属性／方法缓存、生成器及异常处理等执行路径。
- 优化CP311整数、浮点、下标访问、循环数值和HIR／代码布局等路径。实际收益随程序和配置变化，发布摘要不提供统一加速倍数。
- 将面向CPython 3.11的CinderX构建流程的PGO和LTO默认值设为开启，PGO使用仓库训练场景。具体制品的参数以构建记录为准。
- 改进AutoJIT的循环感知IC稳定性检查。

### v3.0.0已解决的问题

- 修复CP311 quickened-code计数、tracing回调入口保护，以及异常边界的状态处理。
- 修复生成器运行／挂起状态、委托与finalizer处理的若干问题。
- 修复引用计数、内联目标生命周期、并行编译期间类型监视注销等正确性问题。
- 修复代码补丁标志初始化、过大编译输入处理和LIR／寄存器分配确定性等问题。

上述修复和优化均有各自的适用条件。JIT只编译其支持的函数形态，其他形态可能继续解释执行。

### 开发分支后续变更

本次资料所核对的开发基线为`7d0f10e62dfa3cbc1211b091de011ee6bea8c40c`。相对`v3.0.0`，主要后续变更包括：

- `!285`：CP311在import／setup作用域内跳过帧观察器。
- `!286`：降低CP311参数绑定与共享编译入口开销。
- `!289`：修正CP311 refcycle引用计数验收，并保留生命周期检查。

上述变更需要从对应开发提交构建，未包含在本页列出的`v3.0.0` wheel中。测试结果和性能数据也应与实际测试的提交及构建配置对应。

### 遗留问题与使用限制

- OSR为CP314实验能力，默认关闭；不能与轻量级帧同时开启。
- CP311设置轻量级帧开关后仍使用普通帧，不能据此认为轻量级帧已生效。
- CP311不支持AutoJIT行为分类；CP314的分类准入也可能延后或拒绝某个函数，不能把调用次数等同于必然编译。
- 依赖frame、trace／profile、异常栈或其他运行时内部行为的应用，应验证其业务路径。
- 已知问题的后续状态以所选Release说明、相关Issue和当前代码为准。

## 版本配套文档

| 文档名称 | 内容简介 |
|---|---|
| [快速入门](quick_start.md) | 安装并运行最小JIT示例，检查结果和编译状态 |
| [编译构建指南](compilation_build_guide.md) | 双线开发环境、源码构建和发布打包 |
| [安装部署指南](installation_guide.md) | 选包、校验、安装、升级、回退和卸载 |
| [用户指南](user_guide.md) | 功能、原理、场景、配置、运行维护和故障处理 |

## 修订记录

| 日期 | 修改内容 | 变更原因 |
|---|---|---|
| 2026-11-12 | 增加五类中文资料，列出v3.0.0制品及开发分支后续变更 | 补齐用户资料并明确CP311／CP314使用边界 |
| 2026-11-13 | 补充CP311 RPM的openEuler 24.03 LTS-SP5配套、获取、部署和使用说明 | 增加RPM安装方式及系统解释器使用步骤 |
| 2026-11-13 | 增加wheel命令行取包与两种Python环境准备方式，调整快速入门顺序 | 便于按步骤安装、创建示例和运行 |

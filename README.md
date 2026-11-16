# CinderX

> English upstream documentation: [README_Opensource.md](README_Opensource.md)

## 简介

CinderX是Python运行时扩展，通过即时编译（JIT）将受支持函数的Python字节码转换为本机机器码。本项目基于[Meta开源的CinderX](https://github.com/facebookincubator/cinderx)，面向鲲鹏/ARM平台开展适配和性能优化，并提供配套安装包、测试工具和中文使用资料。

项目提供CPython 3.11.6和CPython 3.14两条使用路径，配套系统、架构和补丁版本要求见下文。JIT的实际收益取决于业务热点、运行配置和编译支持范围，应结合目标业务验证。首次使用建议从[快速入门](docs/zh/quick_start.md)开始。

## 版本说明

本页以正式版本[v3.0.0](https://gitcode.com/openeuler/cinderx/releases)为安装示例，Python包版本为`2026.11.10.0`。Release Tag与Python包版本采用不同规则；正式发布内容和开发分支后续变更见[版本说明书](docs/zh/release_notes.md)。

| 安装包 | Python版本 | 配套环境与版本 |
|---|---|---|
| CP311 wheel | CPython 3.11.6 | openEuler 24.03 LTS-SP3、AArch64，glibc 2.38及以上；系统Python可配合venv，也可选择匹配的Conda环境 |
| CP311 RPM | 发行版CPython 3.11.6 | openEuler 24.03 LTS-SP5、AArch64，包名`python3-cinderx`，RPM版本`2026.11.10.0-1` |
| CP314 fat wheel | CPython 3.14.0—3.14.3，推荐3.14.3 | Linux AArch64，推荐openEuler 24.03 LTS-SP3；需满足wheel平台标签和动态库依赖要求 |

当前发布的预编译包面向AArch64（ARM64），未提供x86_64预编译包。CP314 fat wheel按补丁版本选择包内的本地扩展，不支持3.14.4及后续版本。两条Python支持线不能互换安装包，自由线程版本不在本次配套范围内。

wheel及校验文件从[CinderX Releases](https://gitcode.com/openeuler/cinderx/releases)获取。CP311 RPM由发布方提供软件源或本地文件，源地址待提供，部署步骤见[安装部署指南](docs/zh/installation_guide.md#cpython-311-rpm)。

## 特性介绍

| 特性 | CPython 3.11 | CPython 3.14 |
|---|---|---|
| 自动导入 | 支持；执行JIT还需配置帧求值器和execute模式 | 支持；按下文配置JIT |
| 按调用次数自动JIT | 支持数值阈值 | 支持数值阈值 |
| AutoJIT行为分类（`auto`／`auto:N`） | 不支持 | 支持；推荐使用3.14.3评估 |
| OSR | 不支持 | 实验特性，默认关闭，要求普通帧 |
| 轻量级帧执行 | 不支持，请求后仍使用普通帧 | 取决于构建能力，不能与OSR同时开启 |

### 自动导入

设置`CINDERX_PLUGIN_ENABLE=1`后，Python启动时通过site钩子自动加载CinderX。自动加载与业务函数进入JIT执行是不同状态，还需按Python版本配置运行模式，并检查目标函数是否编译。

CP311使用CinderX帧求值器、execute模式和数值调用阈值：

```bash
CINDERX_PLUGIN_ENABLE=1 CINDERX_EVAL_MODE=cinder \
  CINDERX_JIT_MODE=execute PYTHONJITAUTO=20 \
  CINDERX_OSR_ENABLED=0 PYTHONJITLIGHTWEIGHTFRAME=0 \
  python3.11 your_app.py
```

CP314可使用AutoJIT行为分类；以下示例显式选择普通帧并关闭OSR：

```bash
CINDERX_PLUGIN_ENABLE=1 PYTHONJITAUTO=auto:2 \
  CINDERX_OSR_ENABLED=0 PYTHONJITLIGHTWEIGHTFRAME=0 \
  python3.14 your_app.py
```

`python3.11`、`python3.14`应来自实际安装CinderX的环境；`your_app.py`替换为业务入口。完整示例及结果检查见[快速入门](docs/zh/quick_start.md)。

### JIT编译器

**作用**：将受支持函数的Python字节码即时编译为本机机器码，实际收益取决于业务热点和运行配置。JIT会自动追踪函数调用频率，对频繁调用的函数进行编译优化。

**开启方式**：

CP314可使用下列环境变量控制JIT；CP311使用数值调用阈值和本页的完整启动配置，其接口与参数限制见[用户指南](docs/zh/user_guide.md#cpython-311)：

| 环境变量 | 作用 |
|---|---|
| `PYTHONJITAUTO=N` | 启用自动JIT模式，达到N次调用阈值后尝试编译 |
| `PYTHONJITAUTO=auto` | 启用AutoJIT行为分类，默认按2次调用阈值开始准入判断 |
| `PYTHONJITAUTO=auto:N` | 启用AutoJIT行为分类，并以N次调用作为初始准入阈值 |
| `PYTHONJITALL=1` | 请求在函数首次调用时尽早编译，仍受支持范围约束 |
| `PYTHONJITDISABLE=1` | 禁用JIT |
| `PYTHONJITLISTFILE=/path/to/list` | 通过JIT列表文件选择性编译指定函数 |

#### AutoJIT行为分类

普通`PYTHONJITAUTO=N`只看调用次数：函数被调用到阈值后就尝试进入JIT。低阈值能更早编译热点函数，但在启动、导入、脚本初始化等阶段也容易把大量只执行一两次的函数送进JIT，造成编译风暴。

`PYTHONJITAUTO=auto:N`在调用次数之外增加一层行为分类。CinderX会先根据字节码形状、启动/导入/setup阶段信号和运行后的deopt反馈判断函数是否值得立即编译，再决定是放行、延后还是保持解释执行。它的目标是保留数值循环、稳定热点等高收益函数，同时抑制启动链路、导入链路、异常慢路径和低ROI helper的JIT成本。

推荐在需要低阈值自动JIT的场景优先使用分类模式：

```bash
CINDERX_PLUGIN_ENABLE=1 PYTHONJITAUTO=auto:2 python3.14 your_app.py
```

分类模式下，import provider、setup wrapper和动态ROI backoff默认生效。setup wrapper当前覆盖`lib2to3.main`和`multiprocessing.pool.Pool`两类一次性初始化/进程池通信窗口。当前`StartupInit`策略由import provider打开，setup wrapper作为同一启动/初始化窗口的附加信号；关闭import provider后，不能把setup wrapper单独当作setup-only策略来评估。需要做A/B验证或临时回退时，可以使用以下开关：

| 环境变量 | 作用 |
|---|---|
| `CINDERX_AUTOJIT_IMPORT_PROVIDER=off` | 关闭import阶段信号 |
| `CINDERX_AUTOJIT_SETUP_PROVIDER=off` | 关闭`lib2to3`和multiprocessing setup wrapper；需在import provider开启时评估其增量 |
| `CINDERX_AUTOJIT_SETUP_PROVIDER=lib2to3_main,multiprocessing_pool` | 显式指定setup provider组合 |
| `CINDERX_AUTOJIT_ROI_BACKOFF=0` | 关闭运行时负ROI回退 |
| `CINDERX_AUTOJIT_GATE_STATS=1` | 进程退出时输出AutoJIT准入统计，便于分析分类效果 |

更完整的使用、测试和诊断方法见[AutoJIT行为分类使用指南](cinderx/Docs/README_CN.md#autojit-行为分类使用指南)。

在CP314中，先按本页启动配置初始化JIT，也可以通过以下API控制编译与调度。`your_function`替换为业务函数；`auto()`与`compile_after_n_calls(N)`设置传统调用阈值，不开启行为分类：

```python
import cinderx.jit

# 自动模式，自动追踪并编译热点函数
cinderx.jit.auto()

# 指定调用次数阈值，函数被调用 N 次后自动编译
cinderx.jit.compile_after_n_calls(10)

# 手动立即编译某个函数
cinderx.jit.force_compile(your_function)

# 标记某个函数在下次被调用时编译
cinderx.jit.lazy_compile(your_function)

# 暂停 JIT（用于调试或特定场景）
with cinderx.jit.pause():
    ...

# 禁用 JIT
cinderx.jit.disable()

# 重新启用 JIT
cinderx.jit.enable()
```

### OSR（栈上替换，实验特性）

OSR（On-Stack Replacement，栈上替换）用于优化单次调用内运行很久的热循环。普通自动JIT主要在函数调用边界按调用次数触发编译；OSR会在解释器执行循环回边时计数，达到阈值后尝试编译包含OSR入口的版本，并从当前循环头直接切入JIT代码。

OSR目前是实验特性，默认关闭，建议仅在性能验证或受控场景中开启。OSR依赖JIT已完成初始化；如果设置了`PYTHONJITDISABLE=1`，OSR不会单独生效。

**环境变量开关**：

| 环境变量 | 作用 |
|---|---|
| `CINDERX_OSR_ENABLED=1` | 开启OSR热循环检测 |
| `CINDERX_OSR_ENABLED=0`或不设置 | 关闭OSR（默认） |
| `CINDERX_OSR_BACKEDGE_THRESHOLD=N` | 单条循环回边执行N次后尝试OSR，默认值为2000；`N`建议设置为1到2147483647的整数 |

**触发示例**：

```python
# osr_demo.py
def hot_loop(n):
    total = 0
    i = 0
    while i < n:
        total += i
        i += 1
    return total


print(hot_loop(20_000))
```

```bash
CINDERX_PLUGIN_ENABLE=1 \
PYTHONJITAUTO=10 \
PYTHONJITLIGHTWEIGHTFRAME=0 \
CINDERX_OSR_ENABLED=1 \
CINDERX_OSR_BACKEDGE_THRESHOLD=100 \
PYTHONJITDUMPHIR=1 \
python3.14 osr_demo.py
```

上述示例只调用`hot_loop`一次，但循环回边会超过OSR阈值并触发OSR尝试。配合`PYTHONJITDUMPHIR=1`可在HIR日志中观察是否生成`OSREntry`。

### 轻量级帧（Lightweight Frames）

轻量级帧用于减少部分JIT帧维护与物化开销，需要时再创建完整Python帧对象。依赖`sys._getframe()`、trace／profile回调和异常栈的业务应验证相应路径。

CP314的帧模式按以下规则选择；若同时设置对应的`-X`选项，以`-X`选项为准：

| 帧开关 | 作用 |
|---|---|
| `PYTHONJITLIGHTWEIGHTFRAME=1` | 请求轻量级帧，要求构建包含该能力，且不能同时开启OSR |
| `PYTHONJITLIGHTWEIGHTFRAME=0` | 显式使用普通帧 |
| 未设置 | 按构建能力选择：包含该能力时默认轻量级帧，否则默认普通帧 |

```bash
CINDERX_PLUGIN_ENABLE=1 PYTHONJITAUTO=2 \
  PYTHONJITLIGHTWEIGHTFRAME=1 CINDERX_OSR_ENABLED=0 \
  python3.14 your_app.py
```

构建时需要启用`ENABLE_LIGHTWEIGHT_FRAMES`；CP314的aarch64/arm64构建默认包含该能力，实际制品仍以构建配置为准。在CP314中，显式请求轻量级帧但构建不包含该能力时，JIT初始化会失败；未设置帧开关且构建不包含该能力时，默认使用普通帧。CP311始终使用普通帧，即使设置`PYTHONJITLIGHTWEIGHTFRAME=1`也会回退，不能据此认为轻量级帧已经生效。

`cinderx.is_lightweight_frames_enabled()`只查询构建是否包含能力。JIT初始化成功后，使用`cinderjit.jit_frame_mode()`查询实际配置：`0`为普通帧，`1`为轻量级帧。构建能力查询返回`True`不代表运行时已经选择轻量级帧。

> **注意**：OSR与轻量级帧互斥。当前OSR只支持普通帧模式（`FrameMode::kNormal`）；当前帧模式为轻量级帧时，即使它来自未设置开关时的默认选择，开启OSR也会导致JIT初始化失败。使用OSR时应显式设置`PYTHONJITLIGHTWEIGHTFRAME=0`。

### 其他特性

上游还提供以下能力，其可用性取决于Python版本、平台和构建条件。使用前请核对目标环境，详细说明见[上游开发资料](cinderx/Docs/README.md)及对应子目录：

- **Static Python（静态Python）**：利用类型标注在编译期生成更高效的字节码，为JIT提供更多类型信息和优化机会。通过`import __static__`开启。
- **Strict Modules（严格模块）**：冻结模块类型与内容为不可变，消除常见开发错误。通过`import __strict__`开启。
- **并行垃圾回收（Parallel GC）**：多线程并行GC，减少暂停时间。通过`cinderx.enable_parallel_gc()`或`PARALLEL_GC_ENABLED=1`开启。
- **缓存属性（Cached Properties）**：高性能`cached_property` / `async_cached_property`实现，通过`from cinderx import cached_property`使用。
- **对象不朽化（Immortalize）**：标记对象永不被GC回收，通过`cinderx.immortalize_heap()`开启。
- **自定义帧求值器（Frame Evaluator）**：替换CPython解释器循环以支持Static Python字节码，通过`cinderx.install_frame_evaluator()`开启。
- **JIT列表（JIT List）**：通过外部文件精确控制JIT编译范围，通过`PYTHONJITLISTFILE=/path/to/list`开启。
- **JIT预加载（Preloading）**：编译前预先解析全局变量和类型描述符，为JIT内置步骤，通过`PYTHONJITPRELOADDEPENDENTLIMIT`等调整。
- **调试与性能分析**：提供`PYTHONJITDEBUG`、`PYTHONJITDUMPHIR`、`PYTHONJITDUMPASM`等环境变量用于JIT调试和汇编输出，详情参见[cinderx/Docs/README_CN.md](cinderx/Docs/README_CN.md#调试与性能分析)。

## 环境部署

### 环境与安装方式选择

先按[版本说明](#版本说明)选择Python、系统和安装包。普通用户推荐使用已有Python配合venv；Conda为可选方式，仍需满足系统动态库和包的配套要求。安装前确认解释器版本与架构，例如：

```bash
python3.14 - <<'PY'
import platform
import sys

print(sys.version)
print(platform.platform())
print(platform.machine())
PY
```

wheel安装到所选Python环境；CP311 RPM通过系统包管理器安装，使用发行版Python。普通venv默认不能访问系统RPM，需要复用时按[CP311 RPM部署步骤](docs/zh/installation_guide.md#cpython-311-rpm)创建共享系统包环境，并检查实际导入路径。

### 获取与安装

按[安装部署指南](docs/zh/installation_guide.md#wheel下载方式)下载匹配的wheel及同名`.sha256`文件并完成校验。以下以CP314为例，在已取得安装包的目录创建独立环境并安装：

```bash
python3.14 -m venv .venv314
.venv314/bin/python -m pip install --no-index \
  ./cinderx-2026.11.10.0-cp314-cp314-manylinux_2_24_aarch64.manylinux_2_28_aarch64.whl
```

后续CP314命令中的`python3.14`替换为`.venv314/bin/python`，或先激活该环境。CP311 wheel、Conda及RPM的完整步骤见[安装部署指南](docs/zh/installation_guide.md)；升级、回退、禁用与卸载也在该指南中说明。源码安装与打包见[编译构建指南](docs/zh/compilation_build_guide.md)。

### 安装后的文件变化

安装成功后，目标Python的`site-packages`下会新增CinderX相关文件和目录。主要文件说明如下：

| 文件/目录 | 说明 |
|---|---|
| `cinderx/` | CinderX Python包，包含`cinderx.jit`、缓存属性、Strict/Static辅助接口、compiler子包、`cinderx/opcode.py`等 |
| `cinderx/_native/` | CPython 3.11.6 wheel在该目录包含构建溯源信息`build_info_311.json`；CPython 3.14 fat wheel包含`fat_wheel.json`和`py314_0`至`py314_3`对应的`_cinderx_314*.so` |
| `_cinderx*.so` | 仅CPython 3.11.6 wheel提供的顶层native扩展；CPython 3.14 fat wheel使用下述`_cinderx.py` loader |
| `_cinderx.py` | CPython 3.14 fat wheel的顶层native loader；按当前micro版本加载`cinderx/_native/`下对应的native扩展 |
| `__static__/`、`__strict__/` | Static Python和Strict Modules的兼容包入口 |
| `opcodes/` | CinderX opcode生成/维护辅助包，包含`3.12/`、`3.14/`、`3.15/`版本化`opcode.py`和生成脚本 |
| `_cinderx_auto.py` | 自动导入入口；由`cinderx.pth`在Python启动时触发 |
| `cinderx.pth` | site模块启动钩子；设置`CINDERX_PLUGIN_ENABLE=1`时自动导入CinderX |
| `cinderx-<version>.dist-info/` | wheel元数据、`RECORD`、安装器信息等 |

> 实际属主和权限会受安装方式、`umask`、虚拟环境或系统Python管理策略影响。系统级安装通常归`root:root`所有；虚拟环境安装通常归当前用户所有。

### 安装成功验证

在安装包所在环境中检查自动加载、初始化、JIT状态和帧配置，以下以CP314为例：

```bash
CINDERX_PLUGIN_ENABLE=1 PYTHONJITAUTO=auto:2 \
  CINDERX_OSR_ENABLED=0 PYTHONJITLIGHTWEIGHTFRAME=0 \
  .venv314/bin/python - <<'PY'
import sys
import cinderx
import cinderx.jit

print("_cinderx_auto loaded:", "_cinderx_auto" in sys.modules)
print("cinderx initialized:", cinderx.is_initialized())
print("import error:", cinderx.get_import_error())
if cinderx.get_import_error() is not None or not cinderx.is_initialized():
    raise SystemExit("CinderX initialization failed")

import _cinderx
import cinderjit

print("cinderx:", cinderx.__file__)
print("_cinderx:", _cinderx.__file__)
print("jit enabled:", cinderx.jit.is_enabled())
print("lightweight frames compiled:", cinderx.is_lightweight_frames_enabled())
print("jit frame mode:", cinderjit.jit_frame_mode())
PY
```

该示例显式选择普通帧，帧模式应为`0`。自动加载、初始化及JIT启用应为`True`，导入错误应为`None`。这些状态不能代替目标函数已编译、机器码实际执行或性能收益的验证；最小函数示例和CP311检查命令见[快速入门](docs/zh/quick_start.md)。

### Docker开发与测试环境

在Linux AArch64主机的仓库根目录执行，按Python版本分别构建镜像：

```bash
docker build --build-arg BUILD_JOBS=16 -t cinderx-dev:py314 .
docker build -f Dockerfile.cp311-dev --build-arg BUILD_JOBS=16 -t cinderx-dev:py311 .
```

- 两套镜像默认基于`openeuler/openeuler:24.03-lts-sp3`。镜像构建需要联网，预装CinderX、测试及性能工具。
- [Dockerfile](Dockerfile)使用GCC 12构建CPython 3.14.3（静态libpython、PGO+LTO）；[Dockerfile.cp311-dev](Dockerfile.cp311-dev)使用固定的openEuler `python3/python3-devel-3.11.6-34.oe2403sp3`包和共享libpython。
- CinderX wheel和RuntimeTests均使用GCC 14；RuntimeTests默认关闭LTO，覆盖率模式不能与LTO/PGO同时启用。
- 镜像预置`/opt/cinderx-deps`和`/opt/cinderx-pydeps`，默认启用pip离线安装。pytest统一为`9.0.3`；3.11构建依赖以[requirements-cp311-build.txt](ci_pipeline/requirements-cp311-build.txt)为准，离线缓存必须包含指定版本及其传递依赖。
- Dockerfile、Python、编译器或依赖版本变化后须重建镜像。镜像包含构建时复制的源码；挂载新源码后须重新构建安装CinderX。CI应记录源码SHA和镜像ID。

完整PR/Daily运行方式见[CI测试指南](ci_pipeline/README_CN.md)。

## 参考文档

首次使用建议先阅读快速入门，安装或升级前核对版本说明；源码构建、运行配置和故障处理按对应指南查阅。各指南按CPython 3.11、CPython 3.14分别说明差异，共用流程集中描述。

| 文档 | 主要内容 |
|---|---|
| [快速入门](docs/zh/quick_start.md) | 安装并运行最小JIT示例，检查结果与编译状态 |
| [版本说明书](docs/zh/release_notes.md) | 发布版本、配套环境、制品、主要变更和使用限制 |
| [编译构建指南](docs/zh/compilation_build_guide.md) | 开发环境、源码构建、wheel打包和依赖缓存 |
| [安装部署指南](docs/zh/installation_guide.md) | 取包与校验、环境准备、安装、升级、回退和卸载 |
| [用户指南](docs/zh/user_guide.md) | 特性、配置、API、运行维护、故障处理和性能验证 |
| [详细开发文档](cinderx/Docs/README_CN.md) | 手动构建CPython、性能测试与调试方法 |
| [CI测试指南](ci_pipeline/README_CN.md) | GitCode门禁、PR/Daily功能验证和依赖缓存 |
| [英文上游说明](README_Opensource.md) | Meta上游项目介绍和使用说明 |

## 贡献声明

本项目基于Meta开源的CinderX开展鲲鹏/ARM平台适配与优化。感谢上游社区，以及参与问题反馈、代码改进、测试验证和文档建设的贡献者。

欢迎通过本仓库的[Issue](https://gitcode.com/openeuler/cinderx/issues)和[PR](https://gitcode.com/openeuler/cinderx/pulls)反馈问题、提交改进。问题反馈请提供版本、运行环境和复现步骤；代码与文档修改请说明目的、影响范围及验证结果。贡献提交应遵循本仓库及openEuler社区的贡献流程，按门禁提示完成所需检查。[上游贡献说明](CONTRIBUTING.md)描述Meta上游流程，本仓库贡献以openEuler社区要求为准。

项目使用MIT许可证。项目及第三方组件的版权和许可要求见[COPYRIGHT](COPYRIGHT)、[LICENSE](LICENSE)及相应第三方目录。

# 用户指南

本指南介绍CinderX的主要功能、适用场景、配置方法和故障处理。先按CPython版本选择运行配置，再根据需要查看诊断和性能验证方法。下文将CPython 3.11、3.14分别简称为CP311、CP314。

首次使用建议先完成[快速入门](quick_start.md)；安装、升级和环境管理见[安装部署指南](installation_guide.md)。本文对应的源码基线和发布包信息见[版本说明书](https://gitcode.com/openeuler/cinderx/releases)。

## 特性、架构与适用场景

CinderX由Python包和本地扩展模块组成。Python启动时，site模块处理`cinderx.pth`，由其调用自动加载入口；入口根据配置加载扩展并初始化相关功能。CP311的运行配置还涉及帧求值器（evaluator），即负责执行Python帧的解释器入口。

JIT（即时编译器）把受支持函数的字节码转换为中间表示，优化后生成本机机器码。自动编译会先检查调用热度及准入条件；机器码运行所依赖的类型等假设失效时，可能发生去优化（deopt），恢复解释执行。因而，并非所有函数都需要或能够进入JIT。

普通自动JIT在函数调用边界进行调度。OSR（栈上替换）针对一次调用中长时间运行的热循环，尝试在循环执行过程中转入机器码。轻量级帧则减少部分帧维护成本，并在需要访问完整Python帧对象时创建该对象。

| 场景 | 使用建议 |
|---|---|
| 长期运行、有稳定热点的服务 | 从普通JIT开始，验证业务语义，再测量稳定运行收益和内存成本 |
| 调用密集或数值循环程序 | 检查目标函数是否编译，结合实际负载评估调用和循环优化 |
| 短脚本、启动和导入较重的程序 | 同时观察编译开销；CP314可评估分类策略，避免只看稳定阶段 |
| 一次调用内的长循环 | 在支持的CP314环境受控评估实验性OSR |
| 依赖frame、trace／profile和异常栈的应用 | 对这些业务路径做兼容验证，再选择帧模式和调度参数 |

上游仓库还包括Static Python、Strict Modules、并行垃圾回收（Parallel GC）和缓存属性等能力，其可用性取决于Python、平台和构建条件。使用前应核对目标环境，进阶资料见[Static Python](../../cinderx/Docs/StaticPython/README.md)、[Strict Modules](../../cinderx/Docs/StrictModules/README.md)和[详细开发文档](../../cinderx/Docs/README_CN.md)。

## 通用配置与API

环境变量通常在进程启动或JIT初始化时读取，修改后应启动新进程。对于同时提供环境变量和`-X`选项的JIT参数，`-X`选项优先；排查时应记录两者。初次使用建议采用对应版本的示例配置，再逐项调整。

| 环境变量 | 用法与作用 |
|---|---|
| `CINDERX_PLUGIN_ENABLE=1` | 启用site启动阶段的插件自动加载；两条线还需满足各自启动条件 |
| `CINDERX_DISABLE=1` | 关闭CinderX的常规插件加载和Python包初始化 |
| `PYTHONJITDISABLE=1` | 禁用JIT，可用于两条Python支持线 |
| `PYTHONJITAUTO=N` | 使用数值调用阈值；达到阈值后仍受准入和运行条件约束 |
| `PYTHONJITALL=1` | 请求尽早编译可支持的函数，仍受支持范围约束；会增加启动编译成本 |
| `PYTHONJITDEBUG=1` | 打开JIT调试日志 |
| `PYTHONJITLOGFILE=/absolute/path/to/jit.log` | 指定JIT日志输出文件，目录需存在且可写 |
| `PYTHONJITDUMPFINALHIR=1` | 输出最终高级中间表示（HIR），供进一步分析 |
| `PYTHONJITDUMPLIR=1` | 输出低级中间表示（LIR） |
| `PYTHONJITDUMPASM=1` | 输出生成汇编；完整反汇编能力受构建配置影响 |

应用也可通过`cinderx.jit`操作一个已正确初始化的JIT：

| API | 用途 |
|---|---|
| `is_enabled()` | 检查JIT当前是否启用 |
| `force_compile(func)` | 请求立即编译指定函数；检查异常及编译后的状态 |
| `is_jit_compiled(func)` | 检查指定函数是否已经编译 |
| `disable()`／`enable()` | 暂停／恢复JIT调度；`disable(deopt_all=True)`还使已编译函数的后续调用回到解释路径 |
| `pause()` | 在`with`块中暂停调度，退出时恢复原启用状态；也支持`deopt_all=True` |

先按对应版本的启动配置启用JIT，再使用这些接口。判断编译结果时，使用`is_jit_compiled(func)`检查状态，并留意`force_compile`抛出的异常；目标可能已经编译，不能只看本次编译请求的返回值。

查询帧模式时，还需区分两个接口：`cinderx.is_lightweight_frames_enabled()`返回构建是否包含轻量级帧能力；JIT已初始化后，`cinderjit.jit_frame_mode()`返回实际配置的帧模式，`0`为普通帧，`1`为轻量级帧。

## CPython 3.11

### 选择解释器

CP311 RPM用于openEuler 24.03 LTS-SP5 AArch64的CPython 3.11.6；安装与维护见[安装部署指南](installation_guide.md#cpython-311-rpm)。按安装方式选择下面一行，为后续命令设置`CINDERX_PY`：

| 安装方式 | 解释器设置 |
|---|---|
| wheel安装到独立venv | `CINDERX_PY="$PWD/.venv311/bin/python"` |
| wheel安装到Conda环境 | `conda activate cinderx311`后设置`CINDERX_PY="$(command -v python)"` |
| RPM安装到系统 | `CINDERX_PY=/usr/bin/python3.11` |
| RPM配合已创建的共享系统包venv | `CINDERX_PY="$PWD/.venv311-rpm/bin/python"` |

普通venv默认无法访问系统RPM。需要复用时，用`/usr/bin/python3.11 -m venv --system-site-packages .venv311-rpm`新建环境，并核对导入位置。系统RPM通过`dnf`维护，避免在相同系统目录中用`pip`覆盖其文件。

### 启动与执行模式

设置`CINDERX_PY`后执行以下命令，`your_app.py`替换为业务入口。wheel和RPM使用相同的CP311启动参数：

```bash
CINDERX_PLUGIN_ENABLE=1 CINDERX_EVAL_MODE=cinder \
  CINDERX_JIT_MODE=execute PYTHONJITAUTO=20 \
  CINDERX_OSR_ENABLED=0 PYTHONJITLIGHTWEIGHTFRAME=0 \
  "$CINDERX_PY" your_app.py
```

`CINDERX_EVAL_MODE=cinder`选择CinderX帧求值器，`CINDERX_JIT_MODE=execute`选择机器码执行模式。本例显式设置这两项，便于检查启动配置。`CINDERX_JIT_MODE`默认是`off`，可选模式如下：

| 模式 | 作用 |
|---|---|
| `off` | 不启用这条JIT调度路径 |
| `observe` | 观察调用与相关状态，用于诊断 |
| `shadow` | 验证编译流程，生成代码不作为业务机器码执行 |
| `execute` | 用于业务运行的自动JIT执行模式 |

`canary`是测试中保留的模式名称，日常使用选择`execute`。配置完成后，先检查JIT状态，再检查目标函数的编译和执行情况。

### 自动调度与参数

CP311使用数值调用阈值。`PYTHONJITAUTO=20`表示函数达到该调用阈值后进入编译判断；未显式设置时，调用观察器的默认阈值为50。启动宽限期、函数支持范围和退避状态等条件仍可能推迟或拒绝编译。

CP311不支持`PYTHONJITAUTO=auto`／`auto:N`行为分类；使用这些配置会被拒绝。按次数自动编译与行为分类是两种能力。

当前CP311不支持JIT列表，也未开放`compile_after_n_calls`、`lazy_compile`等CP314控制接口。请通过启动配置设置数值阈值；调用未开放的接口会报错。

| 配置 | 默认值 | 说明 |
|---|---|---|
| `CINDERX_JIT_STARTUP_GRACE_MS` | `100` | 自动调度启动宽限期，毫秒；`0`关闭延迟，显式编译不受同样延迟约束 |
| `CINDERX_JIT_FORK_GRACE` | `1` | fork子进程重新开始宽限期，继承的编译产物继续可用 |
| `CINDERX_JIT_DESPECIALIZE` | `1` | 满足条件时允许一次去特化重试，不能无限重编译 |
| `CINDERX_JIT_FORCE_DEOPT_CHECKS` | `1` | 保留force-deopt诊断能力；关闭后相应请求会被明确拒绝 |
| `CINDERX_JIT_AUTO_SIZE_SCALING` | `0` | 可选地提高大段直线代码的自动编译阈值 |

需要检查精确调用阈值时，必须记录宽限期配置并避免把诊断配置与默认运行结果混用。正式参数的范围、优先级和完整说明见[CP311调度策略](../../ci_pipeline/README_CN.md#cpython-311-调度策略)。

### 帧与OSR限制

CP311使用普通帧，不支持OSR和轻量级帧执行。即使设置`PYTHONJITLIGHTWEIGHTFRAME=1`，当前代码也会回退到普通帧；在已启用的execute模式下，`cinderjit.jit_frame_mode()`返回`0`。

## CPython 3.14

### 启动与普通JIT

使用推荐的CPython 3.14.3。wheel安装到venv时，设置`CINDERX_PY="$PWD/.venv314/bin/python"`；安装到Conda时，先`conda activate cinderx314`，再设置`CINDERX_PY="$(command -v python)"`。环境准备见[安装部署指南](installation_guide.md#cpython-314)。选择解释器后，开启分类模式，并保持普通帧和OSR关闭：

```bash
CINDERX_PLUGIN_ENABLE=1 PYTHONJITAUTO=auto:2 \
  CINDERX_OSR_ENABLED=0 PYTHONJITLIGHTWEIGHTFRAME=0 \
  "$CINDERX_PY" your_app.py
```

需要传统调用阈值调度时，将`PYTHONJITAUTO`改为正整数，例如`20`。调用数达到阈值表示进入调度判断，不保证所有函数立即编译。

CP314还可通过`cinderx.jit.compile_after_n_calls(N)`配置传统调用阈值，或通过`lazy_compile(func)`请求后续调用时编译。使用`PYTHONJITLISTFILE=/absolute/path/to/jit-list.txt`可从文件选择范围，条目示例为`module_name:qualified_function_name`；这些入口均受函数支持范围约束。

### AutoJIT行为分类

`PYTHONJITAUTO=auto`使用默认初始调用阈值2，`auto:N`使用指定阈值。达到阈值后，分类器结合函数字节码形状、导入／初始化阶段信号和去优化反馈，决定是否编译、延后编译或保持解释执行。

分类模式用于控制低阈值下的启动编译成本，同时保留合适热点的编译机会。实际效果需要通过业务测量判断。CPython 3.14.0的直接调用路径绕过分类准入，建议使用3.14.3评估分类策略。

| 配置 | 默认行为 | 用途 |
|---|---|---|
| `CINDERX_AUTOJIT_IMPORT_PROVIDER` | 分类模式为`find_and_load` | 提供模块导入阶段信号；`off`关闭 |
| `CINDERX_AUTOJIT_SETUP_PROVIDER` | 分类模式为`lib2to3_main,multiprocessing_pool` | 提供特定初始化阶段信号；`off`关闭 |
| `CINDERX_AUTOJIT_ROI_BACKOFF` | 开启 | 对反复去优化的函数延后或停止自动编译；`0`关闭 |
| `CINDERX_AUTOJIT_GATE_STATS=1` | 默认关闭 | 退出时输出准入统计 |
| `CINDERX_AUTOJIT_GATE_STATS_FILE` | 默认输出到stderr | 指定统计JSONL文件 |

这里的provider指阶段信号提供器。导入阶段信号会启用`StartupInit`策略，setup信号是该策略的补充，用于识别特定初始化窗口。因此，评估setup信号的增量效果时，应保持导入阶段信号开启，只切换setup开关；单独开启setup不能形成独立的初始化阶段策略。

需要观察分类状态时：

```bash
CINDERX_PLUGIN_ENABLE=1 PYTHONJITAUTO=auto:2 \
  CINDERX_OSR_ENABLED=0 PYTHONJITLIGHTWEIGHTFRAME=0 \
  CINDERX_AUTOJIT_GATE_STATS=1 \
  CINDERX_AUTOJIT_GATE_STATS_FILE="$PWD/autojit-gate.jsonl" \
  "$CINDERX_PY" your_app.py
```

阶段信号、运行收益退避（ROI backoff）及统计字段的完整说明见[AutoJIT专题](../../cinderx/Docs/README_CN.md#autojit-行为分类使用指南)。`cinderx.jit.auto()`和`compile_after_n_calls(N)`配置传统调用阈值，不开启行为分类。

### OSR：单次调用中的热循环

OSR是默认关闭的实验特性，依赖已经初始化的JIT。循环回边是循环内返回循环头的控制流边；OSR按每条回边的执行次数计数，达到阈值后尝试编译入口并迁移帧状态。条件不满足时继续解释执行。

| 配置 | 默认值 | 说明 |
|---|---|---|
| `CINDERX_OSR_ENABLED` | `0` | `1`开启热循环OSR检测 |
| `CINDERX_OSR_BACKEDGE_THRESHOLD` | `2000` | 每条回边的触发阈值；使用1—2147483647的整数 |
| `PYTHONJITLIGHTWEIGHTFRAME` | 取决于构建能力 | OSR示例显式设为`0`使用普通帧，不能同时开启轻量级帧 |

创建`osr_demo.py`：

```python
def hot_loop(n):
    total = 0
    i = 0
    while i < n:
        total += i
        i += 1
    return total


print(hot_loop(20_000))
```

示例只调用`hot_loop`一次，而普通JIT调用阈值设为10，便于观察由循环回边触发的OSR：

```bash
CINDERX_PLUGIN_ENABLE=1 PYTHONJITAUTO=10 \
  CINDERX_OSR_ENABLED=1 CINDERX_OSR_BACKEDGE_THRESHOLD=100 \
  PYTHONJITLIGHTWEIGHTFRAME=0 PYTHONJITDUMPHIR=1 \
  "$CINDERX_PY" osr_demo.py
```

计算结果应为`199990000`。HIR日志可用于检查是否生成`OSREntry`；确认入口实际执行及其性能收益，还需要运行时证据。禁用JIT后，OSR也不会生效。

### 轻量级帧

轻量级帧用于减少部分JIT帧维护开销，在需要时再创建完整Python帧对象。构建时需要启用`ENABLE_LIGHTWEIGHT_FRAMES`；包含该能力的CP314构建在未设置帧开关时默认使用轻量级帧，未包含该能力时默认使用普通帧。前面的基础示例显式设置`PYTHONJITLIGHTWEIGHTFRAME=0`，以选择普通帧。

```bash
CINDERX_PLUGIN_ENABLE=1 PYTHONJITAUTO=2 \
  PYTHONJITLIGHTWEIGHTFRAME=1 CINDERX_OSR_ENABLED=0 \
  "$CINDERX_PY" your_app.py
```

采用该配置启动后，`cinderjit.jit_frame_mode()`应返回`1`。`cinderx.is_lightweight_frames_enabled()`只表示构建是否包含该能力，返回`True`不代表运行时已经启用。依赖`sys._getframe()`、跟踪／性能分析回调（trace／profile）和异常栈的业务，还需检查相应路径的行为。

在CP314中，显式设置`PYTHONJITLIGHTWEIGHTFRAME=1`后，如果构建产物未包含轻量级帧能力，或同时启用OSR，JIT初始化会失败。未设置帧开关且构建不包含该能力时，默认使用普通帧。遇到配置错误时，应先修正构建或运行配置，再执行验证程序。

## 运行维护与故障处理

### 检查身份与状态

`CINDERX_PY`沿用前面选择的解释器：wheel指向安装包的venv或Conda环境，CP311 RPM指向`/usr/bin/python3.11`或已创建的共享系统包venv。重新打开终端后，先将它设为该解释器的绝对路径。以下命令检查包身份和初始化状态；检查业务JIT状态时，还应带上对应版本的完整启动参数。

```bash
"$CINDERX_PY" - <<'PY'
import importlib.metadata as metadata
import platform
import sys
import sysconfig
import cinderx

print("python:", sys.version)
print("platform:", platform.platform(), platform.machine())
print("SOABI:", sysconfig.get_config_var("SOABI"))
print("package:", metadata.version("cinderx"))
print("path:", cinderx.__file__)
print("initialized:", cinderx.is_initialized())
print("import error:", cinderx.get_import_error())
if cinderx.get_import_error() is not None or not cinderx.is_initialized():
    raise SystemExit("CinderX initialization failed; inspect the error above")
PY
```

RPM安装还可用`rpm -q python3-cinderx`查询发行版包版本，用`rpm -V python3-cinderx`核对文件完整性。`rpm`查询结果与Python元数据、实际导入路径应对应同一份包。

建议依次检查包和解释器的二进制接口（ABI）是否匹配、扩展是否初始化、JIT是否启用、目标是否编译及执行，最后再分析性能。

### 记录日志

在所选版本的启动命令前增加`PYTHONJITDEBUG=1`和`PYTHONJITLOGFILE=/absolute/path/to/jit.log`。需要分析生成代码时，再增加HIR、LIR或汇编输出开关。日志输出会影响运行开销，诊断记录应与正式性能测量结果分开保存。

| 现象 | 下一步 |
|---|---|
| RPM已装但业务进程找不到CinderX | 检查业务实际解释器和venv隔离设置；使用系统Python或允许访问系统包的venv |
| 初始化失败 | 记录`get_import_error()`或完整启动traceback；检查ABI、禁用变量和参数冲突 |
| JIT启用但函数未编译 | 先检查阈值、函数支持范围及是否已停止自动编译；CP311另查宽限期，CP314另查列表和分类延后 |
| 已编译但性能未改善 | 区分编译时间、机器码实际进入、deopt和程序中非目标部分，不仅观察compiled状态 |
| 开启帧访问或跟踪后行为变化 | 使用相同业务路径复现，记录帧模式和trace／profile配置 |
| 本地扩展断言或崩溃 | 保留完整命令、退出状态、core／栈、包哈希和最小复现；恢复可用版本见安装指南 |

反馈问题时附带版本／源码SHA、系统与Python指纹、wheel或RPM的版本与哈希、完整启动参数、预期与实际结果以及相关日志。日志和core中可能包含业务数据，分享前按业务要求处理。

## 性能验证

先确认业务结果，再比较相同硬件和资源配置下的基线与候选。记录Python、源码与安装包身份、JIT模式、PGO／LTO、CPU affinity、进程／线程、预热、loops和测量范围。

分别评估启动时间、稳定运行时间、编译开销、内存和回退行为。最小示例不是性能结论；定向benchmark不替代全量或实际业务验收。使用pyperformance时，worker还需继承所选版本的完整JIT环境。

现有工具入口见[性能测试与调试文档](../../cinderx/Docs/README_CN.md#性能测试)和[CI指南](../../ci_pipeline/README_CN.md)。

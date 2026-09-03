# CinderX

CinderX 是一个 Python 运行时性能扩展，核心功能是将 Python 字节码即时编译（JIT）为原生机器码，从而显著提升 Python 程序的执行速度。本项目基于上游社区 CinderX 项目，专注于 Kunpeng & ARM 环境下的性能优化。

> English documentation: [README.md](README.md)

## 环境要求

- Python 3.11/3.14
- Linux (aarch64)
- CPython 3.11.6 目前仅支持 openEuler 24.03 (LTS-SP3)；CPython 3.14 推荐 openEuler 24.03 (LTS-SP3)

## 兼容性

当前发布的 CinderX whl 包对各平台的兼容情况如下：

| CPython 版本 | aarch64 (ARM64) | x86_64 |
|---|---|---|
| 3.11.6 | 支持 | — |
| 3.14.0 | 支持 | — |
| 3.14.1 | 支持 | — |
| 3.14.2 | 支持 | — |
| 3.14.3 | 支持（推荐） | — |

> **注意**：
> - 当前仅提供 aarch64 (ARM64) 架构的预编译 whl 包，x86_64 暂未提供预编译包。
> - 仅支持 CPython 3.11.6 和 3.14.0–3.14.3，不支持其他 Python 版本。

## 安装

### 安装系统要求

安装预编译 whl 前，请确认运行环境满足以下要求：

- 操作系统：Linux aarch64/ARM64。CPython 3.11.6 目前仅支持 openEuler 24.03 (LTS-SP3)；CPython 3.14 推荐 openEuler 24.03 (LTS-SP3)。
- Python：CPython 3.11.6 或 3.14.0 - 3.14.3，推荐 3.14.3；`pip` 必须来自对应的 `python3.11` 或 `python3.14`。
- 权限：安装到系统 `site-packages` 需要 root 权限；普通用户建议先创建虚拟环境，或使用 `--user` 安装。

建议先确认解释器和平台：

```bash
python3.14 - <<'PY'
import platform
import sys

print(sys.version)
print(platform.platform())
print(platform.machine())
PY
```

### 安装命令

在 [openeuler releases](https://gitcode.com/openeuler/cinderx/releases) 获取最新
cinderx whl 包后安装（以 `python3.14` 为例）：

```bash
python3.14 -m pip install --no-index cinderx-*_aarch64.whl
```

### 安装后的文件变化

安装成功后，目标 Python 的 `site-packages` 下会新增 CinderX 相关文件和目录。主要文件说明如下：

| 文件/目录 | 说明 |
|---|---|
| `cinderx/` | CinderX Python 包，包含 `cinderx.jit`、缓存属性、Strict/Static 辅助接口、compiler 子包、`cinderx/opcode.py` 等 |
| `cinderx/_native/` | CPython 3.11.6 wheel 在该目录包含构建溯源信息 `build_info_311.json`；CPython 3.14 fat wheel 包含 `fat_wheel.json` 和 `py314_0` 至 `py314_3` 对应的 `_cinderx_314*.so` |
| `_cinderx*.so` | 仅 CPython 3.11.6 wheel 提供的顶层 native 扩展；CPython 3.14 fat wheel 使用下述 `_cinderx.py` loader |
| `_cinderx.py` | CPython 3.14 fat wheel 的顶层 native loader；按当前 micro 版本加载 `cinderx/_native/` 下对应的 native 扩展 |
| `__static__/`、`__strict__/` | Static Python 和 Strict Modules 的兼容包入口 | 
| `opcodes/` | CinderX opcode 生成/维护辅助包，包含 `3.12/`、`3.14/`、`3.15/` 版本化 `opcode.py` 和生成脚本 |
| `_cinderx_auto.py` | 自动导入入口；由 `cinderx.pth` 在 Python 启动时触发 |
| `cinderx.pth` | site 模块启动钩子；设置 `CINDERX_PLUGIN_ENABLE=1` 时自动导入 CinderX |
| `cinderx-<version>.dist-info/` | wheel 元数据、`RECORD`、安装器信息等 |

> 实际属主和权限会受安装方式、`umask`、虚拟环境或系统 Python 管理策略影响。系统级安装通常归 `root:root` 所有；虚拟环境安装通常归当前用户所有。

### 安装成功验证

先验证包和 native 扩展可以导入：

```bash
# 以 python3.14 为例
python3.14 - <<'PY'
import _cinderx
import cinderx
import cinderx.jit

print("cinderx:", cinderx.__file__)
print("_cinderx:", _cinderx.__file__)
print("jit enabled:", cinderx.jit.is_enabled())
print("lightweight frames:", cinderx.is_lightweight_frames_enabled())
PY
```

再验证自动导入路径：

```bash
CINDERX_PLUGIN_ENABLE=1 PYTHONJITAUTO=auto:2 python3.14 - <<'PY'
import sys

print("_cinderx_auto loaded:", "_cinderx_auto" in sys.modules)
import cinderx
import cinderx.jit

print("cinderx initialized:", cinderx.is_initialized())
print("jit enabled:", cinderx.jit.is_enabled())
PY
```

如果上述命令能输出 `cinderx` 和 `_cinderx` 的安装路径，且自动导入场景中
`_cinderx_auto loaded: True`，说明 whl 已安装到当前 `python3.14` 的搜索路径中。

## 特性概览

| 特性 | CPython 3.11 | CPython 3.14 |
|---|---|---|
| 自动导入 | 支持 | 支持 |
| OSR | 不支持 | 支持 |
| 轻量级帧 | 不支持 | 支持 |
| AutoJIT | 不支持 | 支持 |

### 自动导入

CinderX 支持零代码侵入的自动导入机制。只需设置一个环境变量，CPython 启动时便会自动加载 CinderX 扩展，已有的 Python 项目无需任何改动即可享受 JIT 带来的性能加速。无论是遗留系统还是大型项目，都能以最低成本完成性能验证。

```bash
export CINDERX_PLUGIN_ENABLE=1
```

### OSR（栈上替换，实验特性）

OSR（On-Stack Replacement，栈上替换）用于优化单次调用内运行很久的热循环。普通自动 JIT 主要在函数调用边界按调用次数触发编译；OSR 会在解释器执行循环回边时计数，达到阈值后尝试编译包含 OSR 入口的版本，并从当前循环头直接切入 JIT 代码。

OSR 目前是实验特性，默认关闭，建议仅在性能验证或受控场景中开启。OSR 依赖 JIT 已完成初始化；如果设置了 `PYTHONJITDISABLE=1`，OSR 不会单独生效。

**环境变量开关**：

| 环境变量 | 作用 |
|---|---|
| `CINDERX_OSR_ENABLED=1` | 开启 OSR 热循环检测 |
| `CINDERX_OSR_ENABLED=0` 或不设置 | 关闭 OSR（默认） |
| `CINDERX_OSR_BACKEDGE_THRESHOLD=N` | 单条循环回边执行 N 次后尝试 OSR，默认值为 2000；`N` 建议设置为 1 到 2147483647 的整数 |

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
CINDERX_OSR_ENABLED=1 \
CINDERX_OSR_BACKEDGE_THRESHOLD=100 \
PYTHONJITDUMPHIR=1 \
python3.14 osr_demo.py
```

上述示例只调用 `hot_loop` 一次，但循环回边会超过 OSR 阈值并触发 OSR 尝试。配合 `PYTHONJITDUMPHIR=1` 可在 HIR 日志中观察是否生成 `OSREntry`。

### 轻量级帧（Lightweight Frames）

轻量级帧提供更轻量的 JIT 帧模式，用于减少频繁调用场景下 Python 帧维护与物化带来的开销。完整 Python frame 会在需要调试、追踪或访问 frame 对象时再按需物化，因此开启后建议重点验证依赖 `sys._getframe()`、trace/profile、异常栈等能力的业务路径。

**环境变量开关**：

| 环境变量 | 作用 |
|---|---|
| `PYTHONJITLIGHTWEIGHTFRAME=1` | 开启轻量级帧 |
| `PYTHONJITLIGHTWEIGHTFRAME=0` 或不设置 | 使用普通帧模式（默认） |

示例：

```bash
CINDERX_PLUGIN_ENABLE=1 \
PYTHONJITAUTO=2 \
PYTHONJITLIGHTWEIGHTFRAME=1 \
CINDERX_OSR_ENABLED=0 \
python3.14 your_app.py
```

轻量级帧需要构建时启用 `ENABLE_LIGHTWEIGHT_FRAMES`；当前 Python 3.14 的 aarch64/arm64 构建默认编译该能力。如果运行时设置 `PYTHONJITLIGHTWEIGHTFRAME=1`，但构建产物未包含该能力，JIT 初始化会失败并报错。

> **注意**：OSR 和轻量级帧互斥，不能同时开启。当前 OSR 的帧状态迁移和入口桩只支持普通帧模式（`FrameMode::kNormal`），而轻量级帧会切换到 `FrameMode::kLightweight`，两者的帧布局与状态保存方式不同。因此同时设置 `PYTHONJITLIGHTWEIGHTFRAME=1` 和 `CINDERX_OSR_ENABLED=1` 时，JIT 初始化会直接失败。

### JIT 编译器

**作用**：将 Python 字节码即时编译为原生机器码，显著提升热点函数的执行速度。JIT 会自动追踪函数调用频率，对频繁调用的函数进行编译优化。

**开启方式**：

通过环境变量控制 JIT：

| 环境变量 | 作用 |
|---|---|
| `PYTHONJITAUTO=N` | 启用自动 JIT 模式，函数调用 N 次后编译 |
| `PYTHONJITAUTO=auto` | 启用 AutoJIT 行为分类，默认按 2 次调用阈值开始准入判断 |
| `PYTHONJITAUTO=auto:N` | 启用 AutoJIT 行为分类，并以 N 次调用作为初始准入阈值 |
| `PYTHONJITALL=1` | 编译所有函数（调用 0 次即编译） |
| `PYTHONJITDISABLE=1` | 禁用 JIT |
| `PYTHONJITLISTFILE=/path/to/list` | 通过 JIT 列表文件选择性编译指定函数 |

#### AutoJIT 行为分类

普通 `PYTHONJITAUTO=N` 只看调用次数：函数被调用到阈值后就尝试进入 JIT。低阈值能更早编译热点函数，但在启动、导入、脚本初始化等阶段也容易把大量只执行一两次的函数送进 JIT，造成编译风暴。

`PYTHONJITAUTO=auto:N` 在调用次数之外增加一层行为分类。CinderX 会先根据字节码形状、启动/导入/setup 阶段信号和运行后的 deopt 反馈判断函数是否值得立即编译，再决定是放行、延后还是保持解释执行。它的目标是保留数值循环、稳定热点等高收益函数，同时抑制启动链路、导入链路、异常慢路径和低 ROI helper 的 JIT 成本。

推荐在需要低阈值自动 JIT 的场景优先使用分类模式：

```bash
CINDERX_PLUGIN_ENABLE=1 PYTHONJITAUTO=auto:2 python3.14 your_app.py
```

分类模式下，import provider、setup wrapper 和动态 ROI backoff 默认生效。setup wrapper 当前覆盖 `lib2to3.main` 和 `multiprocessing.pool.Pool` 两类一次性初始化/进程池通信窗口。当前 `StartupInit` 策略由 import provider 打开，setup wrapper 作为同一启动/初始化窗口的附加信号；关闭 import provider 后，不能把 setup wrapper 单独当作 setup-only 策略来评估。需要做 A/B 验证或临时回退时，可以使用以下开关：

| 环境变量 | 作用 |
|---|---|
| `CINDERX_AUTOJIT_IMPORT_PROVIDER=off` | 关闭 import 阶段信号 |
| `CINDERX_AUTOJIT_SETUP_PROVIDER=off` | 关闭 `lib2to3` 和 multiprocessing setup wrapper；需在 import provider 开启时评估其增量 |
| `CINDERX_AUTOJIT_SETUP_PROVIDER=lib2to3_main,multiprocessing_pool` | 显式指定 setup provider 组合 |
| `CINDERX_AUTOJIT_ROI_BACKOFF=0` | 关闭运行时负 ROI 回退 |
| `CINDERX_AUTOJIT_GATE_STATS=1` | 进程退出时输出 AutoJIT 准入统计，便于分析分类效果 |

更完整的使用、测试和诊断方法见 [AutoJIT 行为分类使用指南](cinderx/Docs/README_CN.md#autojit-行为分类使用指南)。

也可以通过修改业务代码使能 JIT 功能：

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

### 其他特性

以下特性为 Meta 上游社区内部生产环境的深度优化，本社区不做重点展开：

- **Static Python（静态 Python）**：利用类型标注在编译期生成更高效的字节码，配合 JIT 可实现接近 Cython 级别的性能。通过 `import __static__` 开启。
- **Strict Modules（严格模块）**：冻结模块类型与内容为不可变，消除常见开发错误。通过 `import __strict__` 开启。
- **并行垃圾回收（Parallel GC）**：多线程并行 GC，减少暂停时间。通过 `cinderx.enable_parallel_gc()` 或 `PARALLEL_GC_ENABLED=1` 开启。
- **缓存属性（Cached Properties）**：高性能 `cached_property` / `async_cached_property` 实现，通过 `from cinderx import cached_property` 使用。
- **对象不朽化（Immortalize）**：标记对象永不被 GC 回收，通过 `cinderx.immortalize_heap()` 开启。
- **自定义帧求值器（Frame Evaluator）**：替换 CPython 解释器循环以支持 Static Python 字节码，通过 `cinderx.install_frame_evaluator()` 开启。
- **JIT 列表（JIT List）**：通过外部文件精确控制 JIT 编译范围，通过 `PYTHONJITLISTFILE=/path/to/list` 开启。
- **JIT 预加载（Preloading）**：编译前预先解析全局变量和类型描述符，为 JIT 内置步骤，通过 `PYTHONJITPRELOADDEPENDENTLIMIT` 等调整。
- **调试与性能分析**：提供 `PYTHONJITDEBUG`、`PYTHONJITDUMPHIR`、`PYTHONJITDUMPASM` 等环境变量用于 JIT 调试和汇编输出，详情参见 [cinderx/Docs/README_CN.md](cinderx/Docs/README_CN.md#调试与性能分析)。

## 更多文档

详细的编译指南、性能测试方法等请参阅子目录文档：

- [cinderx/Docs/README_CN.md](cinderx/Docs/README_CN.md) — 手动构建 CPython、性能测试、调试环境变量等详细操作指南
- [ci_pipeline/README_CN.md](ci_pipeline/README_CN.md) — GitCode 门禁、全量功能测试指南

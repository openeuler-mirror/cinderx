# CinderX 详细文档

本文档包含 CinderX 的详细编译、测试、性能分析和调试指南，面向需要深入使用或贡献 CinderX 的开发者。

## 目录

- [开发者构建说明](#开发者构建说明)
- [性能测试](#性能测试)
  - [CPython 3.14 性能测试](#cpython-314-性能测试)
    - [AutoJIT 行为分类使用指南](#autojit-行为分类使用指南)
  - [CPython 3.11 性能测试](#cpython-311-性能测试)
- [调试与性能分析](#调试与性能分析)
- [FAQ](#faq)


---

## 开发者构建说明

本节的本地依赖缓存和构建命令适用于 CPython 3.11 与 3.14。按目标版本选择构建解释器和工具链，版本要求见[编译构建指南](../../docs/zh/compilation_build_guide.md#准备python环境)。

CinderX 的 CMake 构建会通过 `FetchContent` 获取部分第三方依赖。为了减少构建阶段对 GitHub 网络的依赖，可以通过本地依赖缓存目录复用这些源码。启用后，构建会优先校验缓存中的依赖；依赖缺失、远端不匹配或版本不正确时，会刷新对应依赖子目录。

当前纳入本地缓存的依赖包括：

| 依赖 | 版本 |
|---|---|
| `fmt` | `11.2.0` |
| `parallel-hashmap` | `896f1a03e429c45d9fe9638e892fc1da73befadd` |
| `usdt` | `f4ea2f524efa80d062f4d586d78daafb83dc7d24` |
| `capstone` | `5.0.7`，仅在启用 disassembler 时需要 |
| `googletest` | `v1.17.0`，仅在构建 `runtime_tests` 时需要 |

`zlib` 不纳入本地缓存，保持原有 CMake 查找和获取方式。

### 使用 `setup.py` 构建

直接使用 `setup.py` 时，可以通过 `--local` 指定缓存目录。只构建扩展时用 `build_ext`，完整构建时用 `build`。按目标 Python 版本和构建目的选择一条命令。

**CPython 3.11**

```bash
python3.11 setup.py build_ext --local=/path/to/cinderx-local-deps
python3.11 setup.py build --local=/path/to/cinderx-local-deps
```

**CPython 3.14**

```bash
python3.14 setup.py build_ext --local=/path/to/cinderx-local-deps
python3.14 setup.py build --local=/path/to/cinderx-local-deps
```

### 使用 `pip` 构建

通过 pip 构建时，用 `CINDERX_LOCAL_DEPS` 指定缓存目录。

**CPython 3.11**

```bash
CINDERX_LOCAL_DEPS=/path/to/cinderx-local-deps \
python3.11 -m pip install .
```

**CPython 3.14**

```bash
CINDERX_LOCAL_DEPS=/path/to/cinderx-local-deps \
python3.14 -m pip install .
```

### PGO 与 LTO 构建开关

`setup.py` 通过两个环境变量控制 PGO（Profile-Guided Optimization，反馈导向优化）和 LTO（Link-Time Optimization，链接时优化）：

| 环境变量 | 未设置时 | 显式设置 |
|---|---|---|
| `CINDERX_ENABLE_PGO` | CPython 3.11 开启；CPython 3.14 关闭 | `0` 或空值关闭，其他值开启 |
| `CINDERX_ENABLE_LTO` | CPython 3.11 开启；CPython 3.14 关闭 | `0` 或空值关闭，其他值开启 |

CPython 3.11 开启 PGO 后，构建分三个阶段：先构建带插桩的 `_cinderx`，再在 JIT 开启的状态下运行仓库内的训练场景（`cinderx/TestScripts/pgo_train_workloads.py`），最后用采集到的 profile 重新构建。构建时间会比普通构建长。

使用注意：

- **构建进程中不能已加载 `_cinderx`**，否则 3.11 PGO 构建会直接报错。已安装 CinderX 且设置了 `CINDERX_PLUGIN_ENABLE=1` 时，`cinderx.pth` 会在 Python 启动时自动导入 CinderX；这种情况下请不要使用 `--no-build-isolation` 构建，或在构建前取消该变量。
- **3.11 PGO 构建不会自动降级**：训练或 profile 检查任一失败，构建都会报错终止，不会改为普通构建继续。
- **覆盖率构建默认关闭 PGO 和 LTO**：CMake 不允许覆盖率与 PGO/LTO 同时开启，因此设置 `CINDERX_ENABLE_COVERAGE=1` 且未显式设置两个开关时，PGO 和 LTO 默认关闭；显式开启会在 CMake 配置阶段报错。
- **PGO 制品的哈希可能不同**：常规训练采集的 profile 可能有差异，生成的 `_cinderx.so` 哈希也可能随之变化。
- CPython 3.11 需要普通构建（例如快速迭代调试或使用内存受限的容器）时，显式关闭两个开关。

```bash
CINDERX_ENABLE_PGO=0 CINDERX_ENABLE_LTO=0 python3.11 -m pip install .
```

---

## 性能测试

推荐使用 [pyperformance](https://github.com/python/pyperformance) 进行基准性能测试。pyperformance 是 Python 官方认可的基准测试套件，覆盖 JSON 序列化、正则匹配、加密运算、迭代器等多种场景，能够全面评估 CinderX 带来的性能提升。

请按所用的 Python 版本选择测试步骤。

- [CPython 3.14 性能测试](#cpython-314-性能测试)
- [CPython 3.11 性能测试](#cpython-311-性能测试)

### CPython 3.14 性能测试

以下步骤以源码构建的 CPython 3.14 为例。CinderX 通过自动导入加载插件，pyperformance 可在该环境中运行基准测试。

#### 手动构建 CPython

CinderX 当前支持 CPython 3.14 的部分小版本（3.14.0 - 3.14.3）。

##### 获取 CPython 源码

```bash
git clone https://github.com/python/cpython.git
cd cpython
git checkout v3.14.3   # 或其它的 3.14.x 版本
```

##### 配置构建参数

运行 `configure` 时，建议使用以下参数：

```bash
./configure \
    --enable-optimizations \
    --with-lto \
    --prefix=/usr/local/python3.14
```

关键参数说明：

| 参数 | 作用 |
|---|---|
| `--enable-optimizations` | 启用 PGO（Profile-Guided Optimization）优化，显著提升 CPython 自身性能。构建时间较长，但性能测试中对比基线必不可少 |
| `--with-lto` | 启用 LTO 优化，显著提升 CPython 自身性能 |
| `--prefix` | 指定安装路径，避免覆盖系统自带 Python |

##### 编译与安装

```bash
make -j$(nproc)          # 并行编译，$(nproc) 为 CPU 核心数
sudo make altinstall        # 安装到 --prefix 指定的路径
```

安装完成后，将新构建的 Python 加入 `PATH`：

```bash
export PATH=/usr/local/python3.14/bin:$PATH
python3.14 --version     # 确认版本
```

#### 创建虚拟环境

先安装 pyperformance，再创建用于跑分的 worker 虚拟环境。

```bash
# 安装
python3.14 -m pip install pyperformance==1.13.0
# 在当前目录创建 worker 虚拟环境
python3.14 -m pyperformance venv create --inherit-environ http_proxy,https_proxy
```

创建后，在命令输出的 worker 虚拟环境目录中找到 `pyvenv.cfg`。如果直接把 CinderX wheel 装进 worker，`include-system-site-packages` 可以保留为 `false`。如果要复用基础解释器系统包目录中的 CinderX，将该配置行改为 `include-system-site-packages = true`。测量纯 CPython 基线时，使用独立的 worker 环境并关闭 CinderX 插件。

#### AutoJIT 行为分类使用指南

AutoJIT 行为分类用于低阈值自动 JIT 场景。它不是“编译所有函数”，而是在函数达到初始调用阈值后，先判断这个函数现在是否值得进入 JIT。

最常用的启动方式：

```bash
CINDERX_PLUGIN_ENABLE=1 PYTHONJITAUTO=auto:2 python3.14 your_app.py
```

也可以使用 `-X` 选项：

```bash
CINDERX_PLUGIN_ENABLE=1 python3.14 -X jit-auto=auto:2 your_app.py
```

`PYTHONJITAUTO=auto` 等价于使用默认分类阈值 2；`PYTHONJITAUTO=auto:N` 表示函数调用达到 N 次后进入分类准入。对比之下，`PYTHONJITAUTO=N` 是传统自动 JIT，只按调用次数触发，不启用行为分类。

分类模式默认启用三类辅助能力：

| 能力 | 默认值 | 作用 |
|---|---|---|
| import provider | `find_and_load` | 标记 import 链路中的函数，抑制导入期编译风暴 |
| setup provider | `lib2to3_main,multiprocessing_pool` | 标记已知的一次性 setup 链路和进程池构造/任务提交窗口；当前作为 import provider 打开的 `StartupInit` 策略附加信号 |
| ROI backoff | 开启 | 已经编译的函数如果反复 deopt，会反编译或冻结为解释执行 |

常用环境变量如下：

| 环境变量 | 默认值 | 作用 |
|---|---|---|
| `CINDERX_PLUGIN_ENABLE=1` | 关闭 | 通过 `cinderx.pth` 自动加载 CinderX 插件 |
| `PYTHONJITAUTO=auto` | - | 启用 AutoJIT 行为分类，默认阈值为 2 |
| `PYTHONJITAUTO=auto:N` | - | 启用 AutoJIT 行为分类，并使用 N 作为初始调用阈值 |
| `PYTHONJITAUTO=N` | - | 启用传统自动 JIT，不启用行为分类 |
| `CINDERX_AUTOJIT_IMPORT_PROVIDER=find_and_load` | auto 模式默认开启 | 使用 `importlib._bootstrap._find_and_load` 标记 import 阶段 |
| `CINDERX_AUTOJIT_IMPORT_PROVIDER=off` | - | 关闭 import 阶段信号，常用于 A/B 定位 |
| `CINDERX_AUTOJIT_SETUP_PROVIDER=lib2to3_main,multiprocessing_pool` | auto 模式默认开启 | 使用 `lib2to3.main.main` 和 `multiprocessing.pool.Pool` 标记 setup/进程池构造与任务提交阶段；当前需配合 import provider 开启才影响 `StartupInit` 策略 |
| `CINDERX_AUTOJIT_SETUP_PROVIDER=off` | - | 关闭 setup 阶段信号，常用于评估 setup wrapper 增量 |
| `CINDERX_AUTOJIT_ROI_BACKOFF=1` | 开启 | 启用运行时负 ROI 回退 |
| `CINDERX_AUTOJIT_ROI_BACKOFF=0` | - | 关闭运行时负 ROI 回退，常用于回滚或隔离验证 |
| `CINDERX_AUTOJIT_ROI_BACKOFF_BUDGET=N` | `32` | 单轮 deopt 预算基数 |
| `CINDERX_AUTOJIT_ROI_BACKOFF_MAX_ROUNDS=N` | `1` | 允许反编译后重新预热的最大轮数 |
| `CINDERX_AUTOJIT_ROI_REWARM_FACTOR=N` | `64` | 重新预热阈值倍率 |
| `CINDERX_AUTOJIT_ROI_AGING_INTERVAL_MS=MS` | `60000` | 未冻结函数的 deopt 历史计数每经过一个完整周期减半；`0` 关闭老化并保留原累计计数路径，不影响已冻结函数 |
| `CINDERX_AUTOJIT_GATE_STATS=1` | 关闭 | 进程退出时输出 AutoJIT 准入统计 |
| `CINDERX_AUTOJIT_GATE_STATS_FILE=/path/to/file.jsonl` | stderr | 将准入统计写入 JSONL 文件 |
| `CINDERX_AUTOJIT_COMPILE_EVENTS_FILE=/path/to/file.jsonl` | 关闭 | 记录 forced compile 事件、阶段和函数形状，供性能分析使用 |

pyperformance 推荐先跑传统自动 JIT 与分类模式两组，再用结果文件比较：

```bash
CINDERX_PLUGIN_ENABLE=1 PYTHONJITAUTO=2 \
python3.14 -m pyperformance run \
    --affinity=300 \
    --warmup 3 \
    --inherit-environ http_proxy,https_proxy,LD_LIBRARY_PATH,CINDERX_PLUGIN_ENABLE,PYTHONJITAUTO \
    -b 2to3,python_startup,python_startup_no_site \
    -o jit_auto_2.json

CINDERX_PLUGIN_ENABLE=1 PYTHONJITAUTO=auto:2 \
python3.14 -m pyperformance run \
    --affinity=300 \
    --warmup 3 \
    --inherit-environ http_proxy,https_proxy,LD_LIBRARY_PATH,CINDERX_PLUGIN_ENABLE,PYTHONJITAUTO \
    -b 2to3,python_startup,python_startup_no_site \
    -o autojit_classify_2.json

python3.14 -m pyperf compare_to jit_auto_2.json autojit_classify_2.json
```

如果要定位 import provider、setup provider 或 ROI backoff 的贡献，把对应环境变量加入 `--inherit-environ`，并分别设置为 `off` 或 `0` 做 A/B。当前不支持用 `CINDERX_AUTOJIT_IMPORT_PROVIDER=off` + `CINDERX_AUTOJIT_SETUP_PROVIDER=lib2to3_main,multiprocessing_pool` 代表 setup-only 策略；评估 setup wrapper 增量时，应保持 import provider 开启，只切换 `CINDERX_AUTOJIT_SETUP_PROVIDER`。setup provider 支持逗号或加号分隔的组合值，例如只测进程池窗口时可设为 `multiprocessing_pool`。

```bash
CINDERX_PLUGIN_ENABLE=1 \
PYTHONJITAUTO=auto:2 \
CINDERX_AUTOJIT_IMPORT_PROVIDER=off \
CINDERX_AUTOJIT_SETUP_PROVIDER=off \
CINDERX_AUTOJIT_ROI_BACKOFF=0 \
python3.14 -m pyperformance run \
    --affinity=300 \
    --warmup 3 \
    --inherit-environ http_proxy,https_proxy,LD_LIBRARY_PATH,CINDERX_PLUGIN_ENABLE,PYTHONJITAUTO,CINDERX_AUTOJIT_IMPORT_PROVIDER,CINDERX_AUTOJIT_SETUP_PROVIDER,CINDERX_AUTOJIT_ROI_BACKOFF \
    -b 2to3 \
    -o autojit_no_provider_no_backoff.json
```

需要看分类结果时，打开 gate stats：

```bash
CINDERX_PLUGIN_ENABLE=1 \
PYTHONJITAUTO=auto:2 \
CINDERX_AUTOJIT_GATE_STATS=1 \
CINDERX_AUTOJIT_GATE_STATS_FILE=/tmp/autojit-gate-stats.jsonl \
python3.14 your_app.py
```

需要注意：

- Python API `cinderx.jit.auto()` 和 `cinderx.jit.compile_after_n_calls(N)` 只启用传统自动 JIT，不启用行为分类。
- `PYTHONJITALL=1` 会尽早编译所有函数，不适合用来验证 AutoJIT 分类收益。
- `PYTHONJITDISABLE=1` 或 `CINDERX_JIT_DISABLE=1` 会禁用 JIT；`CINDERX_DISABLE=1` 会禁用整个 CinderX 插件。

#### 运行性能测试

``` bash
# 命令示例
CINDERX_PLUGIN_ENABLE=1 PYTHONJITAUTO=auto:2 python3.14 -m pyperformance run \
    --affinity=300 \
    --warmup 3 \
    --inherit-environ http_proxy,https_proxy,LD_LIBRARY_PATH,CINDERX_PLUGIN_ENABLE,PYTHONJITAUTO \
    -o test.json
```
pyperformance可通过`-b`参数指定用例范围，也可通过`--fast`方式快速验证，更多使用方式参考[pyperformance Docs](https://pyperformance.readthedocs.io)

### CPython 3.11 性能测试

#### 准备测试环境

按[安装部署指南](../../docs/zh/installation_guide.md#cpython-311)准备 CPython 3.11.6 和对应的 CinderX 安装包。若使用其他解释器路径，替换命令中的 `python3.11` 即可。测试系统 RPM 时，应使用基于发行版 `/usr/bin/python3.11` 的环境。

安装 pyperformance。

```bash
python3.11 -m pip install pyperformance==1.13.0
```

pyperformance 在 worker 虚拟环境中运行用例。已有 worker 时可直接复用。首次需要提前准备时，在跑分目录执行以下命令。

```bash
python3.11 -m pyperformance venv create -b nbody
```

创建完成后，在命令输出的 worker 虚拟环境目录中找到 `pyvenv.cfg`，即 `<worker 路径>/pyvenv.cfg`。

`include-system-site-packages` 是这个文件里的 Python 虚拟环境配置项。设为 `true` 时，worker 可以访问创建该环境所用 Python 的系统 `site-packages`。设为 `false` 时，worker 与系统包隔离。按 CinderX 的安装位置选择配置：

- **wheel 直接装在 worker 中**　用该 worker 的 `bin/python` 安装 wheel。CinderX 从 worker 自己的包目录加载，配置可保留为 `false`。
- **复用基础解释器中的安装**　如果 CinderX 按[RPM 安装说明](../../docs/zh/installation_guide.md#cpython-311-rpm)安装在基础解释器的系统包目录中，编辑上述 `pyvenv.cfg`，将配置行改为 `include-system-site-packages = true`。如果 wheel 也装在同一个基础解释器的系统包目录中，使用相同设置。

#### 运行 JIT 耗时对照

下面用 `nbody` 对比同一份 CinderX 安装关闭和开启 JIT 的耗时。两组使用同一帧求值器，只切换 `CINDERX_JIT_MODE`。在未额外设置 JIT 参数的终端和同一目录中依次运行，使用尚不存在的结果文件名。需要绑核时，两组使用相同的 `--affinity` 编号。

**1. 运行基线（关闭 JIT）**

保持 CinderX 插件和帧求值器启用，将 JIT 模式设为 `off`，结果保存到 `cp311-off.json`。

```bash
CINDERX_PLUGIN_ENABLE=1 CINDERX_EVAL_MODE=cinder CINDERX_JIT_MODE=off \
    python3.11 -m pyperformance run -b nbody --warmups 3 \
    --inherit-environ CINDERX_PLUGIN_ENABLE,CINDERX_EVAL_MODE,CINDERX_JIT_MODE \
    -o cp311-off.json
```

**2. 运行候选（开启 JIT）**

将 JIT 模式改为 `execute`，结果保存到 `cp311-execute.json`。

```bash
CINDERX_PLUGIN_ENABLE=1 CINDERX_EVAL_MODE=cinder CINDERX_JIT_MODE=execute \
    python3.11 -m pyperformance run -b nbody --warmups 3 \
    --inherit-environ CINDERX_PLUGIN_ENABLE,CINDERX_EVAL_MODE,CINDERX_JIT_MODE \
    -o cp311-execute.json
```

#### 比较结果

两组都跑完后，用 `pyperf compare_to` 对比两份 JSON 的耗时。`--table` 将对比结果以表格形式输出。

```bash
python3.11 -m pyperf compare_to cp311-off.json cp311-execute.json --table
```

---

## 调试与性能分析

表中的日志和编译产物输出需要启用相应版本的 JIT。HIR、LIR 等编译输出会在目标函数进入编译时产生。启动参数分别见 [CPython 3.11 性能测试](#cpython-311-性能测试)和 [CPython 3.14 性能测试](#cpython-314-性能测试)。

### 两版通用的开关

| 环境变量 | 作用与前提 |
|---|---|
| `PYTHONJITDEBUG=1` | 启用 JIT 调试日志 |
| `PYTHONJITDUMPHIR=1` | 输出初始 HIR（高级中间表示） |
| `PYTHONJITDUMPHIRPASSES=1` | 输出每个优化 pass 后的 HIR |
| `PYTHONJITDUMPFINALHIR=1` | 输出最终优化后的 HIR |
| `PYTHONJITDUMPLIR=1` | 输出 LIR（低级中间表示） |
| `PYTHONJITDUMPSTATS=1` | 在 JIT 退出清理时输出运行时统计 |
| `PYTHONJITLOGFILE=/path/to/log` | 将 JIT 日志写入指定文件 |
| `PYTHONJITGDBSUPPORT=1` | 启用 GDB 调试支持和 JIT 调试日志 |
| `PYTHONJITPRELOADDEPENDENTLIMIT=N` | 限制普通预加载路径处理的依赖函数数。配置初值为 99，实际值随 JIT 配置决定 |
| `CINDERX_AUTOJIT_GATE_STATS=1` | 插件在进程退出时输出共享的 JIT 准入统计。CP311 使用 `execute` 模式，具体计数取决于实际调度路径 |
| `CINDERX_AUTOJIT_GATE_STATS_FILE=/path/to/file.jsonl` | 配合 `CINDERX_AUTOJIT_GATE_STATS=1` 将统计写入 JSONL 文件。未指定时输出到 stderr |
| `CINDERX_DISABLE=1` | 禁用 CinderX 扩展 |

### 需要区分版本的开关

| 环境变量 | CPython 3.11 | CPython 3.14 |
|---|---|---|
| `PYTHONJITDUMPASM=1` | 常规构建默认关闭反汇编，需要构建时启用 `ENABLE_DISASSEMBLER` | 常规构建默认启用反汇编，可以输出生成的汇编 |
| `PYTHONJITHUGEPAGES=0` | JIT 默认使用可回收的代码分配器，大页分配被关闭 | 关闭默认开启的 JIT 大页分配 |
| `PYTHONJITLIGHTWEIGHTFRAME=1` | 即使设置为 `1`，也会回退到普通帧 | 要求构建启用 `ENABLE_LIGHTWEIGHT_FRAMES` 且关闭 OSR |
| `PYTHONJITLISTFILE=/path/to/list` | 常规 JIT 初始化路径不加载列表文件 | 从文件读取需要选择性编译的函数 |
| `PYTHONJITALLSTATICFUNCTIONS=1` | 常规帧入口按数值调用阈值和函数资格调度 | 在支持 Static Python 的场景中安排 Static Python 函数编译 |

查看常规调度过程时，**CPython 3.11** 可用 `CINDERX_JIT_OBSERVE_FILE=/path/to/log` 记录函数限定名、调用次数和调度结果。**CPython 3.14** 可用 `CINDERX_AUTOJIT_COMPILE_EVENTS_FILE=/path/to/file.jsonl` 记录 AutoJIT 准入路径的 `forced_compile` 事件，包括阶段和函数形状。两个文件对应各自的常规调度路径。

---

## FAQ

### 1. 如何简单确认pyperformance可正常使能CinderX插件

两版都用跑分实际使用的 worker 解释器检查。按对应[性能测试步骤](#性能测试)准备启动参数，并用相同设置运行下面的命令。将示例路径替换为实际的 worker 解释器路径。

```bash
/path/to/worker/bin/python -c 'import sys; print("_cinderx" in sys.modules)'
```

输出 `True` 表示启动时已加载原生扩展。要进一步核对 CinderX 的导入路径和初始化状态，按用户指南[检查身份与状态](../../docs/zh/user_guide.md#检查身份与状态)执行。复用基础解释器中的 CinderX 时，还应检查 worker 的 `pyvenv.cfg` 是否允许访问系统包。

### 2. 构建时发生 OOM 怎么处理

两版均可按以下方式处理构建 OOM。日志中的 `Killed`、`cc1plus` 或 `c++` 子进程退出，以及系统日志中的 OOM killer 记录，可以帮助确认是编译或链接阶段超出内存限制。

- 用 `setup.py` 或 pip 构建 wheel 时，将 `CMAKE_BUILD_PARALLEL_LEVEL` 设为较小的正整数，限制 CMake 构建并发。
- 运行本地门禁时，用 `CINDERX_TEST_JOBS` 限制 RuntimeTests 构建并发。

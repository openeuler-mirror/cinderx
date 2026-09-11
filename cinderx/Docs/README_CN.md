# CinderX 详细文档

本文档包含 CinderX 的详细编译、测试、性能分析和调试指南，面向需要深入使用或贡献 CinderX 的开发者。

## 目录

- [开发者构建说明](#开发者构建说明)
- [性能测试](#性能测试)
  - [AutoJIT 行为分类使用指南](#autojit-行为分类使用指南)
- [调试与性能分析](#调试与性能分析)


---

## 开发者构建说明

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

直接使用 `setup.py` 时，可以通过 `--local` 指定缓存目录：

```bash
python3.14 setup.py build_ext --local=/path/to/cinderx-local-deps
python3.14 setup.py build --local=/path/to/cinderx-local-deps
```

### 使用 `pip` 构建

`pip` 无法直接传递 `--local`，需要使用环境变量：

```bash
CINDERX_LOCAL_DEPS=/path/to/cinderx-local-deps \
python3.14 -m pip install .
```
---

## 性能测试

推荐使用 [pyperformance](https://github.com/python/pyperformance) 进行基准性能测试。pyperformance 是 Python 官方认可的基准测试套件，覆盖 JSON 序列化、正则匹配、加密运算、迭代器等多种场景，能够全面评估 CinderX 带来的性能提升。
性能测试需要基于自行编译的 CPython 进行，以确保基线一致和访问内部 API正常。CinderX依据自动导入功能， 替换CPython原生解释器，从而可借用pyperformance测试套件进行CinderX的性能测试。


以下步骤从源码构建 CPython 开始，逐步完成测试环境搭建。

### 手动构建 CPython

CinderX 当前支持 CPython 3.14 的部分小版本（3.14.0 - 3.14.3）。

#### 获取 CPython 源码

```bash
git clone https://github.com/python/cpython.git
cd cpython
git checkout v3.14.3   # 或其它的 3.14.x 版本
```

#### 配置构建参数

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

#### 编译与安装

```bash
make -j$(nproc)          # 并行编译，$(nproc) 为 CPU 核心数
sudo make altinstall        # 安装到 --prefix 指定的路径
```

安装完成后，将新构建的 Python 加入 `PATH`：

```bash
export PATH=/usr/local/python3.14/bin:$PATH
python3.14 --version     # 确认版本
```

### 创建虚拟环境

首先安装 pyperformance，并创建一个独立的 pyperformance 虚拟环境用于测试：

```bash
# 安装
python3.14 -m pip install pyperformance==1.13.0
# 当前路径创建虚拟环境
python3.14 -m pyperformance venv create --inherit-environ http_proxy,https_proxy
# 修改配置，启用系统 site-packages（测试 cpython 基线数据则改为 false）
sed -i 's/^include-system-site-packages = false/include-system-site-packages = true/' venv/<venv_name>/pyvenv.cfg
```

### AutoJIT 行为分类使用指南

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

### 性能测试

``` bash
# 命令示例
CINDERX_PLUGIN_ENABLE=1 PYTHONJITAUTO=auto:2 python3.14 -m pyperformance run \
    --affinity=300 \
    --warmup 3 \
    --inherit-environ http_proxy,https_proxy,LD_LIBRARY_PATH,CINDERX_PLUGIN_ENABLE,PYTHONJITAUTO \
    -o test.json
```
pyperformance可通过`-b`参数指定用例范围，也可通过`--fast`方式快速验证，更多使用方式参考[pyperformance Docs](https://pyperformance.readthedocs.io)

---

## 调试与性能分析

CinderX 提供了丰富的环境变量用于调试和性能分析：

| 环境变量 | 作用 |
|---|---|
| `PYTHONJITDEBUG=1` | 启用 JIT 调试日志 |
| `PYTHONJITDUMPHIR=1` | 输出初始 HIR（高级中间表示） |
| `PYTHONJITDUMPHIRPASSES=1` | 输出每个优化 pass 后的 HIR |
| `PYTHONJITDUMPFINALHIR=1` | 输出最终优化后的 HIR |
| `PYTHONJITDUMPLIR=1` | 输出 LIR（低级中间表示） |
| `PYTHONJITDUMPASM=1` | 输出最终生成的汇编代码 |
| `PYTHONJITDUMPSTATS=1` | 在进程退出时输出 JIT 运行时统计 |
| `PYTHONJITLOGFILE=/path/to/log` | 将日志写入指定文件 |
| `PYTHONJITGDBSUPPORT=1` | 启用 GDB 调试支持 |
| `PYTHONJITHUGEPAGES=0` | 禁用大页内存（默认启用） |
| `PYTHONJITLIGHTWEIGHTFRAME=1` | 启用轻量级解释器帧 |
| `PYTHONJITLISTFILE=/path/to/list` | 通过 JIT 列表文件选择性编译指定函数 |
| `PYTHONJITPRELOADDEPENDENTLIMIT=N` | 编译时预加载依赖函数的最大数量（默认 99） |
| `PYTHONJITALLSTATICFUNCTIONS=1` | 预加载并编译所有 Static Python 函数 |
| `CINDERX_AUTOJIT_GATE_STATS=1` | 输出 AutoJIT 准入统计 |
| `CINDERX_AUTOJIT_GATE_STATS_FILE=/path/to/file.jsonl` | 将 AutoJIT 准入统计写入 JSONL 文件 |
| `CINDERX_AUTOJIT_COMPILE_EVENTS_FILE=/path/to/file.jsonl` | 记录 AutoJIT forced compile 事件、阶段和函数形状 |
| `CINDERX_DISABLE=1` | 强制禁用整个 CinderX 扩展 |

---

## FAQ

### 1. 如何简单确认pyperformance可正常使能CinderX插件
- 确认`venv/<venv_name>/pyvenv.cfg`中的 `include-system-site-packages = true`

### 2. 构建时发生 OOM 怎么处理

如果构建过程中出现 `Killed`、`cc1plus`/`c++` 子进程退出，或系统日志中能看到 OOM killer 记录，通常说明并行编译或链接阶段超过了当前机器/容器的内存上限。
- 单独构建 CinderX wheel 时，可以通过 `PYTHON_CPU_COUNT` 降低 `setup.py` 触发的 CMake 构建并发，或通过`taskset -c`限制可调度的CPU范围。
- 运行本地门禁，也可以通过`CINDERX_TEST_JOBS`显式限制 `runtime_tests` 的 `cmake --build --parallel` 并发。
- 小规格容器建议避免 PGO/LTO，必要时加 swap、提高内存上限或换更大构建环境。

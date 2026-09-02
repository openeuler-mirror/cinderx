# CinderX 本地测试门禁

英文文档：[README.md](README.md)

这里维护 ARM64 Linux CPython 3.11 和 3.14 CinderX JIT 的本地门禁流程。入口脚本是
`ci_pipeline/run_gate.py`，suite 配置位于 `ci_pipeline/suites/*.toml`。

## 软件依赖

运行 `run_gate.py` 前，需要先准备目标 Python、编译工具链、CMake 和测试工具。`pr
--coverage` 会额外依赖 native 覆盖率工具；`daily` 和 compat suite 还需要外部 wheel
以及兼容性矩阵里的 Python 解释器。

### 基础依赖

| 依赖 | 用途 |
|---|---|
| `pip`、`venv`、`setuptools >= 77.0.3` | 构建和安装本地 wheel；`pyproject.toml` 指定 setuptools 版本下限 |
| `pytest` | `cinderx_local`、`wheel_compat` 和 Lib/test 运行时使用 |
| `bash`、`coreutils`、`findutils`、`git` | suite shell 命令、源码状态和文件处理需要 |
| `cmake`、`ctest` | 配置、构建和运行 `runtime` suite |
| `make` 或其它 CMake generator 后端 | `cmake --build` 需要实际构建后端；未显式设置 generator 时通常使用 `make` |

### 覆盖率依赖

运行 `python3.14 ci_pipeline/run_gate.py pr --coverage` 或任何带 `--coverage` 的
native suite 时，还需要：

| 依赖 | 用途 |
|---|---|
| GCC/G++ | CMake 覆盖率模式只支持 GNU 编译器 |
| `gcov` | 生成 native coverage 数据 |
| `lcov` | 收集和过滤 coverage tracefile |
| `genhtml` | 生成 HTML 覆盖率报告 |

覆盖率模式不能和 LTO/PGO 同时使用；`run_gate.py` 会给 `runtime` suite 打开
`ENABLE_COVERAGE=ON`，CMake 会拒绝非 GCC/gcov 覆盖率构建。

## 快速入口

### PR 门禁

`pr` 入口根据运行它的解释器版本选择 suite：

```bash
CINDERX_LOCAL_DEPS=/path/to/cinderx-local-deps \
python3.11 ci_pipeline/run_gate.py pr

CINDERX_LOCAL_DEPS=/path/to/cinderx-local-deps \
python3.14 ci_pipeline/run_gate.py pr --coverage
```

Python 3.11 默认运行 3-job 的 `cp311_gate` 快速验收 suite：
`runtime_tests_311`、`setup_release_311`、`test_release_311`。PR 只构建一个
Release wheel。开发者设置 `CINDERX_LOCAL_RUN_LIBTEST=1` 时会额外运行
`libtest_execute_72_311`；该 job 自行运行 stock 72 与 execute 72 differential，
不依赖 Daily 产物。完整 Lib/test differential 仍只在 Daily 运行。Python 3.14
保持现有流程，顺序是：

1. `runtime`：CMake 构建并运行 native `RuntimeTests`，`--coverage` 只作用于这个 suite。
2. 覆盖率后处理：运行 `gcov`、`lcov`、`genhtml`，并检查覆盖率阈值。
3. `cinderx_local`：构建本地 release wheel，安装到临时 venv，并运行 CinderX Python 测试。

如果 `runtime` 或覆盖率后处理失败，pipeline 会在进入后续 suite 前停止。

### Daily 兼容性门禁

`daily` 复用 PR 门禁的主体流程，并额外展开 wheel 兼容性矩阵：

```bash
CINDERX_LOCAL_DEPS=/path/to/cinderx-local-deps \
CINDERX_TEST_WHEEL=/path/to/cinderx.whl \
python3.14 ci_pipeline/run_gate.py daily
```

Python 3.11 Daily 必须传入 fat wheel；`setup_release_311` 直接安装它，不再构建
第二个 wheel，然后追加 `test_release_daily_311` 和 `libtest_daily_311`：

```bash
CINDERX_LOCAL_DEPS=/path/to/cinderx-local-deps \
CINDERX_TEST_WHEEL=/path/to/cinderx-fat.whl \
python3.11 ci_pipeline/run_gate.py daily
```

`libtest_daily_311` 只执行一次 stock 440 和 evaluator-off 440，随后从 stock
440 结果中抽取 72 模块基线，只运行 execute 72、Py_DEBUG refleak 10，并写
统一报告；不会重复 stock 72 或 evaluator-off 440，且该阶段不包含 shadow
Lib/test arm。

`test_release_daily_311` 还会运行有界的 72 模块 stdlib execute canary，
包括精确的 organic-deopt 漂移守卫。pyperformance completion 命令保留为
显式手工诊断入口，不属于 PR 或 Daily；因此自动功能门禁不声明覆盖完整的
pyperformance worker crash、reject ledger 或 benchmark completion。

`daily` pipeline 的顺序是：

1. `runtime`
2. `cinderx_local`，并自动设置 `CINDERX_LOCAL_RUN_LIBTEST=1` 运行本地 wheel 的 Lib/test
3. `ci_pipeline/python_compat_matrix.toml` 中的 `wheel_compat_<name>`
4. 同一矩阵文件中的 `wheel_compat_negative_<name>`

`daily` 不构建外部兼容性 wheel，调用方必须通过 `CINDERX_TEST_WHEEL` 传入待测 wheel。

### 独立 Suite

可用 suite 对应 `ci_pipeline/suites/*.toml`。`--list` 用于列出某个 pipeline 或 suite
将要运行的 job，不会真正执行：

```bash
python3.11 ci_pipeline/run_gate.py pr --list
python3.14 ci_pipeline/run_gate.py pr --list
python3.14 ci_pipeline/run_gate.py --suite runtime --list
```

单独运行 suite 必须使用 `--suite`：

```bash
python3.14 ci_pipeline/run_gate.py --suite runtime --coverage
python3.14 ci_pipeline/run_gate.py --suite cinderx_local
python3.14 ci_pipeline/run_gate.py --suite wheel_compat
python3.14 ci_pipeline/run_gate.py --suite wheel_compat_negative
```

普通 `cinderx_local` 只构建本地 release wheel 并运行 CinderX Python 测试。需要额外运行
Lib/test 时显式设置：

```bash
CINDERX_LOCAL_DEPS=/path/to/cinderx-local-deps \
CINDERX_LOCAL_RUN_LIBTEST=1 \
python3.14 ci_pipeline/run_gate.py --suite cinderx_local
```

合入前可用同一开关进行双版本 L2 自验证：

```bash
export CINDERX_LOCAL_RUN_LIBTEST=1
python3.11 ci_pipeline/run_gate.py pr
python3.14 ci_pipeline/run_gate.py pr
```

其中 3.11 增加 stock/execute 72 differential，3.14 增加现有的本地 Lib/test job；
Jenkins PR 不设置该变量，因此仍保持默认快速门禁。

## Pipeline 与 Suite 组成

### Pipeline

| Pipeline | 组成 | 适用场景 |
|---|---|---|
| `pr` | `runtime` -> 覆盖率后处理 -> `cinderx_local` | 提交前本地验证 |
| `daily` | `runtime` -> 覆盖率后处理 -> `cinderx_local` + Lib/test -> compat 矩阵 | 日常完整兼容性验证 |

`--coverage` 只传递给标记为覆盖率 suite 的 native RuntimeTests；Python wheel 测试仍使用普通 release wheel。

### Suite

| Suite | 作用 | 主要输入 |
|---|---|---|
| `runtime` | 构建并运行 C++ `RuntimeTests`，可生成 native 覆盖率 | 可选 `--coverage`、`CINDERX_LOCAL_DEPS` |
| `cinderx_local` | 构建本地 release wheel，运行 CinderX Python 测试 | 可选 `CINDERX_LOCAL_RUN_LIBTEST=1` |
| `wheel_compat` | 在受支持 Python 上安装并测试外部 wheel | `CINDERX_TEST_WHEEL`、`CINDERX_TEST_PYTHON` |
| `wheel_compat_negative` | 验证不支持的 Python 版本会拒绝外部 wheel | `CINDERX_TEST_WHEEL`、`CINDERX_UNSUPPORTED_TEST_PYTHON` |

`cinderx_local` 的构建 job 会设置 `CINDERX_INCLUDE_TEST_PACKAGE_DATA=1`，确保门禁所需的测试
package data 只进入本地测试 wheel，不影响普通发布 wheel。

## 兼容性矩阵

兼容性矩阵定义在 `ci_pipeline/python_compat_matrix.toml`：

- `[[supported]]` 条目由 `wheel_compat` 使用
- `[[unsupported]]` 条目由 `wheel_compat_negative` 使用

每个条目必须包含：

- `name`
- `python`
- `version`

`daily` 会为每个矩阵条目创建独立的运行目录、venv、日志和 `summary.json`，顶层 summary 会把每个 Python 版本展示为一个独立 job。

## 常用环境变量

| 环境变量 | 作用 |
|---|---|
| `CINDERX_TEST_PYTHON` | 门禁使用的 Python 解释器；未设置时默认为当前运行 `run_gate.py` 的解释器 |
| `CINDERX_TEST_WHEEL` | `daily` compat fan-out 和 `wheel_compat` / `wheel_compat_negative` 待测的外部 wheel |
| `CINDERX_UNSUPPORTED_TEST_PYTHON` | `wheel_compat_negative` 使用的不支持 Python 解释器 |
| `CINDERX_LOCAL_RUN_LIBTEST=1` | 让 3.11 PR 增加 stock/execute 72 differential，并让 3.14 `cinderx_local` 增加本地 wheel Lib/test |
| `CINDERX_LOCAL_DEPS` | CMake FetchContent 依赖的本地缓存目录 |
| `CINDERX_PIP_WHEELHOUSE` | suite venv 引导使用的本地 Python wheelhouse |
| `CINDERX_PIP_OFFLINE=1` | 要求 pip 只从 `CINDERX_PIP_WHEELHOUSE` 安装 |
| `CINDERX_TEST_JOBS` | RuntimeTests CMake/ctest 并行度；默认使用 CPU 数 |
| `CINDERX_TESTGATE_PRELUDE` | 每个 job 前执行的 shell 片段；也可用 `--prelude` 覆盖 |
| `CINDERX_TESTGATE_ALLOW_TARGET_MISMATCH=1` | 允许当前机器与 suite target 不匹配；等价于 `--allow-target-mismatch` |

native `RuntimeTests` 默认从目标 `CINDERX_TEST_PYTHON` 的 `sysconfig` 读取
`CC/CXX`。这样链接器会和目标 Python 的静态 `libpython` 保持兼容，包括带 GCC LTO
bytecode 的 Python 构建。如果手动覆盖 `CC` 或 `CXX`，调用方必须保证该编译器能链接目标
Python 库。

## 依赖缓存与 pip 离线模式

`run_gate` 不硬编码依赖缓存路径。需要离线构建或复用预置依赖时，显式设置
`CINDERX_LOCAL_DEPS`：

```bash
export CINDERX_LOCAL_DEPS=/opt/cinderx-deps
```

缓存覆盖的 CMake FetchContent 依赖包括：

- `fmt`
- `parallel-hashmap`
- `usdt`
- `capstone`
- `googletest`

如果缓存中的依赖缺失，或者 remote/tag/commit 不匹配，CMake 会刷新对应依赖目录。未设置
`CINDERX_LOCAL_DEPS` 时，CMake 会按当前环境使用普通 FetchContent 行为。

对于 suite venv 中的 Python 包引导，使用本地 wheelhouse：

```bash
export CINDERX_PIP_WHEELHOUSE=/opt/cinderx-pydeps
export CINDERX_PIP_OFFLINE=1
```

`CINDERX_PIP_WHEELHOUSE` 至少需要包含 `pip`、`pytest` 以及 pytest 的传递依赖。

离线 ARM64 主机上的完整示例：

```bash
export CINDERX_LOCAL_DEPS=/opt/cinderx-deps
export CINDERX_PIP_WHEELHOUSE=/opt/cinderx-pydeps
export CINDERX_PIP_OFFLINE=1

python3.14 ci_pipeline/run_gate.py pr --coverage
```

## Fat Wheel 构建参数

CPython 3.14 manylinux fat wheel 构建脚本默认使用发布产物导向的参数：

```bash
python3.14 ci_pipeline/build_cp314_manylinux_fat_wheel.py
```

默认构建行为：

- `CMAKE_BUILD_TYPE=Release`
- `CINDERX_ENABLE_PGO=0`
- `CINDERX_ENABLE_LTO=0`

CI job 需要不同构建特征时显式打开：

```bash
python3.14 ci_pipeline/build_cp314_manylinux_fat_wheel.py \
  --cmake-build-type RelWithDebInfo \
  --pgo \
  --lto
```

host build manifest 会记录实际使用的 CMake build type，以及 PGO/LTO 是否开启。容器内构建脚本保持同样默认值，只有当
`CINDERX_ENABLE_PGO` 或 `CINDERX_ENABLE_LTO` 被设置为非零值时才启用对应能力。

## 覆盖率、产物与已知限制

覆盖率阈值定义在 `ci_pipeline/run_gate.py` 顶部附近的 `COVERAGE_MIN_PERCENT`，当前按 runtime-only 覆盖范围校准：

- line: 70%
- function: 60%
- branch: 40%

每次运行都会打印 artifact directory，并在其中写入日志、每个 job 的 JSON 结果、顶层 `summary.json`。覆盖率运行还会生成
`coverage/coverage.info` 和 `coverage/html/index.html`。

LCOV 兼容逻辑会在运行时按版本自动选择参数：

- LCOV 1.x 使用 `lcov_branch_coverage=1`
- LCOV 2.x 使用 `branch_coverage=1`，并在 capture、filter、HTML 生成阶段降级处理已知的第三方/template 一致性问题

已知排除项：

- `test_jit_support_instrumentation.py` 仅运行 ARM64 支持的用例
- `test_compiler_sbs_stdlib_0.py` 到 `test_compiler_sbs_stdlib_9.py` 仍作为 Kunpeng `test_cinderx` 债务，暂不纳入主门禁

请保持 HIR runtime test fixture 文件使用 LF 换行；CRLF 会导致 `runtime_tests` 里的分隔符校验失败。

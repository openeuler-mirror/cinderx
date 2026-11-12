# 快速入门

本指南通过一个加法函数，演示 CinderX 的安装、JIT 启用和编译结果检查。先准备一份示例程序，再选择对应 Python 版本的安装与启动步骤。

预编译包面向 Linux AArch64。CPython 3.11 需要使用指定的 openEuler 24.03 LTS-SP3 发行版环境；CPython 3.14 推荐使用 3.14.3。完整要求见[安装部署指南](installation_guide.md)。

## 准备示例程序

在源码仓库之外准备独立工作目录，例如 `$HOME/cinderx-demo`。从 [CinderX Releases](https://gitcode.com/openeuler/cinderx/releases) 下载所选版本的 wheel 和同名 `.sha256` 文件，放入该目录。下文以已发布的 `v3.0.0` 为例；使用其他版本时，同时替换 wheel 和校验文件。

在该目录创建 UTF-8 文件 `hello_jit.py`：

```python
import cinderx
import cinderx.jit as jit


def add(a, b):
    return a + b


print("initialized:", cinderx.is_initialized())
print("import error:", cinderx.get_import_error())
print("jit enabled:", jit.is_enabled())
if not cinderx.is_initialized():
    raise RuntimeError(f"CinderX initialization failed: {cinderx.get_import_error()}")
if not jit.is_enabled():
    raise RuntimeError("JIT is not enabled")

for _ in range(100):
    add(1, 2)

# 显式编译，让首次体验能够检查一个确定的目标函数。
# 函数可能已被自动编译，因此使用状态查询判断结果。
jit.force_compile(add)
result = add(1, 2)
compiled = jit.is_jit_compiled(add)
print("result:", result)
print("compiled:", compiled)
if result != 3:
    raise RuntimeError(f"Unexpected result: {result!r}")
if not compiled:
    raise RuntimeError("The target function was not compiled")
```

在示例目录中选择对应版本的步骤，安装成功后再启动程序。以下命令使用独立虚拟环境，假定当前 shell 没有额外设置 JIT 调试、禁用或实验参数。启动时保留 Python 默认的 site 初始化；`python -S` 会跳过该阶段，使 wheel 的自动加载入口无法执行。

## CPython 3.11

### 安装

先按[安装指南的 CPython 3.11 部分](installation_guide.md#cpython-311)检查发行版包和实际解释器，确认使用指定的 CPython 3.11.6。

```bash
wheel=cinderx-2026.11.10.0-cp311-cp311-linux_aarch64.whl
sha256sum -c "$wheel.sha256" &&
  CINDERX_PLUGIN_ENABLE=0 python3.11 -m venv .venv311 &&
  CINDERX_PLUGIN_ENABLE=0 .venv311/bin/python -m pip install --no-index --no-deps "$wheel"
```

### 启动

以下命令选择 CinderX 帧求值器（evaluator）和 `execute` 执行模式，并将自动编译的调用阈值设为 20：

```bash
CINDERX_PLUGIN_ENABLE=1 CINDERX_EVAL_MODE=cinder \
  CINDERX_JIT_MODE=execute PYTHONJITAUTO=20 \
  CINDERX_OSR_ENABLED=0 PYTHONJITLIGHTWEIGHTFRAME=0 \
  .venv311/bin/python hello_jit.py
```

CP311 不支持 `PYTHONJITAUTO=auto` 或 `auto:N` 行为分类。日常运行使用 `execute`；`observe` 和 `shadow` 是诊断模式，不执行生成的业务机器码。

## CPython 3.14

### 安装

以下步骤使用 CPython 3.14.3。发布包为包含多个补丁版本扩展的 fat wheel，具体覆盖范围见[安装指南的 CPython 3.14 部分](installation_guide.md#cpython-314)。

```bash
wheel=cinderx-2026.11.10.0-cp314-cp314-manylinux_2_24_aarch64.manylinux_2_28_aarch64.whl
sha256sum -c "$wheel.sha256" &&
  CINDERX_PLUGIN_ENABLE=0 python3.14 -m venv .venv314 &&
  CINDERX_PLUGIN_ENABLE=0 .venv314/bin/python -m pip install --no-index --no-deps "$wheel"
```

### 启动

以下命令开启 AutoJIT 行为分类，初始调用阈值为 2；首次体验使用普通帧，并保持 OSR 关闭：

```bash
CINDERX_PLUGIN_ENABLE=1 PYTHONJITAUTO=auto:2 \
  CINDERX_OSR_ENABLED=0 PYTHONJITLIGHTWEIGHTFRAME=0 \
  .venv314/bin/python hello_jit.py
```

## 检查结果

两种环境下均应看到以下结果，且进程退出码为 `0`：

```text
initialized: True
import error: None
jit enabled: True
result: 3
compiled: True
```

上述结果表示 CinderX 已初始化、JIT 已启用、加法结果正确，且目标函数处于已编译状态。本例调用了 `force_compile`，因此不用于判断自动阈值是否触发；机器码执行次数和性能收益需要另行验证。自动编译、OSR 和轻量级帧的使用见[用户指南](user_guide.md)。

若 `jit enabled` 或 `compiled` 为 `False`，先检查虚拟环境、启动参数和禁用变量，再按[常见问题](installation_guide.md#常见问题)排查。`cinderx.jit` 提供 Python 接口，导入该模块本身不足以确认底层 JIT 可用。

# 快速入门

本指南使用同一份加法示例，依次完成Python环境准备、软件包获取与安装、示例创建、JIT启动和结果检查。示例使用`v3.0.0`对应的包版本`2026.11.10.0`；CP311 RPM的发行号为`1`。

## 选择安装方式

| 安装包 | Python与系统要求 | 环境准备 |
|---|---|---|
| CP311 wheel | CPython 3.11.6，openEuler 24.03 LTS-SP3 AArch64 | 已有Python＋venv，或Conda |
| CP311 RPM | CPython 3.11.6，openEuler 24.03 LTS-SP5 AArch64 | 使用系统`/usr/bin/python3.11` |
| CP314 wheel | CPython 3.14.0—3.14.3，推荐3.14.3；Linux AArch64 | 已有Python＋venv，或Conda |

完整配套要求见[安装部署指南](installation_guide.md)。使用RPM时可直接进入下文的[安装RPM](#安装rpm)，不需要创建Conda环境。

在源码仓库外准备工作目录，例如：

```bash
mkdir -p "$HOME/cinderx-demo"
cd "$HOME/cinderx-demo"
```

## 准备Python环境

wheel用户从下面两种方式中选择一种，再进入软件包安装步骤。`CINDERX_PY`始终指向所选环境的解释器，后文安装与运行均使用它。

### 方式一：已有Python＋venv（推荐）

CP311先按[安装前检查](installation_guide.md#cpython-311)确认系统自带的Python满足配套要求，再创建虚拟环境：

```bash
CINDERX_PLUGIN_ENABLE=0 /usr/bin/python3.11 -m venv .venv311
CINDERX_PY="$PWD/.venv311/bin/python"
"$CINDERX_PY" -V
```

CP314使用已经准备好的Python 3.14.3，先核对[安装前检查](installation_guide.md#cpython-314)，再执行：

```bash
CINDERX_PLUGIN_ENABLE=0 python3.14 -m venv .venv314
CINDERX_PY="$PWD/.venv314/bin/python"
"$CINDERX_PY" -V
```

### 方式二：Conda环境（可选）

已安装并初始化Conda的用户，可从以下两组命令中选择对应版本。尚未安装Conda时，可从[Miniforge](https://github.com/conda-forge/miniforge)获取Linux AArch64安装器。

CP311：

```bash
conda create -y --override-channels -c conda-forge python=3.11.6 pip -n cinderx311
conda activate cinderx311
CINDERX_PY="$(command -v python)"
"$CINDERX_PY" -V
```

CP314：

```bash
conda create -y --override-channels -c conda-forge python=3.14.3 pip -n cinderx314
conda activate cinderx314
CINDERX_PY="$(command -v python)"
"$CINDERX_PY" -V
```

CP311 wheel仍要求其配套系统和`glibc 2.38`及以上；Conda环境中的解释器版本正确，还需满足系统动态库要求。继续安装前，完成安装指南中对应的版本、架构和环境检查。

## 获取并安装软件包

### 安装wheel

先选择与所选Python对应的一行，设置完整文件名：

| Python | 文件名设置 |
|---|---|
| CP311 | `wheel=cinderx-2026.11.10.0-cp311-cp311-linux_aarch64.whl` |
| CP314 | `wheel=cinderx-2026.11.10.0-cp314-cp314-manylinux_2_24_aarch64.manylinux_2_28_aarch64.whl` |

取包可选择网页下载或命令行下载：

- 网页下载：从[CinderX Releases](https://gitcode.com/openeuler/cinderx/releases)的`v3.0.0`页面取得所选wheel和同名`.sha256`文件，放到当前目录。
- 命令行下载：设置上表中的`wheel`后执行：

```bash
release=v3.0.0
base_url="https://gitcode.com/openeuler/cinderx/releases/download/$release"
curl --fail --location --retry 3 --output "$wheel" "$base_url/$wheel" &&
  curl --fail --location --retry 3 --output "$wheel.sha256" "$base_url/$wheel.sha256"
```

文件准备好后，校验并安装到已选环境：

```bash
sha256sum -c "$wheel.sha256" &&
  CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip install --no-index --no-deps "$wheel" &&
  CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip check
```

### 安装RPM

在openEuler 24.03 LTS-SP5 AArch64上，先按[安装部署指南](installation_guide.md#cpython-311-rpm)确认系统Python为3.11.6。软件源配置和指定版本安装见该指南；已有本地RPM和校验文件时，在具备软件包管理权限的shell中执行：

```bash
rpm_file=python3-cinderx-2026.11.10.0-1.aarch64.rpm
sha256sum -c "$rpm_file.sha256" &&
  CINDERX_PLUGIN_ENABLE=0 dnf install "./$rpm_file" &&
  rpm -q python3-cinderx
```

安装后，以普通业务用户选择系统解释器：

```bash
CINDERX_PY=/usr/bin/python3.11
```

普通venv默认看不到系统RPM。需要在venv中复用时，按安装指南使用`--system-site-packages`新建环境。RPM由`dnf`维护。

## 准备示例程序

安装成功后，在当前工作目录中创建UTF-8文件`hello_jit.py`：

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

## 启动程序

假定当前shell没有额外设置JIT调试、禁用或实验参数。启动时保留默认的site初始化；`python -S`会跳过自动加载入口。重新打开终端后，先恢复Conda激活状态或将`CINDERX_PY`设为目标解释器的绝对路径。

### CPython 3.11

wheel与RPM使用相同的CP311启动参数。以下命令选择CinderX帧求值器和`execute`执行模式，将自动编译调用阈值设为20：

```bash
CINDERX_PLUGIN_ENABLE=1 CINDERX_EVAL_MODE=cinder \
  CINDERX_JIT_MODE=execute PYTHONJITAUTO=20 \
  CINDERX_OSR_ENABLED=0 PYTHONJITLIGHTWEIGHTFRAME=0 \
  "$CINDERX_PY" hello_jit.py
```

CP311使用数值调用阈值，不支持`auto`／`auto:N`行为分类。`observe`和`shadow`用于诊断，日常运行选择`execute`。

### CPython 3.14

以下命令在推荐的3.14.3上开启AutoJIT行为分类，初始调用阈值为2，并使用普通帧、关闭OSR：

```bash
CINDERX_PLUGIN_ENABLE=1 PYTHONJITAUTO=auto:2 \
  CINDERX_OSR_ENABLED=0 PYTHONJITLIGHTWEIGHTFRAME=0 \
  "$CINDERX_PY" hello_jit.py
```

## 检查结果

应看到以下结果，且进程退出码为`0`：

```text
initialized: True
import error: None
jit enabled: True
result: 3
compiled: True
```

上述结果表示初始化和JIT启用成功、计算结果正确，且目标函数处于已编译状态。本例显式调用`force_compile`，不用于判断自动调用阈值是否触发；机器码执行次数和性能收益需要另行验证。

若`jit enabled`或`compiled`为`False`，先检查解释器、包的可见性、启动参数和禁用变量，再按[常见问题](installation_guide.md#常见问题)排查。自动编译、OSR和轻量级帧的使用见[用户指南](user_guide.md)。

# 安装部署指南

本指南介绍 CinderX 二进制安装包（wheel、RPM）的获取、安装、升级、回退和卸载。先按系统、Python 版本和安装方式选择步骤：wheel 安装到选定的 venv 或 Conda 环境，CPython 3.11 RPM 由系统软件包管理器安装。

## 配套版本与获取软件包

从 [CinderX Releases](https://gitcode.com/openeuler/cinderx/releases) 选择正式版本，下载与解释器和架构匹配的 wheel 及同名 `.sha256` 文件。版本映射、已知限制和升级变化见[版本说明书](release_notes.md)。

以下以 `v3.0.0` 为例：

| 项目 | 内容 |
|---|---|
| 产品版本 | `v3.0.0` |
| wheel 元数据版本 | `2026.11.10.0` |
| CP311 RPM 版本 | `2026.11.10.0-1`，包名 `python3-cinderx` |
| 架构 | Linux AArch64／ARM64 |
| wheel 来源 | `openeuler/cinderx` Release 附件 |
| RPM 来源 | 发布方提供的 RPM 软件源；源地址待提供，也可安装已取得的本地 RPM |
| 校验方式 | wheel 使用同一 Release 的 SHA256 文件；本地 RPM 使用配套的同名 `.sha256` 文件 |

### wheel 下载方式

网页下载和命令行下载任选一种，得到的文件相同。网页下载使用上面的 Release 入口；使用命令行时，先按目标 Python 选择文件名：

| Python | 文件名设置 |
|---|---|
| CP311 | `wheel=cinderx-2026.11.10.0-cp311-cp311-linux_aarch64.whl` |
| CP314 | `wheel=cinderx-2026.11.10.0-cp314-cp314-manylinux_2_24_aarch64.manylinux_2_28_aarch64.whl` |

在工作目录中执行，两个下载都成功后校验：

```bash
release=v3.0.0
base_url="https://gitcode.com/openeuler/cinderx/releases/download/$release"
curl --fail --location --retry 3 --output "$wheel" "$base_url/$wheel" &&
  curl --fail --location --retry 3 --output "$wheel.sha256" "$base_url/$wheel.sha256" &&
  sha256sum -c "$wheel.sha256"
```

使用其他版本时，同时替换 Release、完整 wheel 文件名和校验文件。下面的安装步骤使用已下载并校验的本地文件。

本分支的发布包与上游 PyPI 包有各自的版本和支持范围。需要本分支能力时，使用对应 Release 的明确文件；不要只执行无版本限制的 `pip install cinderx` 后假定拿到相同制品。

为避免源码路径干扰已安装包的检查，以下命令在目标 Linux 主机的仓库外目录中执行，例如 `$HOME/cinderx-demo`。wheel 步骤可选择已有 Python＋venv 或 Conda，`pip` 始终通过所选环境的 `CINDERX_PY` 调用。RPM 步骤使用系统 `/usr/bin/python3.11`，安装、重装和卸载由具备系统软件包管理权限的用户执行。下载目录中保留准确的文件名，不用跨 Python 版本的 wheel 通配符安装。每一步成功后再继续；校验或安装失败时先处理错误。

包安装、重装和卸载命令显式设置 `CINDERX_PLUGIN_ENABLE=0`，避免包管理进程因继承业务环境而自动加载当前本地扩展。业务验证使用对应 Python 版本的完整启动配置，并保留默认的 site 初始化；`-S` 会跳过自动加载入口。

## CPython 3.11

本节使用 wheel；使用 RPM 时直接进入 [CPython 3.11 RPM](#cpython-311-rpm)。

### 方式一：系统 Python＋venv（推荐）

支持环境为 openEuler 24.03 LTS-SP3 AArch64，使用以下指定版本的 CPython 3.11.6 发行版包。运行环境使用 `python3-3.11.6-34.oe2403sp3`；构建或使用头文件、测试 C API 时，使用版本和发行号相同的 `python3-devel`。

```bash
cat /etc/os-release
uname -m
rpm -q python3
test "$(rpm -q --queryformat '%{VERSION}-%{RELEASE}' python3)" = \
  "3.11.6-34.oe2403sp3" &&
CINDERX_PLUGIN_ENABLE=0 /usr/bin/python3.11 - <<'PY'
from pathlib import Path
import platform
import sys
import sysconfig

print(sys.version)
print(platform.machine())
print(sysconfig.get_config_var("SOABI"))
print(sysconfig.get_config_var("Py_ENABLE_SHARED"))
if platform.python_implementation() != "CPython" or sys.version_info[:3] != (3, 11, 6):
    raise SystemExit("Requires CPython 3.11.6")
if Path(sys.executable).resolve() != Path("/usr/bin/python3.11").resolve():
    raise SystemExit("Use the anchored distribution interpreter")
if platform.machine() != "aarch64" or sysconfig.get_config_var("Py_ENABLE_SHARED") != 1:
    raise SystemExit("Requires AArch64 and the distribution shared libpython")
PY
```

确认发行版、RPM 版本、实际解释器路径与架构均符合要求。RPM 比较或 Python 检查返回非零时停止安装；其他发行版或自行编译的同号 Python 不因版本号相同就自动兼容。

检查通过后创建虚拟环境，并选择该解释器：

```bash
CINDERX_PLUGIN_ENABLE=0 /usr/bin/python3.11 -m venv .venv311
CINDERX_PY="$PWD/.venv311/bin/python"
```

### 方式二：Conda 环境（可选）

已安装并初始化 Conda 时，可直接准备 CPython 3.11.6 环境。没有 Conda 时，可先安装 [Miniforge 的 Linux AArch64 版本](https://github.com/conda-forge/miniforge)。本方式用于安装 wheel；源码构建仍使用编译构建指南规定的发行版解释器、头文件和工具链。

```bash
conda create -y --override-channels -c conda-forge python=3.11.6 pip -n cinderx311
conda activate cinderx311
CINDERX_PY="$(command -v python)"
getconf GNU_LIBC_VERSION
CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" - <<'PY'
import platform
import sys
import sysconfig

print(sys.version)
print(sys.executable)
print(sysconfig.get_config_var("SOABI"))
print("libc:", platform.libc_ver())
if platform.python_implementation() != "CPython" or sys.version_info[:3] != (3, 11, 6):
    raise SystemExit("Requires CPython 3.11.6")
if platform.machine() != "aarch64" or sysconfig.get_config_var("Py_DEBUG"):
    raise SystemExit("Requires AArch64 and a non-debug interpreter")
libc, version = platform.libc_ver()
if libc != "glibc" or tuple(int(x) for x in version.split(".")[:2]) < (2, 38):
    raise SystemExit("This CP311 wheel requires glibc 2.38 or later")
PY
```

CP311 wheel 的系统配套要求仍为 openEuler 24.03 LTS-SP3 AArch64。Conda 准备 Python 环境后，系统动态库也需满足要求；只升级环境内的 Python 不能补齐缺失的 `GLIBC_2.38`。Conda 运行环境使用自己的解释器，不套用前一方式中的 `/usr/bin/python3.11` 和发行版 RPM NVR 检查。

### 安装 wheel

在 wheel 与校验文件所在目录执行：

```bash
wheel=cinderx-2026.11.10.0-cp311-cp311-linux_aarch64.whl
sha256sum -c "$wheel.sha256" &&
  CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip install --no-index --no-deps "$wheel" &&
  CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip show cinderx
```

使用系统 Python＋venv 时，需要相应系统包提供 `venv`／`ensurepip` 和 pip；Conda 环境在创建时安装 pip。先确认 `CINDERX_PY` 指向所选环境，避免安装到其他解释器。

### 启动并验证

准备[快速入门](quick_start.md#准备示例程序)中的 `hello_jit.py`，在新的进程中执行：

```bash
CINDERX_PLUGIN_ENABLE=1 CINDERX_EVAL_MODE=cinder \
  CINDERX_JIT_MODE=execute PYTHONJITAUTO=20 \
  CINDERX_OSR_ENABLED=0 PYTHONJITLIGHTWEIGHTFRAME=0 \
  "$CINDERX_PY" hello_jit.py
```

CP311 的启动配置包括插件加载、帧求值器选择和 JIT 执行模式。日常使用按上例选择 `execute` 和数值调用阈值；`observe`、`shadow` 仅用于诊断。该版本不支持 `auto`／`auto:N` 行为分类、OSR 或轻量级帧执行。

## CPython 3.11 RPM

### 安装前检查

RPM 用于 openEuler 24.03 LTS-SP5 AArch64，要求发行版提供的非调试 CPython 3.11.6。本包不适用于 CPython 3.14，也不适用于其他 3.11 补丁版本。运行安装不要求 `python3-devel`。

```bash
cat /etc/os-release
uname -m
rpm -q python3
CINDERX_PLUGIN_ENABLE=0 /usr/bin/python3.11 - <<'PY'
from pathlib import Path
import platform
import sys
import sysconfig

print(sys.version)
print(sys.executable)
print(sysconfig.get_config_var("SOABI"))
if platform.python_implementation() != "CPython" or sys.version_info[:3] != (3, 11, 6):
    raise SystemExit("Requires CPython 3.11.6")
if Path(sys.executable).resolve() != Path("/usr/bin/python3.11").resolve():
    raise SystemExit("Use the distribution /usr/bin/python3.11")
if platform.machine() != "aarch64" or sysconfig.get_config_var("Py_DEBUG"):
    raise SystemExit("Requires AArch64 and a non-debug interpreter")
if sysconfig.get_config_var("Py_ENABLE_SHARED") != 1:
    raise SystemExit("Requires the distribution shared libpython")
PY
```

核对系统信息为目标 openEuler 24.03 LTS-SP5，Python 版本、解释器路径和架构均符合要求后继续。RPM 会检查 Python 与动态库依赖；安装成功后还需执行下文的启动验证。

### 获取并安装 RPM

软件源地址由发布方提供，目前待提供。取得源配置后，按发布方说明启用该软件源，再查询可用版本并安装指定版本：

```bash
dnf list --showduplicates python3-cinderx
CINDERX_PLUGIN_ENABLE=0 dnf install python3-cinderx-2026.11.10.0-1.aarch64
```

尚未配置软件源时，可将安装包 `python3-cinderx-2026.11.10.0-1.aarch64.rpm` 和同名 `.sha256` 文件放入工作目录，校验后从本地安装。以下 `dnf` 软件包管理命令在具备系统软件包管理权限的 shell 中执行：

```bash
rpm_file=python3-cinderx-2026.11.10.0-1.aarch64.rpm
sha256sum -c "$rpm_file.sha256" &&
  CINDERX_PLUGIN_ENABLE=0 dnf install "./$rpm_file" &&
  rpm -q python3-cinderx &&
  rpm -V python3-cinderx
```

软件包完整且未被修改时，`rpm -V` 无输出并返回 `0`。完全离线安装要求系统已经提供全部依赖，可在安装命令中增加 `--disablerepo='*'`；缺少依赖时先从配套软件源补齐，不使用 `--nodeps` 跳过检查。

### 启动并验证

RPM 安装到系统 `/usr/lib64/python3.11/site-packages`。先查询安装信息和实际导入位置：

```bash
rpm -q python3-cinderx
CINDERX_PLUGIN_ENABLE=0 /usr/bin/python3.11 - <<'PY'
from pathlib import Path
import importlib.metadata as metadata
import cinderx
import _cinderx

print("package version:", metadata.version("cinderx"))
print("cinderx path:", cinderx.__file__)
print("native path:", _cinderx.__file__)
print("initialized:", cinderx.is_initialized())
print("import error:", cinderx.get_import_error())
if metadata.version("cinderx") != "2026.11.10.0":
    raise SystemExit("Unexpected CinderX package version")
root = Path("/usr/lib64/python3.11/site-packages")
for module in (cinderx, _cinderx):
    if not Path(module.__file__).resolve().is_relative_to(root):
        raise SystemExit(f"{module.__name__} is loaded from another environment")
if cinderx.get_import_error() is not None or not cinderx.is_initialized():
    raise SystemExit("CinderX initialization failed")
PY
```

准备[快速入门](quick_start.md#准备示例程序)中的 `hello_jit.py`，使用系统解释器启动新进程：

```bash
CINDERX_PLUGIN_ENABLE=1 CINDERX_EVAL_MODE=cinder \
  CINDERX_JIT_MODE=execute PYTHONJITAUTO=20 \
  CINDERX_OSR_ENABLED=0 PYTHONJITLIGHTWEIGHTFRAME=0 \
  /usr/bin/python3.11 hello_jit.py
```

成功时应看到 `initialized: True`、`import error: None`、`jit enabled: True`、`result: 3` 和 `compiled: True`。安装 RPM 不会自动为所有 Python 进程开启 JIT，业务启动时仍需设置上述参数，且保留默认的 site 初始化。

### 在虚拟环境中使用系统 RPM

普通 venv 默认隔离系统 `site-packages`，看不到通过 RPM 安装的 CinderX。需要在虚拟环境中复用系统 RPM 时，使用同一个发行版解释器新建允许访问系统包的环境：

```bash
CINDERX_PLUGIN_ENABLE=0 /usr/bin/python3.11 -m venv --system-site-packages .venv311-rpm
CINDERX_PLUGIN_ENABLE=1 CINDERX_EVAL_MODE=cinder \
  CINDERX_JIT_MODE=execute PYTHONJITAUTO=20 \
  CINDERX_OSR_ENABLED=0 PYTHONJITLIGHTWEIGHTFRAME=0 \
  .venv311-rpm/bin/python hello_jit.py
```

该环境同时能访问其他系统 Python 包。需要完全隔离依赖时，选择 wheel 安装方式。RPM 管理的 CinderX 文件通过 `dnf` 维护，不在系统解释器中用 `pip` 覆盖或卸载同名包。

### 重装、升级、回退与卸载

维护前记录 `rpm -q python3-cinderx`、原 RPM 及其校验文件、启动配置和业务验证结果，并停止使用该包的相关进程。同版本重装可执行：

```bash
rpm_file=python3-cinderx-2026.11.10.0-1.aarch64.rpm
sha256sum -c "$rpm_file.sha256" &&
  CINDERX_PLUGIN_ENABLE=0 dnf reinstall "./$rpm_file" &&
  rpm -V python3-cinderx
```

升级时使用发布方提供的新 RPM，先核对其系统、Python 配套要求并校验文件，再执行 `CINDERX_PLUGIN_ENABLE=0 dnf upgrade /absolute/path/to/new-package.rpm`。需要回退时，对保留的旧 RPM 执行相同的配套检查和校验，再执行 `CINDERX_PLUGIN_ENABLE=0 dnf downgrade /absolute/path/to/old-package.rpm`。维护后启动新进程，重新运行最小示例和业务检查。

临时禁用 JIT 或 CinderX 分别使用 `PYTHONJITDISABLE=1`、`CINDERX_DISABLE=1`，含义见[用户指南](user_guide.md#通用配置与-api)。卸载使用：

```bash
CINDERX_PLUGIN_ENABLE=0 dnf remove python3-cinderx
```

随后执行 `rpm -q python3-cinderx`，应提示未安装并返回非零。在仓库外启动新的 `/usr/bin/python3.11` 进程，确认没有其他路径提供同名模块；共享系统包的 venv 也会随系统 RPM 卸载而失去该包。

## CPython 3.14

### 准备 Python 环境

已有符合要求的 Python 时，推荐创建 venv：

```bash
CINDERX_PLUGIN_ENABLE=0 python3.14 -m venv .venv314
CINDERX_PY="$PWD/.venv314/bin/python"
```

也可用已安装并初始化的 Conda 准备推荐的 3.14.3：

```bash
conda create -y --override-channels -c conda-forge python=3.14.3 pip -n cinderx314
conda activate cinderx314
CINDERX_PY="$(command -v python)"
```

下面的环境检查、安装和启动命令都使用所选解释器。RPM 不用于 CPython 3.14。

### 安装前检查

发布包包含 CPython 3.14.0—3.14.3 对应的本地扩展，称为 fat wheel，推荐使用 3.14.3。目标平台为 Linux AArch64，推荐 openEuler 24.03 LTS-SP3。系统 C 库及其他动态库仍需满足 wheel 的平台和依赖要求。

```bash
cat /etc/os-release
uname -m
CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" - <<'PY'
import platform
import sys
import sysconfig

print(sys.version)
print(platform.machine())
print(sysconfig.get_config_var("SOABI"))
print(sysconfig.get_config_var("Py_GIL_DISABLED"))
if platform.python_implementation() != "CPython" or sys.version_info[:2] != (3, 14):
    raise SystemExit("Requires CPython 3.14")
if not 0 <= sys.version_info.micro <= 3:
    raise SystemExit("The wheel covers CPython 3.14.0 through 3.14.3")
if platform.machine() != "aarch64" or sysconfig.get_config_var("Py_GIL_DISABLED"):
    raise SystemExit("Requires AArch64 and a GIL-enabled interpreter")
PY
```

加载器按补丁版本（micro，例如 3.14.3 中的末位 3）选择包内扩展，因此该 wheel 不支持 3.14.4 及后续版本，也不用于自由线程版本（free-threaded Python）。CPython 3.14.0 的直接调用路径绕过 AutoJIT 分类准入；评估分类策略时建议使用 3.14.3。

### 安装 wheel

在下载目录执行：

```bash
wheel=cinderx-2026.11.10.0-cp314-cp314-manylinux_2_24_aarch64.manylinux_2_28_aarch64.whl
sha256sum -c "$wheel.sha256" &&
  CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip install --no-index --no-deps "$wheel" &&
  CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip show cinderx
```

### 启动并验证

执行同一份 `hello_jit.py`。这里使用数值阈值，检查所支持补丁版本上的普通 JIT 路径；推荐 3.14.3 上的分类模式示例见快速入门：

```bash
CINDERX_PLUGIN_ENABLE=1 PYTHONJITAUTO=20 \
  CINDERX_OSR_ENABLED=0 PYTHONJITLIGHTWEIGHTFRAME=0 \
  "$CINDERX_PY" hello_jit.py
```

CP314 使用上述启动参数即可。先以普通帧、OSR 关闭的配置完成基础验证，再按[用户指南](user_guide.md#cpython-314)选择其他能力。

## 安装结果与文件变化

本节适用于 wheel 安装；RPM 的查询和验证见 [CPython 3.11 RPM](#cpython-311-rpm)。完成所选版本的 wheel 安装后，`CINDERX_PY` 指向所选 venv 或 Conda 环境的 Python。若重新打开终端，请先将它设为该解释器的绝对路径。以下检查适用于两个版本：

```bash
"$CINDERX_PY" - <<'PY' && CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip check
import importlib.metadata as metadata
import cinderx

print("package version:", metadata.version("cinderx"))
print("cinderx path:", cinderx.__file__)
print("initialized:", cinderx.is_initialized())
print("import error:", cinderx.get_import_error())
if cinderx.get_import_error() is not None or not cinderx.is_initialized():
    raise SystemExit("CinderX initialization failed; inspect the error above")
import _cinderx

print("native entry:", _cinderx.__file__)
PY
```

本节用于包身份和导入检查；初始化失败会返回非零，并停止后续依赖检查。JIT 是否开启及目标是否编译，使用前面带完整启动参数的最小程序判断。

| 文件／目录 | 说明 |
|---|---|
| `cinderx/` | Python 接口、编译器等包内容 |
| `_cinderx_auto.py` 与 `cinderx.pth` | Python site 启动阶段的自动插件入口 |
| `_cinderx*.so` | CP311 wheel 的顶层本地扩展 |
| `_cinderx.py` | CP314 fat wheel 的顶层加载器，负责选择匹配的扩展 |
| `cinderx/_native/` | CP311 构建记录；CP314 fat wheel 的各补丁版本扩展及 `fat_wheel.json` |
| `__static__/`、`__strict__/`、`opcodes/` | 兼容入口和 opcode 辅助包；包存在不等于所有平台均支持其功能 |
| `cinderx-<version>.dist-info/` | 包版本、安装记录和文件清单 |

检查包版本和功能时，以目标解释器实际导入的安装路径为准。手工复制其他构建的 `.so` 可能破坏包的一致性，应通过完整 wheel 安装。

## 升级与回退

本节为两个 Python 版本的 wheel 维护流程，沿用目标环境的 `CINDERX_PY`。RPM 通过前文的 `dnf` 命令维护。业务验证仍使用前面对应版本的启动参数。升级前先阅读目标版本说明，确认目标 wheel 支持当前 Python 补丁版本和系统。

### 保存旧环境信息

```bash
CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip freeze > requirements-before.txt
CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip show cinderx > cinderx-before.txt
```

保留原 wheel、对应校验文件、服务启动配置及业务验证结果。`pip freeze` 记录依赖版本，不替代旧 wheel 备份。优先新建环境安装候选包、验证后再切换服务，以便保留原环境回退。

### 在原环境升级

先停止使用该环境的相关服务或任务。在下面两行填写新包和已保存旧包的实际绝对路径；同一测试环境选择兼容的 wheel：

```bash
NEW_WHEEL=/absolute/path/to/new/cinderx-package.whl
OLD_WHEEL=/absolute/path/to/old/cinderx-package.whl

(cd "$(dirname "$NEW_WHEEL")" && sha256sum -c "$(basename "$NEW_WHEEL").sha256") &&
  CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip install --no-index --no-deps --upgrade "$NEW_WHEEL" &&
  CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip show cinderx &&
  CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip check
```

如果以相同包版本替换了不同构建，pip 可能认为已经安装；确认确需替换后使用 `--force-reinstall`，并记录新旧哈希。安装完成后启动新进程，执行对应版本的 `hello_jit.py` 和业务验证，再恢复服务。

### 回退到旧包

```bash
(cd "$(dirname "$OLD_WHEEL")" && sha256sum -c "$(basename "$OLD_WHEEL").sha256") &&
  CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip install --no-index --no-deps --force-reinstall "$OLD_WHEEL" &&
  CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip show cinderx &&
  CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip check
```

恢复旧启动配置，启动新进程，并使用升级前适用于旧版本的检查验证回退结果；新增于新版本的功能不作为旧版的成功判据。如果 Python 补丁版本、系统或业务依赖也发生变化，恢复完整旧环境，不只替换 CinderX wheel。更新磁盘上的扩展文件不会替换已运行进程中加载的代码，因此升级和回退后都必须重启相关进程。

## 禁用与卸载

需要暂时关闭 JIT 时，在原启动命令中增加 `PYTHONJITDISABLE=1`，然后启动新进程。需要关闭 CinderX 的常规加载和初始化时，使用 `CINDERX_DISABLE=1`，并检查应用是否显式调用依赖 CinderX 的功能。单独移除自动插件开关只影响自动加载，应用显式导入仍需单独核对。

以下命令卸载所选 Python 环境中的 wheel；RPM 使用前文的 `dnf remove python3-cinderx`。停止相关进程后执行：

```bash
CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip uninstall -y cinderx &&
  CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" - <<'PY'
from importlib.metadata import PackageNotFoundError, version

try:
    installed = version("cinderx")
except PackageNotFoundError:
    print("uninstalled: True")
else:
    raise SystemExit(f"CinderX metadata is still visible: {installed}")
PY
```

成功时输出 `uninstalled: True` 并返回 `0`；仍能看到包元数据时返回非零。继续用新进程检查业务和搜索路径，避免源码目录、`PYTHONPATH` 或另一个 site-packages 继续提供同名模块。

## 常见问题

| 现象 | 检查与处理 |
|---|---|
| CP311 导入提示缺少 `GLIBC_2.38` | 核对系统 C 库版本和 wheel 配套环境；创建 Conda 环境不会更换宿主系统的 glibc |
| wheel 不支持当前平台 | 核对 `cp311`／`cp314`、AArch64、Python 补丁版本及发行版要求，不修改文件名强行安装 |
| RPM 已安装，但 venv 中找不到包 | 使用 `/usr/bin/python3.11`，或用该解释器创建带 `--system-site-packages` 的 venv，并检查实际导入路径 |
| RPM 安装提示依赖不满足 | 核对 openEuler 24.03 LTS-SP5、AArch64、CPython 3.11.6 及配套动态库，从目标系统软件源补齐依赖 |
| `pip` 显示已装，但运行时找不到包 | 比对 `CINDERX_PY`、`python -m pip show` 和实际导入路径，避免多个 Python 混用 |
| 本地扩展导入报 `undefined symbol`／依赖缺失 | 核对包来源、解释器 ABI 和系统动态库；保留完整错误，不从其他环境拼接扩展 |
| CP314 加载器拒绝当前补丁版本 | 使用包实际覆盖的 3.14.0—3.14.3，或使用支持目标补丁版本的正式制品 |
| 导入成功但 JIT 未开启 | 检查禁用变量；CP311 核对帧求值器和 `execute`，两条线均运行最小程序 |
| CP311 配置 `auto:2` 后报错 | 改用数值阈值；分类模式仅用于对应支持的 CP314 路径 |
| 轻量级帧与 OSR 同开失败 | 选择一种帧／OSR 配置，见用户指南；CP311 使用普通帧 |
| 升级后仍表现为旧版本 | 核对导入路径和包哈希，确认业务启动了新进程 |

定位问题时收集所选 Release／SHA、wheel 或 RPM 的版本与哈希、系统和 Python 配置、启动命令及完整错误，参见[运行维护与故障处理](user_guide.md#运行维护与故障处理)。

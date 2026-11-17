# 安装部署指南

本指南介绍CinderX二进制安装包（wheel、RPM）的获取、安装、升级、回退和卸载。先按系统、Python版本和安装方式选择步骤：wheel安装到选定的venv或Conda环境，CPython 3.11 RPM由系统软件包管理器安装。

## 配套版本

以下以`v3.0.0`为例：

| 项目 | 内容 |
|---|---|
| 产品版本 | `v3.0.0` |
| wheel元数据版本 | `2026.11.10.0` |
| CP311 RPM版本 | `2026.11.10.0-1`，包名`python3-cinderx` |
| 架构 | Linux AArch64/ARM64 |
| wheel来源 | `openeuler/cinderx` Release附件 |
| RPM来源 | 暂未公开发布。已取得RPM时，参见[本地安装步骤](#获取并安装rpm)安装。 |
| 校验方式 | wheel使用同一Release的SHA256文件。通过软件源安装的RPM由DNF自动校验。 |

## CPython 3.11

本节使用wheel；使用RPM时直接进入[CPython 3.11 RPM](#cpython-311-rpm)。

### 准备环境

#### 方式一：系统Python＋venv（推荐）

支持环境为openEuler 24.03 LTS-SP3 AArch64，使用以下指定版本的CPython 3.11.6发行版包。运行环境使用`python3-3.11.6-34.oe2403sp3`；构建或使用头文件、测试C API时，使用版本和发行号相同的`python3-devel`。

执行以下命令行检查当前操作系统的软硬件环境及Python解释器配置，已验证其是否严格满足运行特定版本Cinderx插件的兼容性要求。

1. 检查基础环境。

    1. 查询详细发行版信息。

        ```bash
        cat /etc/os-release
        ```

        预期输出：

        ```output
        NAME="openEuler"
        VERSION="24.03 (LTS-SP3)"
        ID="openEuler"
        VERSION_ID="24.03"
        PRETTY_NAME="openEuler 24.03 (LTS-SP3)"
        ANSI_COLOR="0;31"
        ```

    2. 查询CPU硬件架构。

        ```bash
        uname -m
        ```

        预期输出：

        ```output
        aarch64
        ```

    3. 查询RPM包管理器安装的Python版本号。

        ```bash
        rpm -q python3
        ```

        预期输出：

        ```output
        python3-3.11.6-34.oe2403sp3.aarch64
        ```

2. 校验Python运行时与编译参数。

    ```bash
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

    预期输出：

    ```output
    3.11.6 (main, Jul  8 2026, 04:34:09) [GCC 12.3.1 (openEuler 12.3.1-110.oe2403sp3)]
    aarch64
    cpython-311-aarch64-linux-gnu
    1
    ```

    确认发行版、RPM版本、实际解释器路径与架构均符合要求。RPM比较或Python检查返回非零时停止安装；其他发行版或自行编译的同号Python不因版本号相同就自动兼容。

3. 创建独立虚拟环境。

    ```bash
    # 在源码仓库外准备工作目录。
    mkdir -p "$HOME/cinderx-demo"
    cd "$HOME/cinderx-demo"

    # 创建虚拟环境
    CINDERX_PLUGIN_ENABLE=0 /usr/bin/python3.11 -m venv .venv311
    # 定义快捷路径变量
    CINDERX_PY="$PWD/.venv311/bin/python"
    ```

#### 方式二：Conda环境（可选）

已安装并初始化Conda时，可直接准备CPython 3.11.6环境。没有Conda时，可先安装[Miniforge的Linux AArch64版本](https://github.com/conda-forge/miniforge)。本方式用于安装wheel；源码构建仍使用编译构建指南规定的发行版解释器、头文件和工具链。

1. 创建并激活Conda虚拟环境。

    ```bash
    # 创建并激活Conda虚拟环境
    conda create -y --override-channels -c conda-forge python=3.11.6 pip -n cinderx311
    conda activate cinderx311

    # 记录解释器路径
    CINDERX_PY="$(command -v python)"
    ```

2. 检查底层库并校验Python兼容性。

    1. 查询系统glibc版本。

        ```bash
        # 检查系统底层库
        getconf GNU_LIBC_VERSION
        ```

        预期输出：

        ```output
        glibc 2.38
        ```

    2. 检验Python运行时与ABI参数。

        ```bash
        # 运行时与兼容性校验
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

    CP311 wheel的系统配套要求仍为openEuler 24.03 LTS-SP3 AArch64。Conda准备Python环境后，系统动态库也需满足要求；只升级环境内的Python不能补齐缺失的`GLIBC_2.38`。Conda运行环境使用自己的解释器，不套用前一方式中的`/usr/bin/python3.11`和发行版RPM NVR检查。

### 获取软件包

网页下载和命令行下载任选一种，得到的文件相同。

1. 下载软件包。

    - 网页下载

      从[CinderX Releases](https://gitcode.com/openeuler/cinderx/releases)选择正式版本，下载与解释器和架构匹配的wheel及同名`.sha256`文件。版本映射、已知限制和升级变化见[版本说明书](https://gitcode.com/openeuler/cinderx/releases)。
    
    - 命令行下载

      先按目标Python选择文件名。

      | Python | 文件名设置 |
      |---|---|
      | CP311 | `wheel=cinderx-2026.11.10.0-cp311-cp311-linux_aarch64.whl` |

2. 在工作目录中执行，校验软件包。

    ```bash
    release=v3.0.0
    base_url="https://gitcode.com/openeuler/cinderx/releases/download/$release"
    curl --fail --location --retry 3 --output "$wheel" "$base_url/$wheel" &&
      curl --fail --location --retry 3 --output "$wheel.sha256" "$base_url/$wheel.sha256" &&
      sha256sum -c "$wheel.sha256"
    ```

### 注意事项

- 使用其他版本时，同时替换Release、完整wheel文件名和校验文件。下面的安装步骤使用已下载并校验的本地文件。

- 本分支的发布包与上游PyPI包有各自的版本和支持范围。需要本分支能力时，使用对应Release的明确文件；不要只执行无版本限制的`pip install cinderx`后假定拿到相同制品。

- 为避免源码路径干扰已安装包的检查，以下命令在目标Linux主机的仓库外目录中执行，例如`$HOME/cinderx-demo`。

- wheel步骤可选择已有Python＋venv或Conda，`pip`始终通过所选环境的`CINDERX_PY`调用。RPM步骤使用系统`/usr/bin/python3.11`，安装、重装和卸载由具备系统软件包管理权限的用户执行。下载目录中保留准确的文件名，不用跨Python版本的wheel通配符安装。每一步成功后再继续；校验或安装失败时先处理错误。

- 包安装、重装和卸载命令显式设置`CINDERX_PLUGIN_ENABLE=0`，避免包管理进程因继承业务环境而自动加载当前本地扩展。业务验证使用对应Python版本的完整启动配置，并保留默认的site初始化；`-S`会跳过自动加载入口。

### 安装wheel

在wheel与校验文件所在目录执行。

```bash
# 定义安装包文件名变量
wheel=cinderx-2026.11.10.0-cp311-cp311-linux_aarch64.whl
# 校验文件完整性
sha256sum -c "$wheel.sha256"
# 离线无依赖安装cinderx
CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip install --no-index --no-deps "$wheel"
# 检查安装包信息
CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip show cinderx
```

使用系统Python＋venv时，需要相应系统包提供`venv`／`ensurepip`和pip；Conda环境在创建时安装pip。先确认`CINDERX_PY`指向所选环境，避免安装到其他解释器。

### 验证安装结果

本节适用于wheel安装；RPM的查询和验证见[CPython 3.11 RPM](#cpython-311-rpm)。完成所选版本的wheel安装后，`CINDERX_PY`指向所选venv或Conda环境的Python。若重新打开终端，请先将它设为该解释器的绝对路径。以下检查适用于两个版本：

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

本节用于包身份和导入检查；初始化失败会返回非零，并停止后续依赖检查。JIT是否开启及目标是否编译，使用前面带完整启动参数的最小程序判断。

| 文件／目录 | 说明 |
|---|---|
| `cinderx/` | Python接口、编译器等包内容 |
| `_cinderx_auto.py`与`cinderx.pth` | Python site启动阶段的自动插件入口 |
| `_cinderx*.so` | CP311 wheel的顶层本地扩展 |
| `_cinderx.py` | CP314 fat wheel的顶层加载器，负责选择匹配的扩展 |
| `cinderx/_native/` | CP311构建记录；CP314 fat wheel的各补丁版本扩展及`fat_wheel.json` |
| `__static__/`、`__strict__/`、`opcodes/` | 兼容入口和opcode辅助包；包存在不等于所有平台均支持其功能 |
| `cinderx-<version>.dist-info/` | 包版本、安装记录和文件清单 |

检查包版本和功能时，以目标解释器实际导入的安装路径为准。手工复制其他构建的`.so`可能破坏包的一致性，应通过完整wheel安装。

### 验证功能生效

准备[快速入门](quick_start.md#验证安装成功)中的`hello_jit.py`，在新的进程中执行：

```bash
CINDERX_PLUGIN_ENABLE=1 CINDERX_EVAL_MODE=cinder \
  CINDERX_JIT_MODE=execute PYTHONJITAUTO=20 \
  CINDERX_OSR_ENABLED=0 PYTHONJITLIGHTWEIGHTFRAME=0 \
  "$CINDERX_PY" hello_jit.py

```

预期输出：

```output
initialized: True
import error: None
jit enabled: True
result: 3
compiled: True
```

CP311的启动配置包括插件加载、帧求值器选择和JIT执行模式。日常使用按上例选择`execute`和数值调用阈值；`observe`、`shadow`仅用于诊断。该版本不支持`auto`／`auto:N`行为分类、OSR或轻量级帧执行。

## CPython 3.11 RPM

### 安装前检查

RPM用于openEuler 24.03 LTS-SP5 AArch64，要求发行版提供的非调试CPython 3.11.6。本包不适用于CPython 3.14，也不适用于其他3.11补丁版本。运行安装不要求`python3-devel`。

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

核对系统信息为目标openEuler 24.03 LTS-SP5，Python版本、解释器路径和架构均符合要求后继续。RPM会检查Python与动态库依赖；安装成功后还需执行下文的启动验证。

### 获取并安装RPM

以下dnf命令在具备系统软件包管理权限的shell中执行。

#### 本地安装

在RPM文件所在目录执行以下命令，将文件名替换为实际取得的文件名。

```shell
CINDERX_PLUGIN_ENABLE=0 dnf install ./python3-cinderx-2026.11.10.0-1.aarch64.rpm
```

完全离线安装要求系统已经提供全部依赖，可在安装命令中增加--disablerepo='*'；缺少依赖时先从配套软件源补齐，不使用--nodeps 跳过检查。

#### 软件源安装

正式软件源开放后，按发布公告配置软件源，再执行以下命令。下载的RPM由DNF自动校验。

```shell
CINDERX_PLUGIN_ENABLE=0 dnf install python3-cinderx
```

### 验证安装结果

RPM安装到系统`/usr/lib64/python3.11/site-packages`。如需核对已安装文件，可运行`rpm -V python3-cinderx`。先查询安装信息和实际导入位置：

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

准备[快速入门](quick_start.md#验证安装成功)中的`hello_jit.py`，使用系统解释器启动新进程：

```bash
CINDERX_PLUGIN_ENABLE=1 CINDERX_EVAL_MODE=cinder \
  CINDERX_JIT_MODE=execute PYTHONJITAUTO=20 \
  CINDERX_OSR_ENABLED=0 PYTHONJITLIGHTWEIGHTFRAME=0 \
  /usr/bin/python3.11 hello_jit.py
```

成功时应看到`initialized: True`、`import error: None`、`jit enabled: True`、`result: 3`和`compiled: True`。安装RPM不会自动为所有Python进程开启JIT，业务启动时仍需设置上述参数，且保留默认的site初始化。

### 在虚拟环境中使用系统RPM

普通venv默认隔离系统`site-packages`，看不到通过RPM安装的CinderX。需要在虚拟环境中复用系统RPM时，使用同一个发行版解释器新建允许访问系统包的环境：

```bash
CINDERX_PLUGIN_ENABLE=0 /usr/bin/python3.11 -m venv --system-site-packages .venv311-rpm
CINDERX_PLUGIN_ENABLE=1 CINDERX_EVAL_MODE=cinder \
  CINDERX_JIT_MODE=execute PYTHONJITAUTO=20 \
  CINDERX_OSR_ENABLED=0 PYTHONJITLIGHTWEIGHTFRAME=0 \
  .venv311-rpm/bin/python hello_jit.py
```

该环境同时能访问其他系统Python包。需要完全隔离依赖时，选择wheel安装方式。RPM管理的CinderX文件通过`dnf`维护，不在系统解释器中用`pip`覆盖或卸载同名包。

### 重装、升级、回退与卸载

维护前记录`rpm -q python3-cinderx`、原RPM、启动配置和业务验证结果，并停止使用该包的相关进程。同版本重装可执行：

```bash
rpm_file=python3-cinderx-2026.11.10.0-1.aarch64.rpm
CINDERX_PLUGIN_ENABLE=0 dnf reinstall "./$rpm_file" &&
  rpm -V python3-cinderx
```

升级时使用发布方提供的新RPM，先核对其系统、Python配套要求，再执行`CINDERX_PLUGIN_ENABLE=0 dnf upgrade /absolute/path/to/new-package.rpm`。需要回退时，对保留的旧RPM执行相同的配套检查，再执行`CINDERX_PLUGIN_ENABLE=0 dnf downgrade /absolute/path/to/old-package.rpm`。维护后启动新进程，重新运行最小示例和业务检查。

临时禁用JIT或CinderX分别使用`PYTHONJITDISABLE=1`、`CINDERX_DISABLE=1`，含义见[用户指南](user_guide.md#通用配置与api)。卸载使用：

```bash
CINDERX_PLUGIN_ENABLE=0 dnf remove python3-cinderx
```

随后执行`rpm -q python3-cinderx`，应提示未安装并返回非零。在仓库外启动新的`/usr/bin/python3.11`进程，确认没有其他路径提供同名模块；共享系统包的venv也会随系统RPM卸载而失去该包。

## CPython 3.14

### 准备Python环境

已有符合要求的Python时，推荐创建venv。

```bash
# 在源码仓库外准备工作目录。
mkdir -p "$HOME/cinderx-demo"
cd "$HOME/cinderx-demo"

CINDERX_PLUGIN_ENABLE=0 python3.14 -m venv .venv314
CINDERX_PY="$PWD/.venv314/bin/python"
```

也可用已安装并初始化的Conda准备推荐的3.14.3。

```bash
conda create -y --override-channels -c conda-forge python=3.14.3 pip -n cinderx314
conda activate cinderx314
CINDERX_PY="$(command -v python)"
```

下面的环境检查、安装和启动命令都使用所选解释器。RPM不用于CPython 3.14。

### 安装前检查

发布包包含CPython 3.14.0—3.14.3对应的本地扩展，称为fat wheel，推荐使用3.14.3。目标平台为Linux AArch64，推荐openEuler 24.03 LTS-SP3。系统C库及其他动态库仍需满足wheel的平台和依赖要求。

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

加载器按补丁版本（micro，例如3.14.3中的末位3）选择包内扩展，因此该wheel不支持3.14.4及后续版本，也不用于自由线程版本（free-threaded Python）。CPython 3.14.0的直接调用路径绕过AutoJIT分类准入；评估分类策略时建议使用3.14.3。

### 获取软件包

网页下载和命令行下载任选一种，得到的文件相同。

1. 下载软件包。

    - 网页下载

      从[CinderX Releases](https://gitcode.com/openeuler/cinderx/releases)选择正式版本，下载与解释器和架构匹配的wheel及同名`.sha256`文件。版本映射、已知限制和升级变化见[版本说明书](https://gitcode.com/openeuler/cinderx/releases)。
    
    - 命令行下载

      先按目标Python选择文件名。

      | Python | 文件名设置 |
      |---|---|
      | CP314 | `wheel=cinderx-2026.11.10.0-cp314-cp314-manylinux_2_24_aarch64.manylinux_2_28_aarch64.whl` |

2. 在工作目录中执行，校验软件包。

    ```bash
    release=v3.0.0
    base_url="https://gitcode.com/openeuler/cinderx/releases/download/$release"
    curl --fail --location --retry 3 --output "$wheel" "$base_url/$wheel" &&
      curl --fail --location --retry 3 --output "$wheel.sha256" "$base_url/$wheel.sha256" &&
      sha256sum -c "$wheel.sha256"
    ```

### 注意事项

- 使用其他版本时，同时替换Release、完整wheel文件名和校验文件。下面的安装步骤使用已下载并校验的本地文件。

- 本分支的发布包与上游PyPI包有各自的版本和支持范围。需要本分支能力时，使用对应Release的明确文件；不要只执行无版本限制的`pip install cinderx`后假定拿到相同制品。

- 为避免源码路径干扰已安装包的检查，以下命令在目标Linux主机的仓库外目录中执行，例如`$HOME/cinderx-demo`。

- wheel步骤可选择已有Python＋venv或Conda，`pip`始终通过所选环境的`CINDERX_PY`调用。RPM步骤使用系统`/usr/bin/python3.11`，安装、重装和卸载由具备系统软件包管理权限的用户执行。下载目录中保留准确的文件名，不用跨Python版本的wheel通配符安装。每一步成功后再继续；校验或安装失败时先处理错误。

- 包安装、重装和卸载命令显式设置`CINDERX_PLUGIN_ENABLE=0`，避免包管理进程因继承业务环境而自动加载当前本地扩展。业务验证使用对应Python版本的完整启动配置，并保留默认的site初始化；`-S`会跳过自动加载入口。

### 安装wheel

在下载目录执行。

```bash
wheel=cinderx-2026.11.10.0-cp314-cp314-manylinux_2_24_aarch64.manylinux_2_28_aarch64.whl
sha256sum -c "$wheel.sha256" &&
  CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip install --no-index --no-deps "$wheel" &&
  CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip show cinderx
```

### 验证安装结果

本节适用于wheel安装；RPM的查询和验证见[CPython 3.11 RPM](#cpython-311-rpm)。完成所选版本的wheel安装后，`CINDERX_PY`指向所选venv或Conda环境的Python。若重新打开终端，请先将它设为该解释器的绝对路径。以下检查适用于两个版本：

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

本节用于包身份和导入检查；初始化失败会返回非零，并停止后续依赖检查。JIT是否开启及目标是否编译，使用前面带完整启动参数的最小程序判断。

| 文件／目录 | 说明 |
|---|---|
| `cinderx/` | Python接口、编译器等包内容 |
| `_cinderx_auto.py`与`cinderx.pth` | Python site启动阶段的自动插件入口 |
| `_cinderx*.so` | CP311 wheel的顶层本地扩展 |
| `_cinderx.py` | CP314 fat wheel的顶层加载器，负责选择匹配的扩展 |
| `cinderx/_native/` | CP311构建记录；CP314 fat wheel的各补丁版本扩展及`fat_wheel.json` |
| `__static__/`、`__strict__/`、`opcodes/` | 兼容入口和opcode辅助包；包存在不等于所有平台均支持其功能 |
| `cinderx-<version>.dist-info/` | 包版本、安装记录和文件清单 |

检查包版本和功能时，以目标解释器实际导入的安装路径为准。手工复制其他构建的`.so`可能破坏包的一致性，应通过完整wheel安装。

### 验证功能生效

执行同一份`hello_jit.py`。这里使用数值阈值，检查所支持补丁版本上的普通JIT路径；推荐3.14.3上的分类模式示例见快速入门：

```bash
CINDERX_PLUGIN_ENABLE=1 PYTHONJITAUTO=20 \
  CINDERX_OSR_ENABLED=0 PYTHONJITLIGHTWEIGHTFRAME=0 \
  "$CINDERX_PY" hello_jit.py
```

CP314使用上述启动参数即可。先以普通帧、OSR关闭的配置完成基础验证，再按[用户指南](user_guide.md#cpython-314)选择其他能力。

## 升级与回退

本节为两个Python版本的wheel维护流程，沿用目标环境的`CINDERX_PY`。RPM通过前文的`dnf`命令维护。业务验证仍使用前面对应版本的启动参数。升级前先阅读目标版本说明，确认目标wheel支持当前Python补丁版本和系统。

### 保存旧环境信息

```bash
CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip freeze > requirements-before.txt
CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip show cinderx > cinderx-before.txt
```

保留原wheel、对应校验文件、服务启动配置及业务验证结果。`pip freeze`记录依赖版本，不替代旧wheel备份。优先新建环境安装候选包、验证后再切换服务，以便保留原环境回退。

### 在原环境升级

先停止使用该环境的相关服务或任务。在下面两行填写新包和已保存旧包的实际绝对路径；同一测试环境选择兼容的wheel：

```bash
NEW_WHEEL=/absolute/path/to/new/cinderx-package.whl
OLD_WHEEL=/absolute/path/to/old/cinderx-package.whl

(cd "$(dirname "$NEW_WHEEL")" && sha256sum -c "$(basename "$NEW_WHEEL").sha256") &&
  CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip install --no-index --no-deps --upgrade "$NEW_WHEEL" &&
  CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip show cinderx &&
  CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip check
```

如果以相同包版本替换了不同构建，pip可能认为已经安装；确认确需替换后使用`--force-reinstall`，并记录新旧哈希。安装完成后启动新进程，执行对应版本的`hello_jit.py`和业务验证，再恢复服务。

### 回退到旧包

```bash
(cd "$(dirname "$OLD_WHEEL")" && sha256sum -c "$(basename "$OLD_WHEEL").sha256") &&
  CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip install --no-index --no-deps --force-reinstall "$OLD_WHEEL" &&
  CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip show cinderx &&
  CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip check
```

恢复旧启动配置，启动新进程，并使用升级前适用于旧版本的检查验证回退结果；新增于新版本的功能不作为旧版的成功判据。如果Python补丁版本、系统或业务依赖也发生变化，恢复完整旧环境，不只替换CinderX wheel。更新磁盘上的扩展文件不会替换已运行进程中加载的代码，因此升级和回退后都必须重启相关进程。

## 禁用与卸载

需要暂时关闭JIT时，在原启动命令中增加`PYTHONJITDISABLE=1`，然后启动新进程。需要关闭CinderX的常规加载和初始化时，使用`CINDERX_DISABLE=1`，并检查应用是否显式调用依赖CinderX的功能。单独移除自动插件开关只影响自动加载，应用显式导入仍需单独核对。

以下命令卸载所选Python环境中的wheel；RPM使用前文的`dnf remove python3-cinderx`。停止相关进程后执行。

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

成功时输出`uninstalled: True`并返回`0`；仍能看到包元数据时返回非零。继续用新进程检查业务和搜索路径，避免源码目录、`PYTHONPATH`或另一个site-packages继续提供同名模块。

## 常见问题

| 现象 | 检查与处理 |
|---|---|
| CP311导入提示缺少`GLIBC_2.38` | 核对系统C库版本和wheel配套环境；创建Conda环境不会更换宿主系统的glibc |
| wheel不支持当前平台 | 核对`cp311`／`cp314`、AArch64、Python补丁版本及发行版要求，不修改文件名强行安装 |
| RPM已安装，但venv中找不到包 | 使用`/usr/bin/python3.11`，或用该解释器创建带`--system-site-packages`的venv，并检查实际导入路径 |
| RPM安装提示依赖不满足 | 核对openEuler 24.03 LTS-SP5、AArch64、CPython 3.11.6及配套动态库，从目标系统软件源补齐依赖 |
| `pip`显示已装，但运行时找不到包 | 比对`CINDERX_PY`、`python -m pip show`和实际导入路径，避免多个Python混用 |
| 本地扩展导入报`undefined symbol`／依赖缺失 | 核对包来源、解释器ABI和系统动态库；保留完整错误，不从其他环境拼接扩展 |
| CP314加载器拒绝当前补丁版本 | 使用包实际覆盖的3.14.0—3.14.3，或使用支持目标补丁版本的正式制品 |
| 导入成功但JIT未开启 | 检查禁用变量；CP311核对帧求值器和`execute`，两条线均运行最小程序 |
| CP311配置`auto:2`后报错 | 改用数值阈值；分类模式仅用于对应支持的CP314路径 |
| 轻量级帧与OSR同开失败 | 选择一种帧／OSR配置，见用户指南；CP311使用普通帧 |
| 升级后仍表现为旧版本 | 核对导入路径和包哈希，确认业务启动了新进程 |

定位问题时收集所选Release／SHA、wheel或RPM的版本与哈希、系统和Python配置、启动命令及完整错误，参见[运行维护与故障处理](user_guide.md#运行维护与故障处理)。

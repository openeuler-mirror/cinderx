# 编译构建指南

本文介绍在Linux AArch64上，分别为CPython 3.11.6和CPython 3.14.3构建CinderX。每条流程使用一个目标解释器，包含源码安装、wheel构建和基础验证。先完成通用准备，再选择对应版本的步骤。

CPython 3.11使用发行版锚定环境和现有wheel构建驱动；CPython 3.14使用仓库Dockerfile构建的3.14.3环境，通过pip生成单版本wheel。构建产物的适用范围由目标解释器、系统和工具链共同决定。

## 构建所需网络条件

宿主机和Docker构建容器都需要具备以下能力：正确解析所用域名，通过TCP 443建立HTTPS连接，通过TLS证书校验，并能持续传输完整文件。若使用代理或出口白名单，配置范围应同时覆盖宿主与容器，以及服务实际使用的重定向地址。所用DNS服务器还需允许正常的DNS查询；仅宿主能够联网，不代表构建容器也能联网。

| 服务 | 本文使用的域名或入口 | 需要完成的操作 |
|---|---|---|
| 源码仓库 | `gitcode.com` | 通过Git HTTPS完整克隆CinderX仓库并检出目标提交 |
| Docker基础镜像 | Docker Hub的`openeuler/openeuler:24.03-lts-sp3`；包括`registry-1.docker.io`、`auth.docker.io`及实际镜像层下载地址 | 完成镜像清单、认证和镜像层下载 |
| openEuler软件源 | `repo.openeuler.org`、`mirrors.openeuler.org`及软件源实际返回的下载地址，例如`dl-cdn.openeuler.openatom.cn` | 获取仓库元数据并完整下载RPM包 |
| Python源码和Python包镜像源 | `mirrors.huaweicloud.com`、`repo.huaweicloud.com`及软件源实际返回的下载地址 | 下载CPython源码、pip构建依赖和工具依赖 |
| GitHub源码依赖与工具归档 | `github.com`及归档实际跳转的`release-assets.githubusercontent.com` | 完成Git HTTPS克隆、固定提交检出和归档下载 |

网络检查应以实际Git克隆、归档下载和软件包安装完成为准。域名能够解析、首页能够打开或HEAD请求成功，都不足以证明完整构建所需的下载链路可用。连接超时、重置或长时间没有收到数据时，应先检查DNS、路由、代理和出口策略，再重试原步骤；不能仅据此认定构建命令错误。

如网络进行HTTPS证书检查，宿主与容器都应信任相应的合法CA，维持正常的证书校验。网络环境准备完成后，直接使用前文原始构建命令，不需要在构建命令中固定GitHub地址，也不需要获取额外的CinderX专用构建镜像。实操中的网络现象和临时排障配置只记录在验证报告中。

## 准备源码

在Linux AArch64主机操作，准备Git、Docker和Python 3.11及以上的宿主解释器。宿主Python用于运行构建驱动脚本；实际编译使用容器中的目标解释器、头文件和工具链。镜像构建及首次依赖获取需要可用的软件源和网络。每一步成功后再继续。进入交互容器后，需先退出容器，再执行后文标明在宿主上运行的命令。

```bash
git clone https://gitcode.com/openeuler/cinderx.git
cd cinderx
SOURCE_REF=origin/dev
git checkout --detach "$SOURCE_REF"
git rev-parse HEAD

# 输出目录放在源码目录之外，每次构建使用独立目录。
BUILD_ROOT="$HOME/cinderx-build/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$BUILD_ROOT"
```

开发时可选择克隆后已有的`origin/dev`；复现某次构建或制作正式制品时，将`SOURCE_REF`改为已确定的Tag或完整提交SHA，并记录实际检出的提交。两种Python的构建目录、镜像和wheel输出分别保存。

下文的Docker开发镜像预置编译器、CMake、测试和性能工具、CMake源码依赖缓存`/opt/cinderx-deps`，以及Python wheelhouse `/opt/cinderx-pydeps`。源码、Dockerfile或依赖改变后，重新构建相应镜像；挂载新源码不会自动更新镜像里已经安装的扩展。

## 准备Python环境

宿主Python用于运行`ci_pipeline/build_cp311_wheel.py`等构建驱动。已有Python 3.11及以上时可直接使用；也可用Conda创建独立的驱动环境。

```bash
conda create -y --override-channels -c conda-forge python=3.11.6 pip -n cinderx-build
conda activate cinderx-build
python3 --version
```

容器内用于编译CinderX的Python、头文件和工具链由对应Dockerfile准备。宿主Conda环境不改变容器中的目标解释器配置。

## CPython 3.11

### 环境要求

| 项目 | 要求 |
|---|---|
| 基础系统 | openEuler 24.03 LTS-SP3，AArch64 |
| Python与头文件 | `python3/python3-devel-3.11.6-34.oe2403sp3`，同NVR |
| Python库 | 发行版共享库libpython，保持发行版补丁和二进制接口（ABI）一致 |
| CinderX编译器 | GCC／G++ 14，包含静态libstdc++ 所需依赖 |
| Python构建依赖 | [requirements-cp311-build.txt](../../ci_pipeline/requirements-cp311-build.txt) |

不能用自行编译的通用CPython 3.11.6直接替代发行版锚定环境。构建前检查由[check_cpython_311_build.sh](../../ci_pipeline/scripts/check_cpython_311_build.sh)执行。

### 构建开发与测试镜像

1. 在仓库根目录执行。

    ```shell
    cd cinderx
    ```

2. 构建CP311开发镜像。   

    ```shell
    docker build -f Dockerfile.cp311-dev --build-arg BUILD_JOBS=8 \
      -t cinderx-dev:py311 .
    ```

3. 验证镜像。

    ```shell
    docker run --rm --workdir /tmp -e CINDERX_PLUGIN_ENABLE=0 cinderx-dev:py311 \
      python3.11 -c 'import cinderx, _cinderx; assert cinderx.is_initialized(), cinderx.get_import_error(); print(cinderx.__file__, _cinderx.__file__)'
    ```

    镜像构建过程会安装当前源码的CinderX。这里的`BUILD_JOBS`设置镜像内`CINDERX_TEST_JOBS`的初始值，不直接限制CinderX wheel编译；各阶段的并发设置见[构建并发的作用范围](#构建并发的作用范围)。

4. 调试交互容器。

    启动`cinderx-dev:py311`容器，并直接进入容器内部的bash命令行界面，方便手动调试或运行测试。

    ```bash
    docker run --rm -it cinderx-dev:py311 bash
    ```

    预期输出：

    ```output
    Welcome to 6.6.0-132.0.0.111.oe2403sp3.aarch64

    System information as of time:  Wed Nov 18 08:01:07 UTC 2026

    System load:    1.04
    Memory used:    11.9%
    Swap used:      0%
    Usage On:       47%
    Users online:   0
    ```

5. （可选）当您进入容器修改了`CinderX`源码，想要快速重新编译并测试效果（增量编译）时，在容器的`/workspace/cinderx`执行以下命令。

    ```shell
    CINDERX_PLUGIN_ENABLE=0 CMAKE_BUILD_PARALLEL_LEVEL=8 \
      python3.11 -m pip install --no-build-isolation --no-deps --force-reinstall .
    ```

    镜像已预装构建依赖，因此这里复用镜像依赖并强制重装当前源码。
    直接通过`setup.py`或`pip`构建时，默认构建类型为`RelWithDebInfo`，需要`Release`时显式设置`CMAKE_BUILD_TYPE=Release`。本例使用`--rm`，退出后容器内修改不会保留；需要将wheel保存到宿主时，使用下一节的发布构建驱动。

6. 退出容器环境。

    ```shell
    exit
    ```

### 构建发布wheel

1. 回到宿主的仓库根目录。

    ```shell
    cd cinderx
    ```

2. 执行以下命令，将当前源码并行编译打包成一个标准的Python安装包。

    ```bash
    CMAKE_BUILD_PARALLEL_LEVEL=8 \
      python3 ci_pipeline/build_cp311_wheel.py \
      --image cinderx-dev:py311 \
      --output-dir "$BUILD_ROOT/cp311/wheelhouse"
    ```

    驱动复制当前源码到容器，执行构建、wheel规范化和依赖检查，并在未预装CinderX的openEuler基础容器中安装验证。默认执行冒烟检查（smoke）；命令成功退出表示这条构建与冒烟检查流程通过。

    主要输出为：

    - `cinderx-<version>-cp311-cp311-linux_aarch64.whl`。
    - 同名`.sha256`和带哈希约束的requirements文件。
    - `logs/host-build-manifest-311.json`，记录源码和镜像等信息。
    - 包内构建记录`cinderx/_native/build_info_311.json`。

该产物是针对指定openEuler环境构建的普通Linux wheel。构建脚本静态链接C++ 运行库并检查依赖；它的兼容范围由实际构建和运行环境决定，修改文件名不会改变兼容性。

### 构建参数与验证

当前`setup.py`在CPython 3.11上默认开启PGO和LTO。PGO先构建插桩扩展，再运行仓库训练场景，最后使用profile重建；训练或profile检查失败时，构建失败。

需要快速开发构建时，在开发容器中显式关闭两项。快速编译当前源码，并强制覆盖安装到python3.11环境中。

```bash
CINDERX_PLUGIN_ENABLE=0 CINDERX_ENABLE_PGO=0 CINDERX_ENABLE_LTO=0 \
  CMAKE_BUILD_PARALLEL_LEVEL=8 python3.11 -m pip install \
  --no-build-isolation --no-deps --force-reinstall .
```

上述开关用于容器内的开发安装。发布构建驱动仅传递其定义的宿主参数，使用该入口时应按驱动说明配置，并记录与默认值不同的参数。

最小运行验证按[快速入门](quick_start.md#验证安装成功)执行。完整开发者验证见[CI指南](../../ci_pipeline/README_CN.md)：CP311 PR使用专用suite，不能把CP314的RuntimeTests命令直接换解释器后当作同等验证。

## CPython 3.14

### 环境要求

开发与测试镜像使用根目录[Dockerfile](../../Dockerfile)：GCC 12构建CPython 3.14.3，CinderX和RuntimeTests使用GCC 14。CPython自身是静态libpython、PGO＋LTO构建；这些是解释器构建属性，与CinderX wheel是否启用PGO／LTO分别配置。

### 构建开发与测试镜像

1. 在仓库根目录执行。

    ```shell
    cd cinderx
    ```

2. 构建CP314开发镜像。   

    ```shell
    docker build --build-arg BUILD_JOBS=8 -t cinderx-dev:py314 .
    ```

3. 验证镜像。

    ```shell
    docker run --rm --workdir /tmp -e CINDERX_PLUGIN_ENABLE=0 cinderx-dev:py314 \
      python3.14 -c 'import cinderx, _cinderx; assert cinderx.is_initialized(), cinderx.get_import_error(); print(cinderx.__file__, _cinderx.__file__)'
    ```

    镜像构建过程会安装当前源码的CinderX。这里的`BUILD_JOBS`设置镜像内`CINDERX_TEST_JOBS`的初始值，不直接限制CinderX wheel编译；各阶段的并发设置见[构建并发的作用范围](#构建并发的作用范围)。

4. 调试交互容器。

    启动`cinderx-dev:py311`容器，并直接进入容器内部的bash命令行界面，方便手动调试或运行测试。

    ```bash
    docker run --rm -it cinderx-dev:py314 bash
    ```

    预期输出：

    ```output
    Welcome to 6.6.0-132.0.0.111.oe2403sp3.aarch64

    System information as of time:  Wed Nov 18 08:01:07 UTC 2026

    System load:    1.04
    Memory used:    11.9%
    Swap used:      0%
    Usage On:       47%
    Users online:   0
    ```

5. （可选）当您进入容器修改了`CinderX`源码，想要快速重新编译并测试效果（增量编译）时，在容器的`/workspace/cinderx`执行以下命令。

    ```shell
    CINDERX_PLUGIN_ENABLE=0 CMAKE_BUILD_PARALLEL_LEVEL=8 \
      python3.14 -m pip install --no-build-isolation --no-deps --force-reinstall .
    ```

    此处使用镜像中已预装的依赖重新安装当前源码，默认构建类型为`RelWithDebInfo`；需要`Release`时显式设置`CMAKE_BUILD_TYPE=Release`。退出`--rm`容器后，容器内修改不会保留。这里安装的扩展仅用于当前解释器；后文的发布fat wheel则包含四个CPython补丁版本的扩展，需要单独构建和验证。

6. 退出容器环境。

    ```shell
    exit
    ```

### 构建发布wheel

1. 回到宿主的仓库根目录。

    ```shell
    cd cinderx
    ```

2. 以下命令使用刚构建的镜像，在CPython 3.14.3环境中生成wheel，并保存到宿主的独立输出目录。

    ```bash
    mkdir -p "$BUILD_ROOT/cp314/wheelhouse" "$BUILD_ROOT/cp314/logs"
    git rev-parse HEAD > "$BUILD_ROOT/cp314/logs/source-commit.txt"
    docker image inspect cinderx-dev:py314 > "$BUILD_ROOT/cp314/logs/image-inspect.json"

    # 固定从Git源码计算包版本时使用的日期。
    SOURCE_DATE_EPOCH=$(git show -s --format=%ct HEAD)

    # 在Bash中执行，使容器构建失败能够透过tee返回。
    set -o pipefail
    docker run --rm \
      -v "$BUILD_ROOT/cp314:/out" \
      -e SOURCE_DATE_EPOCH="$SOURCE_DATE_EPOCH" \
      -e CINDERX_VERSION_PATCH=0 \
      -e CINDERX_PLUGIN_ENABLE=0 \
      -e CMAKE_BUILD_TYPE=Release \
      -e CMAKE_BUILD_PARALLEL_LEVEL=8 \
      -e CINDERX_ENABLE_PGO=0 \
      -e CINDERX_ENABLE_LTO=0 \
      cinderx-dev:py314 bash -lc '
        set -euo pipefail
        cd /workspace/cinderx
        python3.14 -VV
        python3.14 -m sysconfig > /out/logs/python-sysconfig.txt
        gcc --version
        cmake --version
        python3.14 -m pip wheel --no-build-isolation --no-deps \
          --wheel-dir /out/wheelhouse .
      ' 2>&1 | tee "$BUILD_ROOT/cp314/logs/build.log"
    ```

    确认构建命令成功后，再执行下一节。每次使用新的`BUILD_ROOT`，避免把历史wheel混入本次输出。

    主要输出为：

    - `wheelhouse/cinderx-<version>-cp314-cp314-linux_aarch64.whl`，实际文件名以构建输出为准。
    - `logs/build.log`，保存构建输出和工具版本。
    - `logs/source-commit.txt`、`logs/image-inspect.json`和`logs/python-sysconfig.txt`，保存源码、镜像和解释器配置记录。

该wheel用于与本次CPython 3.14.3及系统ABI相匹配的环境。文件名中的`cp314`没有表达补丁版本边界，不能据此认定它适用于任意3.14.x。该流程也未执行manylinux兼容性修复与验收。

### 验证导出的wheel

在宿主执行以下命令。在新容器内创建独立venv，从宿主输出目录安装刚生成的wheel，并在源码目录之外验证导入，确认使用的是新安装的包。

```bash
docker run --rm --workdir /tmp \
  -v "$BUILD_ROOT/cp314:/out:ro" \
  -e CINDERX_PLUGIN_ENABLE=0 \
  cinderx-dev:py314 bash -lc '
    set -euo pipefail
    python3.14 -m venv /tmp/cinderx-wheel-check
    set -- /out/wheelhouse/cinderx-*.whl
    test "$#" -eq 1
    test -f "$1"
    /tmp/cinderx-wheel-check/bin/python -m pip install --no-index --no-deps "$1"
    cd /tmp
    /tmp/cinderx-wheel-check/bin/python -c "import sys, cinderx, _cinderx; assert sys.version_info[:3] == (3, 14, 3), sys.version; assert cinderx.is_initialized(), cinderx.get_import_error(); assert cinderx.__file__.startswith(sys.prefix + \"/\"), cinderx.__file__; assert _cinderx.__file__.startswith(sys.prefix + \"/\"), _cinderx.__file__; print(sys.version); print(cinderx.__file__, _cinderx.__file__)"
  '
```

导入检查通过后，保存wheel校验值：

```bash
(cd "$BUILD_ROOT/cp314/wheelhouse" && sha256sum cinderx-*.whl > SHA256SUMS)
```

上述检查覆盖wheel安装、导入和CinderX初始化。最小JIT运行验证见[快速入门](quick_start.md#验证安装成功)，导入成功不代表完整功能或性能验收通过。

### 构建参数与验证

CP314 fat wheel默认`Release`，PGO和LTO默认关闭。需要其他配置时明确指定：

```bash
python3 ci_pipeline/build_cp314_manylinux_fat_wheel.py \
  --image "$BUILDER_IMAGE" \
  --output-dir "$BUILD_ROOT/cp314-pgo-lto/build" \
  --wheelhouse-dir "$BUILD_ROOT/cp314-pgo-lto/wheelhouse" \
  --cmake-build-type RelWithDebInfo --pgo --lto \
  --cmake-build-parallel-level 8 --makeflags=-j8
```

这组参数会改变构建类型和优化配置，使用时应在构建记录中注明。默认冒烟检查验证导入等基础能力；完整功能测试和性能验收需另行执行。最小运行验证见[快速入门](quick_start.md)。

## 依赖缓存与构建记录

### 构建并发的作用范围

| 阶段 | 实际控制项 |
|---|---|
| Dockerfile内构建CPython 3.14 | `--build-arg BUILD_JOBS=N`传给解释器的`make -jN` |
| 开发镜像内后续RuntimeTests | `BUILD_JOBS`预设`CINDERX_TEST_JOBS`，运行时可覆盖 |
| CinderX `setup.py`／`pip`构建 | 正整数`CMAKE_BUILD_PARALLEL_LEVEL`；未设置或值无效时使用`os.cpu_count()` |
| CP311发布构建驱动 | 透传宿主的`CMAKE_BUILD_PARALLEL_LEVEL` |
| CP314发布构建驱动 | `--cmake-build-parallel-level`和`--makeflags`分别传入构建环境 |

当前Dockerfile没有将`BUILD_JOBS`映射为安装CinderX时的`CMAKE_BUILD_PARALLEL_LEVEL`。因此`--build-arg BUILD_JOBS=8`不保证镜像构建中所有编译阶段都限制为8；需要按实际阶段检查并发和内存。仅给`docker build`前的宿主shell设置环境变量，也不会自动把它传进Dockerfile的`RUN`。

### 依赖与溯源

两条线使用相同的源码依赖缓存机制：开发`pip install .`可设置`CINDERX_LOCAL_DEPS=/path/to/cache`；直接运行`setup.py`可使用`--local`。缺失或版本、来源不匹配的依赖可能重新获取；完全离线构建还需事先补齐并核对所有依赖。

Python依赖离线安装还需要完整wheelhouse；门禁使用`CINDERX_PIP_WHEELHOUSE`和`CINDERX_PIP_OFFLINE=1`，细节见[CI依赖缓存说明](../../ci_pipeline/README_CN.md#依赖缓存与-pip-离线模式)。构建隔离环境也必须能获取所需的setuptools等依赖。

保存源码SHA、镜像ID、Python配置、工具链版本、构建参数、wheel哈希和日志。PGO训练采集的性能剖面（profile）可能随运行变化。`SOURCE_DATE_EPOCH`用于固定版本日期和部分归档元数据；判断wheel是否逐字节一致，还需比较实际文件。

从Git源码检出构建、且没有`PKG-INFO`版本覆盖时，两个独立发布驱动的版本日期来源不同：CP311驱动默认使用源码提交时间对应的UTC日期；CP314 fat wheel驱动未收到`SOURCE_DATE_EPOCH`时，使用构建当天的UTC日期。需要统一同批CP311／CP314制品的包版本时，可使用统一发布入口`build_release_wheels.py`，或为两个驱动设置相同的`SOURCE_DATE_EPOCH`和补丁号；若指定CP314的`--version-patch`，也应保持一致。

## 常见问题

| 现象 | 检查与处理 |
|---|---|
| CP311开发镜像隔离启动时找不到CinderX | 发行版的`-I`或`PYTHONNOUSERSITE=1`会排除`/usr/local`安装目录；使用正常site启动，或在独立venv中安装wheel |
| 构建时找不到Python headers或libpython | 确认目标解释器、headers、发行版包及ABI一致；CP311重新执行锚定预检 |
| 依赖拉取或pip失败 | 区分CMake源码缓存与Python wheelhouse，检查固定版本、网络和镜像源 |
| `Killed`、`cc1plus`退出或OOM | 先区分解释器、wheel、RuntimeTests所处阶段，再调整该阶段的并发；参考上表并检查容器内存限额 |
| CP311 PGO提示已加载`_cinderx` | 在未启用自动插件的构建进程中执行，检查`CINDERX_PLUGIN_ENABLE`和构建隔离方式 |
| 覆盖率与PGO／LTO冲突 | 覆盖率构建关闭PGO／LTO，按CI指南使用专门的验证配置 |
| CP314发布预检缺少解释器或auditwheel | 检查专用构建镜像是否满足前述环境要求，根目录开发镜像不能替代它 |
| wheel已生成但smoke／门禁失败 | 保留日志和manifest，分别定位打包、运行时或测试框架错误；整条命令失败时不记作验证通过 |

更多运行时错误见[安装部署指南](installation_guide.md#常见问题)和[用户指南](user_guide.md#运行维护与故障处理)。

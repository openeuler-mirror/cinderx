# 快速入门

本指南以CP311 wheel为例，介绍完成Python环境准备、软件包获取与安装、示例创建、JIT启动和结果检查的完整流程。示例使用`v3.0.0`对应的包版本`2026.11.10.0`。

## 配套版本

以下以`v3.0.0`为例：

| 项目 | 内容 |
|---|---|
| 产品版本 | `v3.0.0` |
| wheel元数据版本 | `2026.11.10.0` |
| CP311 RPM版本 | `2026.11.10.0-1`，包名`python3-cinderx` |
| 架构 | Linux AArch64 / ARM64 |
| wheel来源 | `openeuler/cinderx` Release附件 |
| RPM来源 | 暂未公开发布。已取得RPM时，参见[本地安装步骤](installation_guide.md)本地安装。 |
| 校验方式 | wheel使用同一Release的SHA256文件。通过软件源安装的RPM由DNF自动校验。 |

## 安装软件包

完成[环境检查](installation_guide.md#准备环境)后，执行以下步骤安装软件包。

1. 在源码仓库外准备工作目录。

    ```bash
    mkdir -p "$HOME/cinderx-demo"
    cd "$HOME/cinderx-demo"
    ```

2. 准备Python环境。

    ```bash
    CINDERX_PLUGIN_ENABLE=0 /usr/bin/python3.11 -m venv .venv311
    CINDERX_PY="$PWD/.venv311/bin/python"
    "$CINDERX_PY" -V
    ```

    `CINDERX_PY`始终指向所选环境的解释器，后文安装与运行均使用它。

3. 获取并安装软件包。
网页下载和命令行下载任选一种，得到的文件相同。

    - 网页下载

      从[CinderX Releases](https://gitcode.com/openeuler/cinderx/releases)选择正式版本，下载与解释器和架构匹配的wheel及同名`.sha256`文件。版本映射、已知限制和升级变化见[版本说明书](https://gitcode.com/openeuler/cinderx/releases)。
    
    - 命令行下载

    1. 使用CPython 3.11 wheel，设置完整文件名。

        ```shell
        wheel=cinderx-2026.11.10.0-cp311-cp311-linux_aarch64.whl
        ```

    2. 执行以下命令行安装wheel包。

        ```bash
        release=v3.0.0
        base_url="https://gitcode.com/openeuler/cinderx/releases/download/$release"
        curl --fail --location --retry 3 --output "$wheel" "$base_url/$wheel" &&
          curl --fail --location --retry 3 --output "$wheel.sha256" "$base_url/$wheel.sha256"
        ```

    3. 校验并安装到已选环境。

        ```bash
        sha256sum -c "$wheel.sha256" &&
          CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip install --no-index --no-deps "$wheel" &&
          CINDERX_PLUGIN_ENABLE=0 "$CINDERX_PY" -m pip check
        ```

## 验证安装成功

执行以下操作测试和验证Cinderx是否安装成功并能正常工作。

1. 在当前工作目录中创建UTF-8文件`hello_jit.py`。

    ```bash
    touch hello_jit.py
    ```

2. 编辑`hello_jit.py`。

    ```bash
    vi hello_jit.py
    ```

3. 复制以下示例代码粘贴到`hello_jit.py`，按":wq"退出编辑模式。

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

4. 启动程序。

    ```bash
    CINDERX_PLUGIN_ENABLE=1 CINDERX_EVAL_MODE=cinder \
      CINDERX_JIT_MODE=execute PYTHONJITAUTO=20 \
      CINDERX_OSR_ENABLED=0 PYTHONJITLIGHTWEIGHTFRAME=0 \
      "$CINDERX_PY" hello_jit.py
    ```

    CP311使用数值调用阈值，不支持`auto`／`auto:N`行为分类。`observe`和`shadow`用于诊断，日常运行选择`execute`。

5. 检查结果。

    预期输出示例如下所示，且进程退出码为`0`：

    ```output
    initialized: True
    import error: None
    jit enabled: True
    result: 3
    compiled: True
    ```

    上述结果表示初始化和JIT启用成功、计算结果正确，且目标函数处于已编译状态。本例显式调用`force_compile`，不用于判断自动调用阈值是否触发；机器码执行次数和性能收益需要另行验证。

# CPython 3.11 锚定记录（openEuler 24.03-LTS-SP3）

## 锚定事实

| 项 | 值 |
|---|---|
| 基底镜像 | `openeuler/openeuler:24.03-lts-sp3` @ `sha256:03b3f365bbba13e9f90c70b08bd9aa0002bc69db858c2808cfab8a55d6e184e3` |
| 目标 Python | `python3-3.11.6-34.oe2403sp3`（含同 NVR 的 python3-devel） |
| SOABI | `cpython-311-aarch64-linux-gnu` |
| 编译器 | GCC 14.2.0 源码编译，`LDFLAGS=-Wl,-rpath,/usr/local/gcc-14.2.0/lib64` 解决 libstdc++（与 3.14.3 交付 Dockerfile 同方案） |
| 发行版构建形态 | `--enable-shared --with-computed-gotos=yes --with-dtrace --with-lto --enable-optimizations`；CFLAGS `-O2 -fstack-protector-strong -D_FORTIFY_SOURCE=2` |
| Lib/test | 发行版包自带（regrtest 可列 457 项），双模比对无需额外语料 |
| 烘焙工具（镜像 v2 起） | `ci_pipeline/requirements-cp311-build.txt`：`setuptools==80.9.0`、`wheel==0.45.1`、`build==1.3.0`；`ci_pipeline/requirements-cp311-test.txt`：`pytest==9.0.3`（统一 PR、Daily 与开发/性能镜像口径）；git `http.sslVerify=false`（内网 TLS 拦截代理场景；依赖完整性由钉死 commit 与 vendored SHA256SUMS 清单承载，不依赖传输通道） |

制品 sha256：

```
python3-3.11.6-34.oe2403sp3.aarch64.rpm  7dc776537c3eb42a088fa58ce9c7399b7a598701a90f6bd2a96fe5ea26c27531
python3-3.11.6-34.oe2403sp3.src.rpm      bf124b75613faf3b15094666fe635e5e470d3e1503c36b2dbec7b9909312b1e0
```

## 符号可链性结论

libpython3.11 因 `--with-lto` 仅导出 PyAPI 标记符号（共 1657 个），`_pydict_global_version`、`_PyFrame_Clear`、`_PyThreadState_PopFrame` 等内部符号均不可外链（公开 API 正常）。由此定案：

- 所需内部私有实现基本无法直调，**需通过 UpstreamBorrow 机制提供**；
- **dict 版本发号器采用影子方案**（真源不可链；高位播种与 ABA 论证为 IC 阶段义务）。

## 附录：采集命令（容器内）

```bash
# 基底镜像 digest（宿主侧）
docker inspect --format='{{index .RepoDigests 0}}' openeuler/openeuler:24.03-lts-sp3

# 包版本与制品校验
rpm -q python3 python3-devel
dnf download python3-3.11.6-34.oe2403sp3 python3-devel-3.11.6-34.oe2403sp3
curl -fLO https://repo.openeuler.org/openEuler-24.03-LTS-SP3/update/source/Packages/python3-3.11.6-34.oe2403sp3.src.rpm   # srpm 不在 dnf 源仓索引，走直链
sha256sum ./*.rpm

# 符号可链性
nm -D --defined-only /usr/lib64/libpython3.11.so.1.0 | wc -l
nm -D --defined-only /usr/lib64/libpython3.11.so.1.0 | grep -wE '_pydict_global_version|_PyFrame_Clear|_PyThreadState_PopFrame'

# 发行版构建形态与 Lib/test
python3.11 -c "import sysconfig; print(sysconfig.get_config_var('CONFIG_ARGS'))"
python3.11 -m test.regrtest --list-tests | wc -l

# post-patch 源码树（vendor 与 borrow --check 的取源）
mkdir -p /tmp/rpmbuild/{BUILD,SOURCES,SPECS,SRPMS}
rpm -ivh --define "_topdir /tmp/rpmbuild" python3-3.11.6-34.oe2403sp3.src.rpm
rpmbuild -bp --nodeps --define "_topdir /tmp/rpmbuild" /tmp/rpmbuild/SPECS/python3.spec
```

# CinderX development and validation image for CPython 3.14.3.
#
# Toolchain contract:
#   * CPython is built with the openEuler system GCC 12.
#   * CinderX, RuntimeTests, and coverage are built with GCC 14.
#   * CPython is static/non-shared and does not contain GCC 12 LTO objects,
#     so GCC 14 can safely link RuntimeTests against libpython3.14.a.
#
# Build:
#   docker build -t cinderx-dev:py314 .
#
# Validate the baked source tree:
#   docker run --rm cinderx-dev:py314 python3.14 ci_pipeline/run_gate.py pr
#   docker run --rm cinderx-dev:py314 python3.14 ci_pipeline/run_gate.py pr --coverage
#
# Run a pyperformance smoke benchmark:
#   docker run --rm cinderx-dev:py314 bash -lc \
#     'CINDERX_PLUGIN_ENABLE=1 PYTHONJITAUTO=auto:2 python3.14 -m pyperformance run --fast -b nbody --inherit-environ CINDERX_PLUGIN_ENABLE,PYTHONJITAUTO -o /tmp/nbody.json'

ARG BASE_IMAGE=openeuler/openeuler:24.03-lts-sp3
ARG PYTHON_VERSION=3.14.3
ARG GCC_TOOLSET_MAJOR=14
ARG LCOV_VERSION=1.16
ARG PYPERFORMANCE_VERSION=1.13.0
ARG PYPERF_VERSION=2.9.0
ARG SETUPTOOLS_VERSION=80.9.0
ARG PYTEST_VERSION=8.4.1
ARG WHEEL_VERSION=0.45.1
ARG BUILD_VERSION=1.3.0
ARG BUILD_JOBS=16
ARG PIP_INDEX_URL=https://mirrors.huaweicloud.com/repository/pypi/simple
ARG PYTHON_DOWNLOAD_BASE=https://mirrors.huaweicloud.com/python

FROM ${BASE_IMAGE}

ARG GCC_TOOLSET_MAJOR
ARG LCOV_VERSION
ARG PYPERFORMANCE_VERSION
ARG PYPERF_VERSION
ARG SETUPTOOLS_VERSION
ARG PYTEST_VERSION
ARG WHEEL_VERSION
ARG BUILD_VERSION
ARG BUILD_JOBS
ARG PIP_INDEX_URL
ARG PYTHON_DOWNLOAD_BASE

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

ENV LANG=C.UTF-8 \
    LC_ALL=C.UTF-8 \
    PIP_INDEX_URL=${PIP_INDEX_URL} \
    PIP_DISABLE_PIP_VERSION_CHECK=1 \
    PYTHONDONTWRITEBYTECODE=1

# Build dependencies for CPython and CinderX, PR-gate coverage tools, and
# native crash/performance diagnostics. lcov is installed separately because
# it is not present in the default openEuler 24.03-LTS-SP3 repositories.
RUN dnf install -y \
        gcc \
        gcc-c++ \
        gcc-toolset-${GCC_TOOLSET_MAJOR}-gcc \
        gcc-toolset-${GCC_TOOLSET_MAJOR}-gcc-c++ \
        gcc-toolset-${GCC_TOOLSET_MAJOR}-libgcc \
        gcc-toolset-${GCC_TOOLSET_MAJOR}-libstdc++-devel \
        make \
        cmake \
        ninja-build \
        git \
        wget \
        curl \
        tar \
        gzip \
        xz \
        unzip \
        zip \
        patch \
        diffutils \
        findutils \
        which \
        file \
        procps-ng \
        util-linux \
        perl \
        openssl-devel \
        zlib-devel \
        bzip2-devel \
        readline-devel \
        sqlite-devel \
        ncurses-devel \
        xz-devel \
        tk-devel \
        libxml2-devel \
        libffi-devel \
        gdbm-devel \
        expat-devel \
        binutils \
        gdb \
        perf \
        strace \
    && dnf clean all \
    && rm -rf /var/cache/dnf

RUN if ! command -v lcov >/dev/null 2>&1 || ! command -v genhtml >/dev/null 2>&1; then \
        curl --fail --location --retry 5 --retry-delay 2 \
            --output /tmp/lcov.tar.gz \
            "https://github.com/linux-test-project/lcov/releases/download/v${LCOV_VERSION}/lcov-${LCOV_VERSION}.tar.gz"; \
        tar -C /tmp -xzf /tmp/lcov.tar.gz; \
        make -C "/tmp/lcov-${LCOV_VERSION}" install PREFIX=/usr/local; \
        rm -rf /tmp/lcov.tar.gz "/tmp/lcov-${LCOV_VERSION}"; \
    fi \
    && lcov --version \
    && genhtml --version

# Build CPython before GCC 14 is added to PATH. Explicit compiler paths and
# major-version assertions keep the two toolchain roles machine-checkable.
ARG PYTHON_VERSION
WORKDIR /tmp
RUN test "$(/usr/bin/gcc -dumpfullversion | cut -d. -f1)" = "12" \
    && test "$(/usr/bin/g++ -dumpfullversion | cut -d. -f1)" = "12" \
    && curl --fail --location --retry 5 --retry-delay 2 \
        --output "Python-${PYTHON_VERSION}.tgz" \
        "${PYTHON_DOWNLOAD_BASE}/${PYTHON_VERSION}/Python-${PYTHON_VERSION}.tgz" \
    && tar -xzf "Python-${PYTHON_VERSION}.tgz" \
    && cd "Python-${PYTHON_VERSION}" \
    && CC=/usr/bin/gcc CXX=/usr/bin/g++ ./configure \
        --prefix="/usr/local/cpython-${PYTHON_VERSION}" \
        --enable-optimizations \
        --with-ensurepip=install \
    && make -j"${BUILD_JOBS}" \
    && make altinstall \
    && cd /tmp \
    && rm -rf "Python-${PYTHON_VERSION}" "Python-${PYTHON_VERSION}.tgz"

ENV CINDERX_TEST_PYTHON=/usr/local/cpython-${PYTHON_VERSION}/bin/python3.14 \
    PATH=/usr/local/cpython-${PYTHON_VERSION}/bin:/opt/openEuler/gcc-toolset-${GCC_TOOLSET_MAJOR}/root/usr/bin:/usr/local/bin:/usr/local/sbin:/usr/sbin:/usr/bin:/sbin:/bin \
    CC=/opt/openEuler/gcc-toolset-${GCC_TOOLSET_MAJOR}/root/usr/bin/gcc \
    CXX=/opt/openEuler/gcc-toolset-${GCC_TOOLSET_MAJOR}/root/usr/bin/g++ \
    GCOV=/opt/openEuler/gcc-toolset-${GCC_TOOLSET_MAJOR}/root/usr/bin/gcov \
    LDFLAGS=-Wl,-rpath,/opt/openEuler/gcc-toolset-${GCC_TOOLSET_MAJOR}/root/usr/lib64 \
    CINDERX_LOCAL_DEPS=/opt/cinderx-deps \
    CINDERX_PIP_WHEELHOUSE=/opt/cinderx-pydeps \
    CINDERX_PIP_OFFLINE=1 \
    CINDERX_TEST_JOBS=${BUILD_JOBS}

RUN ln -sf "${CINDERX_TEST_PYTHON}" /usr/local/bin/python3 \
    && ln -sf "${CINDERX_TEST_PYTHON}" /usr/local/bin/python \
    && ln -sf /usr/local/cpython-${PYTHON_VERSION}/bin/pip3.14 /usr/local/bin/pip3 \
    && ln -sf /usr/local/cpython-${PYTHON_VERSION}/bin/pip3.14 /usr/local/bin/pip \
    && ln -sf "${CC}" /usr/local/bin/gcc-${GCC_TOOLSET_MAJOR} \
    && ln -sf "${CXX}" /usr/local/bin/g++-${GCC_TOOLSET_MAJOR} \
    && test "$("${CC}" -dumpfullversion | cut -d. -f1)" = "${GCC_TOOLSET_MAJOR}" \
    && test "$("${CXX}" -dumpfullversion | cut -d. -f1)" = "${GCC_TOOLSET_MAJOR}" \
    && "${CINDERX_TEST_PYTHON}" -c 'import pathlib, subprocess, sys, sysconfig; assert sys.version_info[:3] == (3, 14, 3), sys.version; assert sysconfig.get_config_var("Py_ENABLE_SHARED") != 1; assert subprocess.check_output(["/usr/bin/gcc", "-dumpfullversion"], text=True).split(".")[0] == "12"; assert not any(pathlib.Path("/usr/local/cpython-3.14.3/lib").glob("libpython3.14*.so*")); print(sys.version); print("CPython CC:", sysconfig.get_config_var("CC"))'

# Bake the exact FetchContent revisions used by this repository so run_gate
# does not depend on GitHub availability after the image is built.
RUN set -eux; \
    clone_dep() { \
        name="$1"; url="$2"; revision="$3"; \
        if [ -d "/opt/cinderx-deps/$name/.git" ] \
            && [ "$(git -C "/opt/cinderx-deps/$name" remote get-url origin)" = "$url" ] \
            && [ "$(git -C "/opt/cinderx-deps/$name" rev-parse HEAD)" = "$revision" ] \
            && [ -z "$(git -C "/opt/cinderx-deps/$name" status --porcelain)" ]; then \
            return; \
        fi; \
        rm -rf "/opt/cinderx-deps/$name"; \
        git clone --no-checkout "$url" "/opt/cinderx-deps/$name"; \
        git -C "/opt/cinderx-deps/$name" checkout --detach "$revision"; \
        test "$(git -C "/opt/cinderx-deps/$name" rev-parse HEAD)" = "$revision"; \
        test -z "$(git -C "/opt/cinderx-deps/$name" status --porcelain)"; \
    }; \
    mkdir -p /opt/cinderx-deps; \
    clone_dep fmt https://github.com/fmtlib/fmt 40626af88bd7df9a5fb80be7b25ac85b122d6c21; \
    clone_dep parallel-hashmap https://github.com/greg7mdp/parallel-hashmap 896f1a03e429c45d9fe9638e892fc1da73befadd; \
    clone_dep usdt https://github.com/libbpf/usdt f4ea2f524efa80d062f4d586d78daafb83dc7d24; \
    clone_dep capstone https://github.com/capstone-engine/capstone 52c66920fc7bfa15fd9626dfd9f646c698aaa99b; \
    clone_dep googletest https://github.com/google/googletest 52eb8108c5bdec04579160ae17225d66034bd723

# Keep both an installed tool set and a complete wheelhouse for run_gate's
# isolated venvs. The wheelhouse includes pytest's transitive dependencies.
RUN "${CINDERX_TEST_PYTHON}" -m pip install --no-cache-dir --upgrade pip \
    && "${CINDERX_TEST_PYTHON}" -m pip install --no-cache-dir \
        "setuptools==${SETUPTOOLS_VERSION}" \
        "wheel==${WHEEL_VERSION}" \
        "build==${BUILD_VERSION}" \
        "pytest==${PYTEST_VERSION}" \
        "pyperformance==${PYPERFORMANCE_VERSION}" \
        "pyperf==${PYPERF_VERSION}" \
    && mkdir -p /opt/cinderx-pydeps \
    && "${CINDERX_TEST_PYTHON}" -m pip download --dest /opt/cinderx-pydeps \
        pip \
        "setuptools==${SETUPTOOLS_VERSION}" \
        "wheel==${WHEEL_VERSION}" \
        "build==${BUILD_VERSION}" \
        "pytest==${PYTEST_VERSION}" \
        "pyperformance==${PYPERFORMANCE_VERSION}" \
        "pyperf==${PYPERF_VERSION}"

WORKDIR /workspace/cinderx
COPY . /workspace/cinderx

# Install the checkout as a wheel so cinderx.pth is present and
# CINDERX_PLUGIN_ENABLE works in pyperformance workers. run_gate still
# builds its own clean wheel and venv from the same source tree.
RUN env -u CINDERX_ENABLE_LTO \
        "${CINDERX_TEST_PYTHON}" -m pip install \
        . --no-build-isolation --no-deps --force-reinstall \
    && "${CINDERX_TEST_PYTHON}" -c \
        'import cinderx, _cinderx; print(cinderx.__file__); print(_cinderx.__file__)' \
    && "${CINDERX_TEST_PYTHON}" -m pyperformance venv create \
        --inherit-environ http_proxy,https_proxy,PIP_INDEX_URL,CINDERX_PLUGIN_ENABLE,PYTHONJITAUTO,PYTHONJITLIGHTWEIGHTFRAME \
    && find venv -name pyvenv.cfg -exec \
        sed -i 's/^include-system-site-packages = false/include-system-site-packages = true/' {} + \
    && worker_python="$(find venv -path '*/bin/python' -print -quit)" \
    && test -n "${worker_python}" \
    && "${worker_python}" -m pip install --no-index \
        --find-links=/opt/cinderx-pydeps "pyperf==${PYPERF_VERSION}" \
    && "${worker_python}" -c \
        'import importlib.metadata as m; assert m.version("pyperf") == "2.9.0"'

CMD ["/bin/bash"]

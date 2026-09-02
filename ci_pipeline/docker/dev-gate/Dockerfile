ARG BASE_IMAGE=ghcr.io/zhaolangchen2-beep/lang_env/oe2403sp3-py3116_py3140_py3141_py3142_py3143-gcc14-aarch64@sha256:0d8453d93b7f480887748d8dc24ee8a20319ba1805c3d3186965e8d86c7c3685

FROM ${BASE_IMAGE}

LABEL org.opencontainers.image.title="CinderX CPython 3.11/3.14 developer gate"
LABEL org.opencontainers.image.description="Pinned openEuler 24.03-LTS-SP3 environment for local CinderX PR gates"

COPY python-launcher.sh /usr/local/libexec/cinderx-dev-gate-python

RUN chmod 0755 /usr/local/libexec/cinderx-dev-gate-python \
    && ln -sfn /usr/local/libexec/cinderx-dev-gate-python /usr/local/bin/python3.11 \
    && ln -sfn /usr/local/libexec/cinderx-dev-gate-python /usr/local/bin/python3.14 \
    && test -x /usr/local/cpython-3.11.6/bin/python3.11 \
    && test -x /usr/local/cpython-3.14.3/bin/python3.14 \
    && test -d /opt/cinderx-deps/fmt \
    && test -d /opt/cinderx-pydeps

ENV CINDERX_LOCAL_DEPS=/opt/cinderx-deps \
    CINDERX_PIP_WHEELHOUSE=/opt/cinderx-pydeps \
    CINDERX_PIP_OFFLINE=1 \
    CINDERX_TEST_JOBS=4 \
    RT311_CENSUS_SHARD_SIZE=100

WORKDIR /workspace

CMD ["/bin/bash", "-l"]

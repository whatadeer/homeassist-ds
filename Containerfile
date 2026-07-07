# Builds ha3ds.cia without installing devkitPro on the host.
#
#   podman build -t ha3ds-builder -f Containerfile .
#   podman run --rm -v ${PWD}:/project ha3ds-builder make cia
#
# devkitpro/devkitarm already ships devkitARM, libctru, citro2d/citro3d, and
# the curl/mbedtls/jansson/zlib 3DS ports this project links against.
#
# The two CIA-packaging tools it does NOT ship - makerom and bannertool -
# aren't in devkitPro's pacman repos at all, and their prebuilt GitHub
# release binaries are linked against a newer glibc/libstdc++ than this
# (Debian 12 bookworm-based) image has, so they're built from source here
# instead, against the same base image, to guarantee they'll actually run.
FROM devkitpro/devkitarm AS tools-builder

RUN git clone --depth 1 --recursive --branch makerom-v0.19.0 \
        https://github.com/profi200/Project_CTR.git /tmp/Project_CTR && \
    make -C /tmp/Project_CTR/makerom deps && \
    make -C /tmp/Project_CTR/makerom

RUN git clone --depth 1 --branch 1.2.3 \
        https://github.com/carstene1ns/3ds-bannertool.git /tmp/3ds-bannertool && \
    cmake -B /tmp/3ds-bannertool/build -S /tmp/3ds-bannertool -DCMAKE_BUILD_TYPE=Release && \
    cmake --build /tmp/3ds-bannertool/build

FROM devkitpro/devkitarm

COPY --from=tools-builder /tmp/Project_CTR/makerom/bin/makerom /opt/devkitpro/tools/bin/makerom
COPY --from=tools-builder /tmp/3ds-bannertool/build/bannertool /opt/devkitpro/tools/bin/bannertool

WORKDIR /project

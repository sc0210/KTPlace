# The CI build environment as a container: Ubuntu 24.04 with the same packages
# .github/workflows/ci.yml installs, so a Linux build, test run or placement can
# be reproduced on any machine -- including a Mac, where Docker runs it in a
# Linux VM of the host's own architecture, and on Windows, where it runs in WSL 2.
#
# Stages:
#
#   build  copies the sources in and builds them; the compile that `ci` and
#          `run` below share, not meant to be selected by hand.
#
#   ci     (the default) the build plus the unit tests, so `docker build`
#          alone is a Linux CI run:
#
#          docker build -t ktplace .
#          docker run -v "$PWD/output:/ktplace/output" ktplace \
#              ktplace benchmark/ISPD_2005/adaptec1 -w output/adaptec1-linux
#
#   run    the same binary without the toolchain that built it -- the engine
#          and the few libraries it links, nothing else -- so this is what to
#          run placements from:
#
#          docker build --target run -t ktplace-run .
#          docker run --rm -v "$PWD/output:/ktplace/output" ktplace-run \
#              benchmark/ISPD_2005/adaptec1 -w output/adaptec1
#
#   dev    a long-lived development container you SSH into, with the repository
#          mounted live rather than copied. Start it with scripts/devenv.sh,
#          which drives compose.yaml; see the README.

# ----------------------------------------------------------------- toolchain
FROM ubuntu:24.04 AS toolchain

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        g++ \
        git \
        make \
        python3 \
        libtbb-dev \
        libboost-iostreams-dev \
        libboost-test-dev \
        zlib1g-dev \
        libfmt-dev \
        clang-format \
    && rm -rf /var/lib/apt/lists/*

# ----------------------------------------------------------------------- dev
FROM toolchain AS dev

# What a day of work in the container needs on top of the build: the SSH server
# it is reached through, a debugger, and enough of a shell to be comfortable.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        openssh-server \
        gdb \
        sudo \
        less \
        vim-tiny \
        ca-certificates \
        bash-completion \
    && rm -rf /var/lib/apt/lists/*

# A non-root user, so files the container writes into the mounted repository are
# not owned by root. Ubuntu's image already has an `ubuntu` user at uid 1000, so
# it is renamed rather than a second user being added beside it; the entrypoint
# then moves it to the uid that owns the repository on the host, if different.
RUN usermod -l dev -d /home/dev -m ubuntu \
    && groupmod -n dev ubuntu \
    && usermod -s /bin/bash dev \
    && echo 'dev ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/dev \
    && chmod 0440 /etc/sudoers.d/dev

# Key-only SSH. Host keys live in a volume (see docker/dev-entrypoint.sh), so a
# rebuilt image keeps its identity and ssh does not warn about a changed host.
RUN mkdir -p /run/sshd \
    && rm -f /etc/ssh/ssh_host_* \
    && printf '%s\n' \
        'PasswordAuthentication no' \
        'KbdInteractiveAuthentication no' \
        'PermitRootLogin no' \
        'AllowUsers dev' \
        'HostKey /var/lib/ktplace-ssh/ssh_host_ed25519_key' \
        > /etc/ssh/sshd_config.d/ktplace.conf

# The repository is a bind mount, which git would otherwise refuse as owned by
# someone else.
#
# ktplace goes on PATH three ways, one per way in: ENV for `docker exec`,
# /etc/environment for SSH sessions (sshd reads it through pam_env), and the
# .bashrc lines below for both.
#
# Every top-level shell starts in /workspace, so `ssh ktplace-dev 'make test'`
# runs against the repository rather than the home directory. The snippet goes
# at the *top* of .bashrc (docker/bashrc-workspace.sh says why), and is a copied
# file rather than inline text: Docker's own handling of backslashes and `#`
# lines inside a RUN silently reshapes a multi-line string.
ENV PATH=/workspace/build/bin:$PATH
COPY docker/bashrc-workspace.sh /tmp/bashrc-workspace.sh
RUN git config --system --add safe.directory /workspace \
    && echo 'PATH="/workspace/build/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"' \
        > /etc/environment \
    && cat /tmp/bashrc-workspace.sh /home/dev/.bashrc > /tmp/bashrc \
    && install -m 644 -o dev -g dev /tmp/bashrc /home/dev/.bashrc \
    && rm /tmp/bashrc /tmp/bashrc-workspace.sh

# chmod rather than trusting the mode: a build context sent from Windows does
# not carry the executable bit.
COPY docker/dev-entrypoint.sh /usr/local/bin/dev-entrypoint.sh
RUN chmod 755 /usr/local/bin/dev-entrypoint.sh
WORKDIR /workspace
EXPOSE 22
ENTRYPOINT ["/usr/local/bin/dev-entrypoint.sh"]

# ---------------------------------------------------------------------- build
# The compile that `ci` and `run` share. Stripped here, and not in `run`: a
# strip after the COPY would add a layer but leave the unstripped 19 MB copy
# in the layer below it, so the image would not shrink at all. The symbols
# are no loss to `ci` -- the unit tests link their own objects, and a debugger
# belongs in the dev container, which builds its own tree anyway.
FROM toolchain AS build

WORKDIR /ktplace
COPY . .

RUN make -j"$(nproc)" && strip build/bin/ktplace

# ------------------------------------------------------------------------ run
# The binary and what it links, and nothing else: no compiler, no sources, no
# objects. These four are the runtime packages behind the toolchain stage's
# -dev packages, on the same Ubuntu the binary was built on, so what runs here
# is exactly what was built -- a tenth of the ci image's size.
FROM ubuntu:24.04 AS run

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        libtbb12 \
        libboost-iostreams1.83.0 \
        zlib1g \
        libfmt9 \
    && rm -rf /var/lib/apt/lists/*

# The entrypoint rather than the command, so `docker run ktplace-run <args>`
# passes <args> to ktplace instead of replacing it. The vendored design comes
# along (5 MB), so the image runs a full placement with no mounts at all;
# mount benchmark/ for any other suite.
WORKDIR /ktplace
COPY --from=build /ktplace/build/bin/ktplace /usr/local/bin/ktplace
COPY --from=build /ktplace/benchmark/ISPD_2005/adaptec1 benchmark/ISPD_2005/adaptec1
ENTRYPOINT ["ktplace"]
CMD ["--help"]

# ------------------------------------------------------------------------ ci
# Last, so it is what a plain `docker build .` produces. The build itself
# happened in the stage above; what is left here is the test run.
FROM build AS ci

RUN make -j"$(nproc)" test

ENV PATH=/ktplace/build/bin:$PATH
CMD ["ktplace", "--help"]

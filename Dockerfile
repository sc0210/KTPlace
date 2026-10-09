# The CI build environment as a container: Ubuntu 24.04 with the same packages
# .github/workflows/ci.yml installs, so a Linux build, test run or placement can
# be reproduced on any machine -- including a Mac, where Docker runs it in a
# Linux VM of the host's own architecture, and on Windows, where it runs in WSL 2.
#
# Two targets share one toolchain stage:
#
#   ci   (the default) copies the sources in, builds them and runs the unit
#        tests, so `docker build` alone is a Linux CI run:
#
#          docker build -t ktplace .
#          docker run -v "$PWD/output:/ktplace/output" ktplace \
#              ktplace benchmark/ISPD_2005/adaptec1 -w output/adaptec1-linux
#
#   dev  a long-lived development container you SSH into, with the repository
#        mounted live rather than copied. Start it with scripts/devenv.sh, which
#        drives compose.yaml; see "Linux development container" in the README.

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
# it is renamed rather than a second user being added beside it.
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
# runs against the repository rather than the home directory. The lines go at
# the *top* of .bashrc: bash reads it for a command run over SSH, but Ubuntu's
# copy returns at once for any non-interactive shell, so lines appended after
# that guard only ever reached an interactive login. SHLVL=1 limits it to the
# shell sshd or docker exec starts, so a nested `bash` stays where it was run.
ENV PATH=/workspace/build/bin:$PATH
RUN git config --system --add safe.directory /workspace \
    && echo 'PATH="/workspace/build/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"' \
        > /etc/environment \
    && sed -i '1i\
# KTPlace dev container: top-level shells start in the repository.\
if [ "${SHLVL:-1}" = 1 ] && [ -d /workspace ]; then cd /workspace; fi\
' /home/dev/.bashrc

# chmod rather than trusting the mode: a build context sent from Windows does
# not carry the executable bit.
COPY docker/dev-entrypoint.sh /usr/local/bin/dev-entrypoint.sh
RUN chmod 755 /usr/local/bin/dev-entrypoint.sh
WORKDIR /workspace
EXPOSE 22
ENTRYPOINT ["/usr/local/bin/dev-entrypoint.sh"]

# ------------------------------------------------------------------------ ci
# Last, so it is what a plain `docker build .` produces.
FROM toolchain AS ci

WORKDIR /ktplace
COPY . .

RUN make -j"$(nproc)" && make -j"$(nproc)" test

ENV PATH=/ktplace/build/bin:$PATH
CMD ["ktplace", "--help"]

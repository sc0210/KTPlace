#!/usr/bin/env bash
# Manage the Docker environment: the Linux development container, the web
# console beside it, and the binaries both run (Dockerfile targets `dev` and
# `web`, driven through compose.yaml).
#
#   scripts/devenv.sh up [--no-build]  start both containers, wait for SSH,
#                                      then compile build/bin/ktplace inside
#                                      the dev container (the binary the web
#                                      console runs too)
#   scripts/devenv.sh build [targets]  make in the dev container, as `up` did
#   scripts/devenv.sh ssh [cmd]        SSH in, or run one command over SSH (a
#                                      shell string, as with ssh itself:
#                                      'make && make test')
#   scripts/devenv.sh exec [cmd]       the same through `docker exec`, no SSH
#                                      involved
#   scripts/devenv.sh status           what is running, and on which ports
#   scripts/devenv.sh ssh-config       print an ~/.ssh/config entry for it
#   scripts/devenv.sh stop             stop them; containers and volumes kept
#   scripts/devenv.sh down             remove the containers; volumes kept
#
# Environment:
#   KTPLACE_SSH_PUBKEY  public key to install (default: the first of
#                       ~/.ssh/id_ed25519.pub, id_ecdsa.pub, id_rsa.pub)
#   KTPLACE_SSH_PORT    host port for SSH, bound to 127.0.0.1 (default 2222)
#   KTPLACE_WEB_PORT    host port for the web console, bound to 127.0.0.1
#                       (default 8080)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
port="${KTPLACE_SSH_PORT:-2222}"
export KTPLACE_SSH_PORT="$port"

# The public key to install, or nothing if there is none. An explicit
# KTPLACE_SSH_PUBKEY must exist: compose would otherwise refuse the mount with a
# message about bind sources rather than about the key.
find_key() {
    if [ -n "${KTPLACE_SSH_PUBKEY:-}" ]; then
        [ -f "$KTPLACE_SSH_PUBKEY" ] && printf '%s\n' "$KTPLACE_SSH_PUBKEY"
        return 0
    fi
    for k in "$HOME/.ssh/id_ed25519.pub" "$HOME/.ssh/id_ecdsa.pub" "$HOME/.ssh/id_rsa.pub"; do
        if [ -s "$k" ]; then
            printf '%s\n' "$k"
            return 0
        fi
    done
}

# The same, for the commands that cannot do without one.
need_key() {
    local k
    k="$(find_key)"
    if [ -z "$k" ]; then
        if [ -n "${KTPLACE_SSH_PUBKEY:-}" ]; then
            echo "devenv: KTPLACE_SSH_PUBKEY=$KTPLACE_SSH_PUBKEY does not exist." >&2
        else
            echo "devenv: no SSH public key found; create one with ssh-keygen -t ed25519," >&2
            echo "        or point KTPLACE_SSH_PUBKEY at one." >&2
        fi
        exit 1
    fi
    printf '%s\n' "$k"
}

# Passes on whichever key was found, so a non-default one (id_ecdsa, id_rsa) is
# installed too -- compose.yaml on its own only knows id_ed25519. Commands that
# do not create a container (stop, down, ps) run without one.
compose() {
    local k
    k="$(find_key)"
    if [ -n "$k" ]; then
        KTPLACE_SSH_PUBKEY="$k" docker compose -f "$root/compose.yaml" "$@"
    else
        docker compose -f "$root/compose.yaml" "$@"
    fi
}

# The one compile, in the dev container against the live-mounted repository:
# build/ is a volume there, so the objects are Linux ones, survive a container
# restart, and are the same tree the web console runs the engine from (see
# compose.yaml). Extra arguments are passed to make as targets.
build_tree() {
    local flags=(-i)
    local targets="$*"
    [ -t 1 ] && flags=(-it)
    docker exec "${flags[@]}" -u dev -w /workspace ktplace-dev \
        bash -lc "make -j\"\$(nproc)\" $targets"
}

# The key the container was given is the one to offer: with a non-default
# KTPLACE_SSH_PUBKEY, ssh's default identities would all be refused.
ssh_opts() {
    local k
    k="$(need_key)"
    ssh_opts=(-p "$port" -o HostKeyAlias=ktplace-dev -i "${k%.pub}" -o IdentitiesOnly=yes)
}

case "${1:-}" in
    up)
        key="$(need_key)"
        ssh_opts
        echo "devenv: installing $key"
        compose up -d --build
        # sshd needs a moment after the container starts. The last error is kept,
        # because the likely failures -- a changed host key after the host-key
        # volume was recreated, a key the container does not have -- are only
        # named in ssh's own message.
        ready=0
        err=""
        for _ in $(seq 1 30); do
            if err=$(ssh "${ssh_opts[@]}" -o BatchMode=yes -o ConnectTimeout=2 \
                -o StrictHostKeyChecking=accept-new dev@127.0.0.1 true 2>&1); then
                ready=1
                break
            fi
            sleep 1
        done
        if [ "$ready" != 1 ]; then
            echo "devenv: the container is up but SSH failed:" >&2
            printf '%s\n' "$err" | sed 's/^/        /' >&2
            if printf '%s' "$err" | grep -q "IDENTIFICATION HAS CHANGED"; then
                echo "        The container's host key is new (its volume was recreated)." >&2
                echo "        Forget the old one with: ssh-keygen -R ktplace-dev" >&2
            else
                echo "        See also: docker logs ktplace-dev" >&2
            fi
            exit 1
        fi

        # The compile, after SSH is known to work so a failure can be fixed
        # the usual way. build/ is a volume, so this is incremental: only what
        # the sources changed since the last time needs recompiling, and the
        # web console picks the result up on its next run, without a restart.
        if [ "${2:-}" != "--no-build" ]; then
            echo
            echo "devenv: building build/bin/ktplace in the dev container..."
            if ! build_tree; then
                echo "devenv: the build failed. Fix it with scripts/devenv.sh build," >&2
                echo "        then start the console -- it runs the same binary." >&2
                exit 1
            fi
        fi

        echo
        echo "Ready. One environment, both halves:"
        echo "  shell   scripts/devenv.sh ssh        (ssh -p $port dev@127.0.0.1)"
        echo "  web     http://127.0.0.1:${KTPLACE_WEB_PORT:-8080}"
        echo "  engine  build/bin/ktplace -- one Linux build, shared by both"
        echo "Rebuild with scripts/devenv.sh build; the console needs no restart."
        ;;
    ssh)
        shift
        ssh_opts
        # A terminal only when there is one to give: `-t` from a script or a pipe
        # just warns. A one-off command runs from the repository, as a login does.
        tty=()
        [ -t 0 ] && tty=(-t)
        if [ $# -eq 0 ]; then
            exec ssh "${ssh_opts[@]}" -o StrictHostKeyChecking=accept-new ${tty[@]+"${tty[@]}"} \
                dev@127.0.0.1
        fi
        exec ssh "${ssh_opts[@]}" -o StrictHostKeyChecking=accept-new ${tty[@]+"${tty[@]}"} \
            dev@127.0.0.1 "cd /workspace && $*"
        ;;
    exec)
        shift
        flags=(-i)
        [ -t 0 ] && flags=(-it)
        if [ $# -eq 0 ]; then
            exec docker exec "${flags[@]}" -u dev -w /workspace ktplace-dev bash -l
        fi
        exec docker exec "${flags[@]}" -u dev -w /workspace ktplace-dev bash -lc "$*"
        ;;
    build)
        shift
        if [ -z "$(compose ps -q dev 2>/dev/null)" ]; then
            echo "devenv: the dev container is not running; run scripts/devenv.sh up." >&2
            exit 1
        fi
        build_tree "$@"
        ;;
    status)
        compose ps
        ;;
    ssh-config)
        key="$(need_key)"
        cat <<EOF
# Add to ~/.ssh/config, then: ssh ktplace-dev  (or VS Code: Remote-SSH -> ktplace-dev)
Host ktplace-dev
    HostName 127.0.0.1
    Port $port
    User dev
    HostKeyAlias ktplace-dev
    IdentityFile ${key%.pub}
EOF
        ;;
    stop)
        compose stop
        ;;
    down)
        compose down
        ;;
    *)
        sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'
        exit 2
        ;;
esac

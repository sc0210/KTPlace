#!/usr/bin/env bash
# Manage the Linux development container (Dockerfile target `dev`, compose.yaml).
#
#   scripts/devenv.sh up          build if needed, start, and print how to connect
#   scripts/devenv.sh ssh [cmd]   SSH in, or run one command over SSH (a shell
#                                 string, as with ssh itself: 'make && make test')
#   scripts/devenv.sh exec [cmd]  the same through `docker exec`, no SSH involved
#   scripts/devenv.sh status      is it running, and on which port
#   scripts/devenv.sh ssh-config  print an ~/.ssh/config entry for it
#   scripts/devenv.sh stop        stop it; the container and its volumes are kept
#   scripts/devenv.sh down        remove the container; the build volumes are kept
#
# Environment:
#   KTPLACE_SSH_PUBKEY  public key to install (default: the first of
#                       ~/.ssh/id_ed25519.pub, id_ecdsa.pub, id_rsa.pub)
#   KTPLACE_SSH_PORT    host port for SSH, bound to 127.0.0.1 (default 2222)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
port="${KTPLACE_SSH_PORT:-2222}"
export KTPLACE_SSH_PORT="$port"

pick_key() {
    if [ -n "${KTPLACE_SSH_PUBKEY:-}" ]; then
        printf '%s\n' "$KTPLACE_SSH_PUBKEY"
        return
    fi
    for k in "$HOME/.ssh/id_ed25519.pub" "$HOME/.ssh/id_ecdsa.pub" "$HOME/.ssh/id_rsa.pub"; do
        if [ -s "$k" ]; then
            printf '%s\n' "$k"
            return
        fi
    done
    echo "devenv: no SSH public key found; create one with ssh-keygen -t ed25519," >&2
    echo "        or point KTPLACE_SSH_PUBKEY at one." >&2
    exit 1
}

compose() {
    # The key is resolved here so a non-default key (id_ecdsa, id_rsa) is used
    # too; compose.yaml on its own only knows id_ed25519.
    KTPLACE_SSH_PUBKEY="$(pick_key)" docker compose -f "$root/compose.yaml" "$@"
}

# The key the container was given is the one to offer: with a non-default
# KTPLACE_SSH_PUBKEY, ssh's default identities would all be refused.
key="$(pick_key)"
ssh_opts=(-p "$port" -o HostKeyAlias=ktplace-dev -i "${key%.pub}" -o IdentitiesOnly=yes)

case "${1:-}" in
    up)
        echo "devenv: installing $key"
        KTPLACE_SSH_PUBKEY="$key" docker compose -f "$root/compose.yaml" up -d --build
        # sshd needs a moment after the container starts.
        ready=0
        for _ in $(seq 1 30); do
            if ssh "${ssh_opts[@]}" -o BatchMode=yes -o ConnectTimeout=2 \
                -o StrictHostKeyChecking=accept-new dev@127.0.0.1 true 2>/dev/null; then
                ready=1
                break
            fi
            sleep 1
        done
        if [ "$ready" != 1 ]; then
            echo "devenv: the container is up but SSH did not accept ${key%.pub}." >&2
            echo "        See: docker logs ktplace-dev" >&2
            exit 1
        fi
        echo
        echo "Ready. Connect with:"
        echo "  scripts/devenv.sh ssh"
        echo "  ssh -p $port dev@127.0.0.1"
        echo "First build inside it:  make -j\"\$(nproc)\" && make test"
        ;;
    ssh)
        shift
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
    status)
        compose ps
        ;;
    ssh-config)
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
        sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
        exit 2
        ;;
esac

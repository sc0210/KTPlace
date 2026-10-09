#!/bin/sh
# Entry point of the dev container: install the SSH key it was given, make sure
# it has a host key, then run sshd in the foreground as the container's process.
set -eu

# The host's public key is mounted read-only at this path by compose.yaml. It is
# copied rather than used in place, because sshd insists the file belong to the
# user and not be writable by others, and a bind mount from the host carries the
# host's ownership and mode.
key_src=/run/ktplace/authorized_keys
ssh_dir=/home/dev/.ssh
if [ -s "$key_src" ]; then
    install -d -m 700 -o dev -g dev "$ssh_dir"
    install -m 600 -o dev -g dev "$key_src" "$ssh_dir/authorized_keys"
else
    echo "dev-entrypoint: no public key at $key_src; SSH logins will be refused." >&2
    echo "                Start the container with scripts/devenv.sh, which mounts one." >&2
fi

# Generated once and kept in a volume, so the container's identity survives a
# rebuild and ssh does not report a changed host key every time the image moves.
host_dir=/var/lib/ktplace-ssh
mkdir -p "$host_dir"
if [ ! -f "$host_dir/ssh_host_ed25519_key" ]; then
    ssh-keygen -q -t ed25519 -N '' -f "$host_dir/ssh_host_ed25519_key"
fi

# The build trees are volumes, which Docker creates owned by root.
chown dev:dev /workspace/build /workspace/build-cov 2>/dev/null || true

exec /usr/sbin/sshd -D -e

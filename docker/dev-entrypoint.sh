#!/bin/sh
# Entry point of the dev container: match the dev user to the repository's
# owner, install the SSH key it was given, make sure it has a host key, then run
# sshd in the foreground as the container's process.
set -eu

# Give `dev` the uid/gid that owns the mounted repository, so files the
# container writes there belong to the host user. On a Linux host the image's
# uid 1000 is only right by luck; a host user of 1001 would otherwise get files
# owned by someone else and could not let the container write into 755
# directories. Read from a tracked file, not from /workspace itself: Docker
# Desktop (macOS, Windows) presents the mount point as root and the files as the
# accessing user, so there the owner is root or already 1000, and nothing moves.
owner_uid=$(stat -c %u /workspace/Makefile 2>/dev/null || echo 0)
owner_gid=$(stat -c %g /workspace/Makefile 2>/dev/null || echo 0)
if [ "$owner_uid" != 0 ] && [ "$owner_uid" != "$(id -u dev)" ]; then
    groupmod -o -g "$owner_gid" dev
    usermod -o -u "$owner_uid" -g "$owner_gid" dev
    chown -R dev:dev /home/dev
fi

# The host's public key is mounted read-only at this path by compose.yaml. It is
# copied rather than used in place, because sshd insists the file belong to the
# user and not be writable by others, and a bind mount from the host carries the
# host's ownership and mode. A missing or unusable key is reported, not fatal:
# sshd still starts, `docker exec` still works, and the log says why ssh fails.
key_src=/run/ktplace/authorized_keys
ssh_dir=/home/dev/.ssh
if [ -f "$key_src" ] && [ -s "$key_src" ] \
    && install -d -m 700 -o dev -g dev "$ssh_dir" \
    && install -m 600 -o dev -g dev "$key_src" "$ssh_dir/authorized_keys"; then
    :
else
    echo "dev-entrypoint: no usable public key at $key_src; SSH logins will be refused." >&2
    echo "                Point KTPLACE_SSH_PUBKEY at a .pub file and recreate the container." >&2
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

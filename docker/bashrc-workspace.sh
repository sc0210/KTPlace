# KTPlace dev container: top-level shells start in the repository.
#
# Prepended to the dev user's .bashrc, above Ubuntu's early return for
# non-interactive shells, so it also applies to `ssh host 'command'` (bash
# reads .bashrc for those, then the stock file returns at once). SHLVL=1 keeps
# it to the shell sshd or docker exec starts: a nested `bash` stays put.
if [ "${SHLVL:-1}" = 1 ] && [ -d /workspace ]; then cd /workspace; fi


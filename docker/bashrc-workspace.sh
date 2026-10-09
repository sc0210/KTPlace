# KTPlace dev container: shells that start in the home directory start in the
# repository instead.
#
# Prepended to the dev user's .bashrc, above Ubuntu's early return for
# non-interactive shells, so it also applies to `ssh host 'command'` (bash
# reads .bashrc for those, then the stock file returns at once).
#
# Keyed on the starting directory, because that is what an SSH session has and
# a nested shell does not: sshd starts every session in $HOME, while
# `ssh host 'cd output && bash run.sh'` starts run.sh in output/, and moving it
# would break the script. SHLVL alone cannot tell the two apart -- bash lowers
# it before exec'ing the last command of a -c string, so that nested bash looks
# top-level -- but it still keeps an interactive `bash` typed in ~ where it is.
if [ "$PWD" = "$HOME" ] && [ "${SHLVL:-1}" -le 1 ] && [ -d /workspace ]; then
    cd /workspace
fi


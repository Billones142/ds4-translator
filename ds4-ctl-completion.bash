# Generic wrapper: asks the ds4-ctl binary itself for its command list and
# per-command argument completions (via hidden --commands/--complete-args
# queries) instead of hardcoding them here. Adding a new command to
# src/ctl.cpp's kCommands table (or a button to kButtons) is all that's
# needed to keep this in sync — nothing in this file ever needs editing.
_ds4_ctl_completions() {
    local cur
    COMPREPLY=()
    cur="${COMP_WORDS[COMP_CWORD]}"

    if [ "${COMP_CWORD}" -eq 1 ]; then
        COMPREPLY=( $(compgen -W "$(ds4-ctl --commands 2>/dev/null)" -- "${cur}") )
        return 0
    fi

    local args
    args=$(ds4-ctl --complete-args "${COMP_WORDS[1]}" 2>/dev/null)
    if [ -n "${args}" ]; then
        COMPREPLY=( $(compgen -W "${args}" -- "${cur}") )
    fi
    return 0
}
complete -F _ds4_ctl_completions ds4-ctl

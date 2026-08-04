_ds4_ctl_completions() {
    local cur prev opts
    COMPREPLY=()
    cur="${COMP_WORDS[COMP_CWORD]}"
    prev="${COMP_WORDS[COMP_CWORD-1]}"
    
    opts="status set-type set-backend help"
    
    case "${prev}" in
        set-type)
            COMPREPLY=( $(compgen -W "ds4 dualsense none" -- "${cur}") )
            return 0
            ;;
        set-backend)
            COMPREPLY=( $(compgen -W "uhid gadget" -- "${cur}") )
            return 0
            ;;
        *)
            ;;
    esac
    
    COMPREPLY=( $(compgen -W "${opts}" -- "${cur}") )
    return 0
}
complete -F _ds4_ctl_completions ds4-ctl

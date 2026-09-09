# TenebraOS Fish Shell Configuration
# /etc/skel/.config/fish/config.fish

# ─── Environment ─────────────────────────────────────────────────────────────
set -gx COLORTERM truecolor
set -gx TERM xterm-256color
set -gx EDITOR nvim
set -gx VISUAL nvim

# ─── Aliases ─────────────────────────────────────────────────────────────────
# Package management
alias pkg-install 'pkg install'
alias pkg-search 'pkg search'
alias pkg-update 'pkg update'

# System
alias ls 'ls --color=auto'
alias ll 'ls -la'
alias la 'ls -A'
alias l 'ls -CF'
alias grep 'grep --color=auto'
alias df 'df -h'
alias du 'du -h'
alias free 'free -h'

# Navigation
alias .. 'cd ..'
alias ... 'cd ../..'
alias .... 'cd ../../..'
alias ~ 'cd ~'

# Git
alias g 'git'
alias gs 'git status'
alias ga 'git add'
alias gc 'git commit'
alias gp 'git push'
alias gl 'git log'
alias gd 'git diff'

# TenebraOS specific
alias tenebra-kernel 'tenebra-kernel'
alias tenebra-pkg 'tenebra-pkg'
alias rebuild-grub 'sudo update-grub'

# ─── Prompt ──────────────────────────────────────────────────────────────────
function fish_prompt
    set -l last_status $status
    set -l normal (set_color normal)
    set -l red (set_color ff6b6b)
    set -l green (set_color 51cf66)
    set -l yellow (set_colorffd43b)
    set -l blue (set_color 339af0)
    set -l cyan (set_color 22b8cf)
    set -l white (set_color f8f9fa)
    
    # Username
    if [ (id -u) -eq 0 ]
        echo -n $red"(root)"$normal
    else
        echo -n $cyan$USER$normal
    end
    
    # @ Hostname
    echo -n $white"@"(hostname | cut -d. -f1)$normal
    
    # : Directory
    echo -n $white":"$normal
    if [ (pwd) = "$HOME" ]
        echo -n $yellow"~"$normal
    else
        echo -n $yellow(prompt_pwd)$normal
    end
    
    # Git branch
    if set -l branch (git branch --show-current 2>/dev/null)
        echo -n $green" ($branch)"$normal
    end
    
    # Prompt symbol
    if [ $last_status -eq 0 ]
        echo -n $green' > '$normal
    else
        echo -n $red' > '$normal
    end
end

# ─── Colors ──────────────────────────────────────────────────────────────────
# LS_COLORS
set -gx LS_COLORS 'di=34:ln=35:so=32:pi=33:ex=31:bd=34;46:cd=34;43:su=31;40:sg=31;40:tw=34;42:ow=34;43'

# ─── Completion ──────────────────────────────────────────────────────────────
# Enable completions for custom commands
complete -c tenebra-kernel -f
complete -c tenebra-kernel -n '__tenebra_kernel_needs_command' -a 'list list-available install remove default info update-grub'
complete -c tenebra-pkg -f
complete -c pkg -f

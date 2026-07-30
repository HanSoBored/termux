# ---- Environment ----
source ~/.config/zsh/export

# ---- History ----
HISTSIZE=5000
HISTFILE=~/.zsh_history
setopt SHARE_HISTORY
setopt HIST_IGNORE_DUPS
setopt HIST_IGNORE_SPACE

# ---- Starship Prompt ----
eval "$(starship init zsh)"

# ---- Plugins ----
source ~/.zsh/zsh-syntax-highlighting/zsh-syntax-highlighting.zsh
source ~/.zsh/zsh-autosuggestions/zsh-autosuggestions.zsh

# ---- FZF ----
source <(fzf --zsh)

# ---- Key Bindings ----
source ~/.config/zsh/bindkey

# ---- Aliases ----
source ~/.config/zsh/alias

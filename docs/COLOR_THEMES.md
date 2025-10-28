# Color Theme Support

The TUI uses **Kitty terminal's theme format** - a simple, dependency-free configuration format. This gives you access to 300+ professionally-designed themes from the kitty-themes ecosystem!

## Configuration

**Default location:**
```bash
~/.config/claude-c/theme.conf
```

**Environment variable override:**
```bash
export CLAUDE_C_THEME="/path/to/your/theme.conf"
./claude-c "your prompt"
```

## Theme Format

Kitty's dead-simple key-value format - no parser library needed!

```conf
# Claude TUI Theme
background #282a36
foreground #f8f8f2
cursor #bbbbbb
selection_background #44475a

# 16 ANSI colors
color0 #000000
color1 #ff5555
color2 #50fa7b
color3 #f1fa8c
# ... color4-15

# TUI-specific (optional)
assistant_fg #8be9fd
user_fg #50fa7b
status_bg #44475a
error_fg #ff5555
```

## Why Kitty's format?

- ✅ Zero dependencies - no parser library needed
- ✅ Trivial to parse in C (~50 lines)
- ✅ 300+ themes available from kitty-themes
- ✅ Human-readable and editable
- ✅ Compatible with Kitty terminal themes
- ✅ Faster than structured formats (TOML/YAML)

## Using Kitty themes directly

Most Kitty themes work out of the box! Download from [kitty-themes](https://github.com/dexpota/kitty-themes):

```bash
# Download a theme
curl -o ~/.config/claude-c/dracula.conf \
  https://raw.githubusercontent.com/dexpota/kitty-themes/master/themes/Dracula.conf

# Use it
export CLAUDE_C_THEME=~/.config/claude-c/dracula.conf
./claude-c "your prompt"
```

If no theme is specified, sensible defaults are used.
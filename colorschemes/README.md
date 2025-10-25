# Claude Code TUI Themes

This directory contains color themes in Kitty terminal format. These themes are compatible with 300+ themes from [kitty-themes](https://github.com/dexpota/kitty-themes).

## Available Themes

### Kitty Default
**File:** `kitty-default.conf`

The official default color scheme from Kitty terminal.
- **Background:** Black (#000000)
- **Foreground:** Light gray (#dddddd)
- **Style:** Classic, high contrast
- **Best for:** Traditional terminal experience

### Dracula
**File:** `dracula.conf`

Popular dark theme with vibrant colors.
- **Background:** Dark purple (#1e1f28 → color 234)
- **Foreground:** Off-white (#f8f8f2)
- **Accent:** Cyan (#8ae9fc → color 116)
- **Status:** Pale yellow (#f0fa8b → color 186)
- **User:** Bright green (#50fa7b → color 78)
- **Error:** Pink-red (#ff5555 → color 203)
- **Style:** Modern, vibrant, purple tones
- **Best for:** Long coding sessions, eye comfort

### Gruvbox Dark
**File:** `gruvbox-dark.conf`

Retro warm color scheme inspired by old-school terminals.
- **Background:** Dark brown (#282828 → color 235)
- **Foreground:** Warm beige (#ebdbb2 → color 187)
- **Accent:** Muted teal (#689d6a → color 108)
- **Status:** Orange (#d79921 → color 178)
- **User:** Yellow-green (#98971a → color 100)
- **Error:** Red (#cc241d)
- **Style:** Warm, retro, low contrast
- **Best for:** Reduced eye strain, vintage aesthetic

### Solarized Dark
**File:** `solarized-dark.conf`

Precision-designed color scheme with balanced contrast.
- **Background:** Deep blue-black (#001e26 → color 16)
- **Foreground:** Blue-gray (#708183 → color 102)
- **Accent:** Teal (#259185 → color 30)
- **Status:** Brown-gold (#a57705 → color 136)
- **User:** Yellow-green (#728905 → color 100)
- **Error:** Red (#d01b24)
- **Style:** Scientific, balanced, blue-tinted
- **Best for:** Professional work, color theory enthusiasts

## Usage

Set the `CLAUDE_THEME` environment variable:

```bash
# Use Dracula theme
export CLAUDE_THEME="./colorschemes/dracula.conf"
./build/claude

# Use Gruvbox theme
export CLAUDE_THEME="./colorschemes/gruvbox-dark.conf"
./build/claude

# Use Solarized theme
export CLAUDE_THEME="./colorschemes/solarized-dark.conf"
./build/claude
```

Or use an absolute path:
```bash
export CLAUDE_THEME="/Users/username/code/claude-c/colorschemes/dracula.conf"
./build/claude
```

Default theme locations (checked in order):
1. `$CLAUDE_THEME` environment variable
2. `$XDG_CONFIG_HOME/claude/theme.conf` (typically `~/.config/claude/theme.conf`)
3. `$HOME/.claude/theme.conf`
4. Built-in defaults

## Theme Format

Themes use Kitty's simple key-value format:

```conf
# Comments start with #
background #282a36
foreground #f8f8f2

# 16 ANSI colors
color0 #000000
color1 #ff5555
color2 #50fa7b
# ... etc

# Optional TUI-specific overrides
assistant_fg #8be9fd
user_fg #50fa7b
status_bg #44475a
```

## Color Mappings

The TUI maps Kitty colors to UI elements:

| TUI Element | Kitty Key (Primary) | Kitty Key (Fallback) |
|-------------|---------------------|----------------------|
| Assistant text | `foreground` | `assistant_fg` |
| User text | `color2` (green) | `user_fg` |
| Status bar | `color3` (yellow) | `status_bg` |
| Headers | `color6` (cyan) | `color4` (blue), `header_fg` |
| Errors | `color1` (red) | `error_fg` |
| Background | `background` | - |

## Adding New Themes

1. Download any Kitty theme from [kitty-themes](https://github.com/dexpota/kitty-themes)
2. Save to this directory with `.conf` extension
3. Use with `CLAUDE_THEME` environment variable

Example:
```bash
# Download Nord theme
curl -O https://raw.githubusercontent.com/dexpota/kitty-themes/master/themes/Nord.conf
mv Nord.conf colorschemes/nord.conf

# Use it
export CLAUDE_THEME="./colorschemes/nord.conf"
./build/claude
```

## Technical Details

- Supports 256-color terminals (automatically detects terminal capability)
- Hex colors (#RRGGBB) converted to ncurses color numbers
- Grayscale colors (232-255) for subtle backgrounds
- RGB cube (16-231) for full color spectrum
- Fallback to 8-color mode for legacy terminals

#include "builtin_themes.h"
#include <string.h>

// Built-in themes embedded as raw .conf content
const BuiltInTheme built_in_themes[] = {
    { "dracula", 
      "# Dracula Theme for Kitty\n"
      "# https://draculatheme.com/\n"
      "\n"
      "background #1e1f28\n"
      "foreground #f8f8f2\n"
      "cursor #bbbbbb\n"
      "selection_background #44475a\n"
      "selection_foreground #1e1f28\n"
      "\n"
      "color0 #000000\n"
      "color8 #545454\n"
      "color1 #ff5555\n"
      "color9 #ff5454\n"
      "color2 #50fa7b\n"
      "color10 #50fa7b\n"
      "color3 #f0fa8b\n"
      "color11 #f0fa8b\n"
      "color4 #bd92f8\n"
      "color12 #bd92f8\n"
      "color5 #ff78c5\n"
      "color13 #ff78c5\n"
      "color6 #8ae9fc\n"
      "color14 #8ae9fc\n"
      "color7 #bbbbbb\n"
      "color15 #ffffff\n" },
    { "gruvbox-dark",
      "# gruvbox dark by morhetz, https://github.com/morhetz/gruvbox\n"
      "# This work is licensed under the terms of the MIT license.\n"
      "# For a copy, see https://opensource.org/licenses/MIT.\n"
      "\n"
      "background #282828\n"
      "foreground #ebdbb2\n"
      "\n"
      "cursor #928374\n"
      "selection_foreground #928374\n"
      "selection_background #3c3836\n"
      "\n"
      "color0 #282828\n"
      "color8 #928374\n"
      "color1 #cc241d\n"
      "color9 #fb4934\n"
      "color2 #98971a\n"
      "color10 #b8bb26\n"
      "color3 #d79921\n"
      "color11 #fabd2d\n"
      "color4 #458588\n"
      "color12 #83a598\n"
      "color5 #b16286\n"
      "color13 #d3869b\n"
      "color6 #689d6a\n"
      "color14 #8ec07c\n"
      "color7 #a89984\n"
      "color15 #928374\n" },
    { "kitty-default",
      "# Kitty Default Theme\n"
      "# Classic high contrast\n"
      "\n"
      "background #000000\n"
      "foreground #ffffff\n"
      "\n"
      "cursor #ffffff\n"
      "\n"
      "color0 #000000\n"
      "color8 #555555\n"
      "color1 #ff0000\n"
      "color9 #ff5555\n"
      "color2 #00ff00\n"
      "color10 #55ff55\n"
      "color3 #ffff00\n"
      "color11 #ffff55\n"
      "color4 #0000ff\n"
      "color12 #5555ff\n"
      "color5 #ff00ff\n"
      "color13 #ff55ff\n"
      "color6 #00ffff\n"
      "color14 #55ffff\n"
      "color7 #cccccc\n"
      "color15 #ffffff\n" },
    { "solarized-dark",
      "# Solarized Dark Theme for Kitty\n"
      "# https://ethanschoonover.com/solarized/\n"
      "\n"
      "background #001e26\n"
      "foreground #708183\n"
      "cursor #708183\n"
      "selection_background #002731\n"
      "selection_foreground #001e26\n"
      "\n"
      "color0 #002731\n"
      "color8 #001e26\n"
      "color1 #d01b24\n"
      "color9 #bd3612\n"
      "color2 #728905\n"
      "color10 #465a61\n"
      "color3 #a57705\n"
      "color11 #52676f\n"
      "color4 #2075c7\n"
      "color12 #708183\n"
      "color5 #c61b6e\n"
      "color13 #5856b9\n"
      "color6 #259185\n"
      "color14 #81908f\n"
      "color7 #e9e2cb\n"
      "color15 #fcf4dc\n" }
};

const size_t built_in_themes_count = sizeof(built_in_themes) / sizeof(built_in_themes[0]);

const char *get_builtin_theme_content(const char *filepath) {
    // Extract base name
    const char *base = strrchr(filepath, '/');
    base = base ? base + 1 : filepath;
    
    // Remove .conf extension if present
    size_t len = strlen(base);
    char key[64];
    size_t key_len;
    if (len > 5 && strcmp(base + len - 5, ".conf") == 0) {
        key_len = len - 5;
    } else {
        key_len = len;
    }
    if (key_len >= sizeof(key)) {
        key_len = sizeof(key) - 1;
    }
    memcpy(key, base, key_len);
    key[key_len] = '\0';

    // Search built-in themes
    for (size_t i = 0; i < built_in_themes_count; ++i) {
        if (strcmp(built_in_themes[i].name, key) == 0) {
            return built_in_themes[i].content;
        }
    }
    return NULL;
}

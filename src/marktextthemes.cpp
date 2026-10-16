#include "marktextthemes.h"

namespace MarkTextThemes {

// 浅色主题（theme.ts 第 37~130 行）
const Entry kLightThemes[] = {
    { "ayu-light",         "Ayu 浅色" },
    { "light",             "Cadmium 浅色" },
    { "catppuccin-latte",  "Catppuccin Latte" },
    { "everforest-light",  "Everforest 浅色" },
    { "graphite",          "Graphite 浅色" },
    { "gruvbox-light",     "Gruvbox 浅色" },
    { "rose-pine-dawn",    "Rosé Pine Dawn" },
    { "solarized-light",   "Solarized 浅色" },
    { "tokyo-night-light", "Tokyo Night 浅色" },
    { "ulysses",           "Ulysses 浅色" }
};
const int kLightThemeCount = static_cast<int>(sizeof(kLightThemes) / sizeof(kLightThemes[0]));

// 深色主题（theme.ts 第 140~370 行）
const Entry kDarkThemes[] = {
    { "ayu-dark",          "Ayu 深色" },
    { "ayu-mirage",        "Ayu Mirage" },
    { "dark",              "Cadmium 深色" },
    { "catppuccin-mocha",  "Catppuccin Mocha" },
    { "cyberdream",        "cyberdream" },
    { "dracula",           "Dracula" },
    { "everforest-dark",   "Everforest 深色" },
    { "gruvbox-dark",      "Gruvbox 深色" },
    { "horizon-dark",      "Horizon 深色" },
    { "kanagawa",          "Kanagawa" },
    { "material-dark",     "Material 深色" },
    { "monokai-pro",       "Monokai Pro" },
    { "nightfox",          "Nightfox" },
    { "nord",              "Nord" },
    { "one-dark",          "One Dark" },
    { "oxocarbon-dark",    "Oxocarbon 深色" },
    { "palenight",         "Palenight" },
    { "rose-pine",         "Rosé Pine" },
    { "rose-pine-moon",    "Rosé Pine Moon" },
    { "solarized-dark",    "Solarized 深色" },
    { "synthwave-84",      "Synthwave '84" },
    { "tokyo-night",       "Tokyo Night" },
    { "tokyo-night-storm", "Tokyo Night Storm" }
};
const int kDarkThemeCount = static_cast<int>(sizeof(kDarkThemes) / sizeof(kDarkThemes[0]));

} // namespace MarkTextThemes

#include "marktextthemes.h"

#include <QSettings>

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

bool systemPrefersDark()
{
#ifdef Q_OS_WIN
    // AppsUseLightTheme：1 = 浅色，0 = 深色。键不存在（Win7/8 等老系统）时按浅色。
    // 用 QSettings 的 NativeFormat 直接读注册表，不额外引入 Win32 头。
    QSettings reg(QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows"
                                 "\\CurrentVersion\\Themes\\Personalize"),
                  QSettings::NativeFormat);
    if (reg.contains(QStringLiteral("AppsUseLightTheme"))) {
        return reg.value(QStringLiteral("AppsUseLightTheme")).toInt() == 0;
    }
#endif
    return false;
}

QString effectiveThemeId(const QString &sTheme, bool bFollowSystemTheme,
                         const QString &sLightModeTheme, const QString &sDarkModeTheme)
{
    if (!bFollowSystemTheme) {
        return sTheme;
    }
    return systemPrefersDark() ? sDarkModeTheme : sLightModeTheme;
}

} // namespace MarkTextThemes

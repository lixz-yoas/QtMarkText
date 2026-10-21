#ifndef MARKTEXTTHEMES_H
#define MARKTEXTTHEMES_H

#include <QString>

/**
 * @brief MarkText 主题清单（菜单与偏好设置窗口共用）
 *
 * 来源：packages/desktop/static/locales/zh-CN.json -> menu.theme.*
 * 顺序：照抄 packages/desktop/src/main/menu/templates/theme.ts
 *       （浅色在前、深色在后，各自保持原字母序）
 * id  ：即 MarkText 的主题 id，渲染层与配置（preference.theme）直接认这个值
 */
namespace MarkTextThemes {

struct Entry
{
    const char *pszId;
    const char *pszText;
};

/// 浅色主题表
extern const Entry kLightThemes[];
extern const int   kLightThemeCount;

/// 深色主题表
extern const Entry kDarkThemes[];
extern const int   kDarkThemeCount;

/**
 * @brief 系统当前是否处于深色模式
 *
 * Windows：读 HKCU\...\Themes\Personalize\AppsUseLightTheme（1=浅色、0=深色）。
 * 键不存在（老系统）或非 Windows 平台一律按浅色处理。
 * 对应 Electron 侧 nativeTheme.shouldUseDarkColors 的语义。
 */
bool systemPrefersDark();

/**
 * @brief 解析「实际生效的主题 id」
 *
 * MarkText 的 preference.theme 只是"选中的主题"，开启「跟随系统主题」后真正
 * 生效的是 lightModeTheme / darkModeTheme 之一 —— 这一步在原版里由 Electron
 * 主进程用 nativeTheme 解析后才把结果塞进渲染层的 URL。
 *
 * @param sTheme             偏好里选中的主题 id
 * @param bFollowSystemTheme 是否跟随系统
 * @param sLightModeTheme    系统浅色时使用的主题 id
 * @param sDarkModeTheme     系统深色时使用的主题 id
 * @return 实际要加载的主题 id
 */
QString effectiveThemeId(const QString &sTheme, bool bFollowSystemTheme,
                         const QString &sLightModeTheme, const QString &sDarkModeTheme);

} // namespace MarkTextThemes

#endif // MARKTEXTTHEMES_H

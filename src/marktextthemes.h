#ifndef MARKTEXTTHEMES_H
#define MARKTEXTTHEMES_H

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

} // namespace MarkTextThemes

#endif // MARKTEXTTHEMES_H

#ifndef PREFERENCESDIALOG_H
#define PREFERENCESDIALOG_H

#include <QDialog>
#include <QHash>
#include <QList>
#include <QPair>
#include <QString>

class PreferencesStore;
class QFormLayout;
class QListWidget;
class QStackedWidget;
class QTableWidget;
class QWidget;

/**
 * @brief 偏好设置窗口（「文件 -> 偏好设置 / Ctrl+,」打开的窗口）
 *
 * 对照 MarkText 的设置窗口（packages/desktop/src/renderer/src/prefComponents）：
 *   左侧分类顺序与名称照抄 zh-CN.json 的 preferences.categories：
 *       通用 / 编辑器 / Markdown / 拼写 / 主题 / 图片 / 快捷键
 *   右侧每一项的文案取自 zh-CN.json 的 preferences.items.*，
 *   键名与选项值与 preference.json、prefComponents 下各分类的 config.ts 一一对应。
 *
 * 与 MarkText 一致：改动**即时生效**，没有「确定/取消」，只有「关闭」。
 *
 * 本轮定位：这是「设置菜单」的 Qt 化落地。控件的落盘由 PreferencesStore 负责，
 * 控件的"实际效果"里只有外壳自己能兑现的那几项（字体、字号、语言、主题记录等）
 * 当场生效，其余一律等渲染层接管（界面上有明确标注，不假装能用）。
 */
class PreferencesDialog : public QDialog
{
    Q_OBJECT

public:
    /// 分类页（顺序即左栏顺序，照抄 MarkText）
    enum Page
    {
        PageGeneral = 0,
        PageEditor,
        PageMarkdown,
        PageSpelling,
        PageTheme,
        PageImage,
        PageKeybindings,
        PageCount
    };

    explicit PreferencesDialog(PreferencesStore *pStore, QWidget *parent = nullptr);
    ~PreferencesDialog() override;

    /// 控件类型
    enum ControlKind
    {
        KindCheck = 0,
        KindCombo,
        KindSpin,
        KindDoubleSpin,
        KindLineEdit
    };

    /// 一项偏好设置的描述（纯数据；公开是为了让文件内的静态表能直接书写）
    struct ItemSpec
    {
        int         nPage;        ///< 归属分类页
        ControlKind kind;         ///< 控件类型
        const char *pszKey;       ///< MarkText preference.json 的键名
        const char *pszLabel;     ///< 中文文案（zh-CN.json）
        int         nMin;         ///< Spin/DoubleSpin 下限
        int         nMax;         ///< Spin/DoubleSpin 上限
        int         nDecimals;    ///< DoubleSpin 小数位
        const char *pszOptions;   ///< Combo 选项："值|显示;值|显示"，或 <themes:light|dark|all>
    };

    /// 切到某一页
    void selectPage(int nPage);

    /// 由外壳把「菜单项 -> 快捷键」灌进来（零重复：直接取自 MarkTextMenu）
    void setKeybindings(const QList<QPair<QString, QString> > &listKeybindings);

signals:
    /// 主题相关配置变化（外壳据此刷新「主题」菜单的勾选态）
    void themeChanged(const QString &sThemeId, bool bFollowSystemTheme);
    /// 语言变化（外壳据此刷新菜单文案）
    void languageChanged(const QString &sLanguage);

private:
    /// 偏好项清单（数据表）
    static const QList<ItemSpec> &itemSpecs();

    QWidget *createPage(int nPage);
    QWidget *createKeybindingsPage();
    void     addItem(QFormLayout *pForm, const ItemSpec &spec);
    void     addNote(QFormLayout *pForm, const QString &sText);

    void onCheckToggled(const QString &sKey, bool bChecked);
    void onComboActivated(const QString &sKey, const QVariant &valueValue);
    void onSpinChanged(const QString &sKey, int nValue);
    void onDoubleChanged(const QString &sKey, double dValue);
    void onLineEdited(const QString &sKey);

    /// 解析 "<themes:xxx>" 或 "值|显示;值|显示"，返回 (值, 显示名) 列表
    static QList<QPair<QVariant, QString> > optionsOf(const ItemSpec &spec);

    PreferencesStore         *m_pStore;
    QListWidget              *m_pNav;
    QStackedWidget           *m_pStack;
    QHash<QString, QWidget *> m_hashControls;
    QTableWidget             *m_pKeyTable;
};

#endif // PREFERENCESDIALOG_H

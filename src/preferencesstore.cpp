#include "preferencesstore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QMetaType>
#include <QSettings>

//=====================================================================
//  QSettings 的 INI 后端会把所有值读成 QString（int/bool/double 一律丢类型），
//  而渲染层对类型敏感（例如 zoom 若是字符串，EDIT_ZOOM 里 zoomFactor.toFixed
//  会直接抛 "not a function"）。因此按缺省值的类型做一次归一化。
//=====================================================================
namespace {

QVariant coerceToDefaultType(const QVariant &vStored, const QVariant &vDefault)
{
    switch (static_cast<QMetaType::Type>(vDefault.userType())) {
    case QMetaType::Bool:     return vStored.toBool();
    case QMetaType::Int:      return vStored.toInt();
    case QMetaType::Double:   return vStored.toDouble();
    case QMetaType::QString:  return vStored.toString();
    default:                  return vStored;
    }
}

} // namespace

//=====================================================================
//  MarkText 缺省值表
//  来源：packages/desktop/static/preference.json（MarkText v0.19.1）
//  说明：只有最后一行的 language 做了有意偏离，其余逐键照抄。
//=====================================================================
const QHash<QString, QVariant> &PreferencesStore::defaults()
{
    static const QHash<QString, QVariant> kDefaults = []() {
        QHash<QString, QVariant> map;

        // ---- 通用 General ----
        // MarkText 缺省为 "en"；本外壳界面文案取自 MarkText 的 zh-CN 语言包，
        // 故缺省改为 "zh-CN"，避免"界面中文 / 配置英文"的错位。
        map.insert(QStringLiteral("language"),                QStringLiteral("zh-CN"));
        map.insert(QStringLiteral("autoSave"),                false);
        map.insert(QStringLiteral("autoSaveDelay"),           5000);
        map.insert(QStringLiteral("openFilesInNewWindow"),    false);
        map.insert(QStringLiteral("startUpAction"),           QStringLiteral("restoreAll"));
        map.insert(QStringLiteral("sideBarVisibility"),       false);
        map.insert(QStringLiteral("tabBarVisibility"),        false);
        map.insert(QStringLiteral("zoom"),                    1.0);
        map.insert(QStringLiteral("hideScrollbar"),           false);

        // ---- 编辑器 Editor ----
        map.insert(QStringLiteral("editorFontFamily"),        QStringLiteral("Open Sans"));
        map.insert(QStringLiteral("fontSize"),                16);
        // 行内代码字体（对应 MarkText preference.json 的 codeFontFamily/codeFontSize）。
        // 注意：codeFontSize 若缺省，startUrl 的 cfs 参数会算成 0，
        // 渲染层 addCommonStyle 会生成 `font-size: 0px`，行内代码整列不可见。
        map.insert(QStringLiteral("codeFontFamily"),          QStringLiteral("DejaVu Sans Mono"));
        map.insert(QStringLiteral("codeFontSize"),            14);
        map.insert(QStringLiteral("lineHeight"),              1.6);
        map.insert(QStringLiteral("editorLineWidth"),         QString());
        map.insert(QStringLiteral("tabSize"),                 4);
        map.insert(QStringLiteral("endOfLine"),               QStringLiteral("default"));
        map.insert(QStringLiteral("defaultEncoding"),         QStringLiteral("utf8"));
        map.insert(QStringLiteral("autoGuessEncoding"),       true);
        map.insert(QStringLiteral("autoNormalizeLineEndings"),true);
        map.insert(QStringLiteral("trimTrailingNewline"),     3);
        map.insert(QStringLiteral("wrapCodeBlocks"),          false);
        map.insert(QStringLiteral("codeBlockLineNumbers"),    true);
        map.insert(QStringLiteral("autoPairBracket"),         true);
        map.insert(QStringLiteral("autoPairMarkdownSyntax"),  true);
        map.insert(QStringLiteral("autoPairQuote"),           true);
        map.insert(QStringLiteral("textDirection"),           QStringLiteral("ltr"));

        // ---- Markdown ----
        map.insert(QStringLiteral("preferHeadingStyle"),      QStringLiteral("atx"));
        map.insert(QStringLiteral("bulletListMarker"),        QStringLiteral("-"));
        map.insert(QStringLiteral("orderListDelimiter"),      QStringLiteral("."));
        map.insert(QStringLiteral("listIndentation"),         1);
        map.insert(QStringLiteral("frontmatterType"),         QStringLiteral("-"));
        map.insert(QStringLiteral("isHtmlEnabled"),           true);
        map.insert(QStringLiteral("isGitlabCompatibilityEnabled"), false);
        map.insert(QStringLiteral("superSubScript"),          false);
        map.insert(QStringLiteral("footnote"),                false);
        map.insert(QStringLiteral("sequenceTheme"),           QStringLiteral("hand"));
        map.insert(QStringLiteral("preferLooseListItem"),     true);

        // ---- 拼写 Spelling ----
        map.insert(QStringLiteral("spellcheckerEnabled"),     false);
        map.insert(QStringLiteral("spellcheckerNoUnderline"), false);
        map.insert(QStringLiteral("spellcheckerLanguage"),    QStringLiteral("en-US"));

        // ---- 主题 Theme ----
        map.insert(QStringLiteral("followSystemTheme"),       true);
        map.insert(QStringLiteral("theme"),                   QStringLiteral("light"));
        map.insert(QStringLiteral("lightModeTheme"),          QStringLiteral("light"));
        map.insert(QStringLiteral("darkModeTheme"),           QStringLiteral("dark"));

        // ---- 图片 Image ----
        map.insert(QStringLiteral("imageInsertAction"),       QStringLiteral("path"));
        map.insert(QStringLiteral("imagePreferRelativeDirectory"), false);
        map.insert(QStringLiteral("imageRelativeDirectoryName"), QStringLiteral("assets"));

        return map;
    }();

    return kDefaults;
}

//=====================================================================
PreferencesStore::PreferencesStore(QObject *parent)
    : QObject(parent)
    , m_pSettings(nullptr)
{
    // 保证 config 目录存在（与工程内其它工具一致的惯例）
    const QString sPath = iniFilePath();
    QDir dir(QFileInfo(sPath).absolutePath());
    if (!dir.exists()) {
        dir.mkpath(QStringLiteral("."));
    }

    m_pSettings = new QSettings(sPath, QSettings::IniFormat, this);
    // Qt5：老式 QSettings 需显式指定 ini 编码，否则中文会写成转义序列
    m_pSettings->setIniCodec("UTF-8");

    // 首次运行：把 MarkText 的缺省值整份落盘，便于用户直接打开 ini 查看/修改
    if (!m_pSettings->childGroups().contains(QStringLiteral("preference"))) {
        const QHash<QString, QVariant> &mapDefaults = defaults();
        for (auto it = mapDefaults.constBegin(); it != mapDefaults.constEnd(); ++it) {
            m_pSettings->setValue(QStringLiteral("preference/") + it.key(), it.value());
        }
        m_pSettings->sync();
    }
}

PreferencesStore::~PreferencesStore()
{
    sync();
}

PreferencesStore *PreferencesStore::instance()
{
    static PreferencesStore *pInstance = nullptr;
    if (pInstance == nullptr) {
        pInstance = new PreferencesStore();
    }
    return pInstance;
}

QString PreferencesStore::iniFilePath()
{
    return QCoreApplication::applicationDirPath()
         + QStringLiteral("/config/QtMarkText.ini");
}

//=====================================================================
QVariant PreferencesStore::value(const QString &sKey) const
{
    const QVariant vDefault = defaultValue(sKey);
    if (!m_pSettings->contains(QStringLiteral("preference/") + sKey)) {
        return vDefault;
    }
    return coerceToDefaultType(
                m_pSettings->value(QStringLiteral("preference/") + sKey, vDefault), vDefault);
}

bool PreferencesStore::boolValue(const QString &sKey) const
{
    return value(sKey).toBool();
}

int PreferencesStore::intValue(const QString &sKey) const
{
    return value(sKey).toInt();
}

QString PreferencesStore::stringValue(const QString &sKey) const
{
    return value(sKey).toString();
}

void PreferencesStore::setValue(const QString &sKey, const QVariant &valueValue)
{
    if (value(sKey) == valueValue) {
        return;
    }
    m_pSettings->setValue(QStringLiteral("preference/") + sKey, valueValue);
    emit preferenceChanged(sKey, valueValue);
}

QVariant PreferencesStore::defaultValue(const QString &sKey) const
{
    return defaults().value(sKey);
}

QStringList PreferencesStore::keys() const
{
    QStringList listKeys = defaults().keys();
    listKeys.sort();
    return listKeys;
}

QVariantMap PreferencesStore::all() const
{
    QVariantMap map;
    const QStringList listKeys = keys();
    for (const QString &sKey : listKeys) {
        map.insert(sKey, value(sKey));
    }
    return map;
}

void PreferencesStore::sync()
{
    if (m_pSettings != nullptr) {
        m_pSettings->sync();
    }
}

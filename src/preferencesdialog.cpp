#include "preferencesdialog.h"

#include "marktextthemes.h"
#include "preferencesstore.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace {

// ===== 选项串（照抄 MarkText 各处 config.ts）=====

// 编码选项（36 项）：packages/desktop/src/common/encoding.ts -> ENCODING_NAME_MAP
const char *const kEncodingOptions =
    "utf8|UTF-8;utf16be|UTF-16 BE;utf16le|UTF-16 LE;utf32be|UTF-32 BE;"
    "utf32le|UTF-32 LE;ascii|Western (ISO 8859-1);latin3|Western (ISO 8859-3);iso885915|Western (ISO 8859-15);"
    "cp1252|Western (Windows 1252);arabic|Arabic (ISO 8859-6);cp1256|Arabic (Windows 1256);latin4|Baltic (ISO 8859-4);"
    "cp1257|Baltic (Windows 1257);iso88592|Central European (ISO 8859-2);windows1250|Central European (Windows 1250);cp866|Cyrillic (CP 866);"
    "iso88595|Cyrillic (ISO 8859-5);koi8r|Cyrillic (KOI8-R);koi8u|Cyrillic (KOI8-U);cp1251|Cyrillic (Windows 1251);"
    "iso885913|Estonian (ISO 8859-13);greek|Greek (ISO 8859-7);cp1253|Greek (Windows 1253);hebrew|Hebrew (ISO 8859-8);"
    "cp1255|Hebrew (Windows 1255);latin5|Turkish (ISO 8859-9);cp1254|Turkish (Windows 1254);gb2312|Simplified Chinese (GB2312);"
    "gb18030|Simplified Chinese (GB18030);gbk|Simplified Chinese (GBK);big5|Traditional Chinese (Big5);big5hkscs|Traditional Chinese (Big5-HKSCS);"
    "shiftjis|Japanese (Shift JIS);eucjp|Japanese (EUC-JP);euckr|Korean (EUC-KR);latin6|Nordic (ISO 8859-10)";

// 分类名：zh-CN.json -> preferences.categories
const char *const kCategoryNames[PreferencesDialog::PageCount] = {
    "通用",      // general
    "编辑器",    // editor
    "Markdown",  // markdown
    "拼写",      // spelling
    "主题",      // theme
    "图片",      // image
    "快捷键"     // keybindings
};

// 分类页脚说明（诚实交代"哪一项当场生效、哪一项等渲染层"）
const char *const kCategoryNotes[PreferencesDialog::PageCount] = {
    "键名与默认值均与 MarkText 一致，已落盘到 exe 同目录 config/QtMarkText.ini 的 [preference] 节。\n"
    "「语言 / 侧边栏 / 标签栏 / 启动状态」这类外壳直接管得到的项当场生效，其余项等渲染层接管后生效。",

    "字体、字号、行高由当前引擎直接兑现；编码 / 行尾 / 自动配对等项需要\n"
    "渲染层（WebView2）落地，占位引擎暂不处理。",

    "Markdown 语法偏好由渲染层直接消费（Muya 的 setOptions），外壳只负责保存与下发。",

    "拼写检查依赖系统词典与渲染层的 spellchecker，本轮仅做配置保存。",

    "主题的「记录」当场生效——会同步刷新顶部「主题(&T)」菜单的勾选态；\n"
    "真正的配色渲染由渲染层加载对应主题 CSS 完成。",

    "图片插入行为涉及渲染层的上传器 / 相对目录推导，本轮仅做配置保存。",

    "下表由菜单实际绑定的快捷键自动生成（与 MarkText 的 keybindingsWindows.ts 一致），\n"
    "当前只读；后续由渲染层的快捷键编辑器接管。"
};

} // namespace

//=====================================================================
//  偏好项清单（纯数据表）
//  键名：MarkText preference.json    文案：zh-CN.json -> preferences.items.*
//  选项：prefComponents 下各分类的 config.ts
//=====================================================================
const QList<PreferencesDialog::ItemSpec> &PreferencesDialog::itemSpecs()
{
    static const QList<ItemSpec> kSpecs = {
        // ---------------- 通用 ----------------
        { PageGeneral, KindCombo,    "language",             "语言",                       0, 0, 0,
          "zh-CN|简体中文;zh-TW|繁體中文;en|English;es|Español;fr|Français;de|Deutsch;ja|日本語;ko|한국어;pt|Português" },
        { PageGeneral, KindCheck,    "autoSave",             "自动保存",                   0, 0, 0, nullptr },
        { PageGeneral, KindSpin,     "autoSaveDelay",        "文件保存前的延迟时间（毫秒）", 1000, 60000, 0, nullptr },
        { PageGeneral, KindCheck,    "openFilesInNewWindow", "在新窗口中打开文件",           0, 0, 0, nullptr },
        { PageGeneral, KindCombo,    "startUpAction",        "启动文件/文件夹",             0, 0, 0,
          "restoreAll|恢复所有已打开的文件和文件夹;openLastFolder|恢复打开的文件夹;folder|打开默认目录;blank|打开空白页" },
        { PageGeneral, KindCheck,    "sideBarVisibility",    "侧边栏是否可见",             0, 0, 0, nullptr },
        { PageGeneral, KindCheck,    "tabBarVisibility",     "是否显示标签页",             0, 0, 0, nullptr },
        { PageGeneral, KindCheck,    "hideScrollbar",        "是否隐藏滚动条",             0, 0, 0, nullptr },
        { PageGeneral, KindDoubleSpin, "zoom",               "缩放级别（0.5 ~ 2.0）",      1, 4, 1, nullptr },

        // ---------------- 编辑器 ----------------
        { PageEditor,  KindLineEdit, "editorFontFamily",     "编辑器字体系列",             0, 0, 0, nullptr },
        { PageEditor,  KindSpin,     "fontSize",             "字体大小（像素）",           12, 72, 0, nullptr },
        { PageEditor,  KindDoubleSpin, "lineHeight",         "行高",                      1, 3, 1, nullptr },
        { PageEditor,  KindLineEdit, "editorLineWidth",      "编辑器区域最大宽度（空 / ch / px / %）", 0, 0, 0, nullptr },
        { PageEditor,  KindCombo,    "tabSize",              "用 x 个空格替换制表符",      0, 0, 0, "1|1;2|2;3|3;4|4" },
        { PageEditor,  KindCombo,    "endOfLine",            "行尾",                      0, 0, 0,
          "default|默认;crlf|CRLF (Windows);lf|LF (Unix/macOS)" },
        { PageEditor,  KindCombo,    "defaultEncoding",      "默认文件编码",               0, 0, 0, kEncodingOptions },
        { PageEditor,  KindCheck,    "autoGuessEncoding",    "打开文件时尝试自动猜测文件编码", 0, 0, 0, nullptr },
        { PageEditor,  KindCheck,    "autoNormalizeLineEndings", "打开时自动将行尾规范化为 LF", 0, 0, 0, nullptr },
        { PageEditor,  KindCheck,    "wrapCodeBlocks",       "在代码块内自动换行",         0, 0, 0, nullptr },
        { PageEditor,  KindCheck,    "codeBlockLineNumbers", "是否显示行号",               0, 0, 0, nullptr },
        { PageEditor,  KindCheck,    "autoPairBracket",      "编辑时自动配对括号",         0, 0, 0, nullptr },
        { PageEditor,  KindCheck,    "autoPairMarkdownSyntax", "自动完成 Markdown 语法",    0, 0, 0, nullptr },
        { PageEditor,  KindCheck,    "autoPairQuote",        "自动完成引号",               0, 0, 0, nullptr },
        { PageEditor,  KindCombo,    "textDirection",        "文本书写方向",               0, 0, 0, "ltr|从左到右;rtl|从右到左" },

        // ---------------- Markdown ----------------
        { PageMarkdown, KindCombo,   "preferHeadingStyle",   "首选标题样式",               0, 0, 0, "atx|ATX 样式;setext|Setext 样式" },
        { PageMarkdown, KindCombo,   "bulletListMarker",     "项目符号列表中使用的标记",    0, 0, 0, "*|*;-|-;+|+" },
        { PageMarkdown, KindCombo,   "orderListDelimiter",   "有序列表中使用的分隔符",      0, 0, 0, ".|.;)|)" },
        { PageMarkdown, KindCombo,   "listIndentation",      "列表的缩进",                 0, 0, 0,
          "dfm|DFM 样式;tab|制表符;1|一个空格;2|两个空格;3|三个空格;4|四个空格" },
        { PageMarkdown, KindCombo,   "frontmatterType",      "前置元数据类型",             0, 0, 0,
          "-|YAML;+|TOML;\\;|带分号的 JSON;{|带大括号的 JSON" },
        { PageMarkdown, KindCheck,   "isHtmlEnabled",        "启用 HTML 渲染",             0, 0, 0, nullptr },
        { PageMarkdown, KindCheck,   "isGitlabCompatibilityEnabled", "启用 GitLab 兼容模式", 0, 0, 0, nullptr },
        { PageMarkdown, KindCheck,   "superSubScript",       "启用上标和下标（Pandoc 扩展）", 0, 0, 0, nullptr },
        { PageMarkdown, KindCheck,   "footnote",             "启用脚注（Pandoc 扩展）",     0, 0, 0, nullptr },
        { PageMarkdown, KindCheck,   "preferLooseListItem",  "首选宽松列表项",             0, 0, 0, nullptr },
        { PageMarkdown, KindCombo,   "sequenceTheme",        "序列图主题",                 0, 0, 0, "hand|手绘;simple|简单" },

        // ---------------- 拼写 ----------------
        { PageSpelling, KindCheck,   "spellcheckerEnabled",   "启用拼写检查",              0, 0, 0, nullptr },
        { PageSpelling, KindCheck,   "spellcheckerNoUnderline", "不要为拼写错误添加下划线", 0, 0, 0, nullptr },
        { PageSpelling, KindCombo,   "spellcheckerLanguage",  "拼写检查器语言",            0, 0, 0,
          "en-US|English (United States);en-GB|English (United Kingdom);zh-CN|简体中文;zh-TW|繁體中文;"
          "ja|日本語;ko|한국어;de-DE|Deutsch;fr-FR|Français;es-ES|Español;pt-BR|Português (Brasil);ru-RU|Русский" },

        // ---------------- 主题 ----------------
        { PageTheme, KindCheck,      "followSystemTheme",    "跟随系统主题",               0, 0, 0, nullptr },
        { PageTheme, KindCombo,      "theme",                "当前主题",                   0, 0, 0, "<themes:all>" },
        { PageTheme, KindCombo,      "lightModeTheme",       "系统处于浅色模式时使用的主题", 0, 0, 0, "<themes:light>" },
        { PageTheme, KindCombo,      "darkModeTheme",        "系统处于深色模式时使用的主题", 0, 0, 0, "<themes:dark>" },

        // ---------------- 图片 ----------------
        { PageImage, KindCombo,      "imageInsertAction",    "粘贴或拖拽图片后的默认行为",   0, 0, 0,
          "upload|上传到云端;folder|复制到文件夹;path|使用绝对路径" },
        { PageImage, KindCheck,      "imagePreferRelativeDirectory", "首选相对图片目录",    0, 0, 0, nullptr },
        { PageImage, KindLineEdit,   "imageRelativeDirectoryName",   "相对图片文件夹名称",  0, 0, 0, nullptr }
    };
    return kSpecs;
}

//=====================================================================
//  构造
//=====================================================================
PreferencesDialog::PreferencesDialog(PreferencesStore *pStore, QWidget *parent)
    : QDialog(parent)
    , m_pStore((pStore != nullptr) ? pStore : PreferencesStore::instance())
    , m_pNav(nullptr)
    , m_pStack(nullptr)
    , m_pKeyTable(nullptr)
{
    setWindowTitle(QStringLiteral("偏好设置"));            // preferences.title
    resize(880, 620);

    m_pNav = new QListWidget(this);
    m_pNav->setObjectName(QStringLiteral("prefNav"));
    m_pNav->setFixedWidth(170);
    for (int i = 0; i < PageCount; ++i) {
        m_pNav->addItem(QString::fromUtf8(kCategoryNames[i]));
    }

    m_pStack = new QStackedWidget(this);
    for (int i = 0; i < PageCount; ++i) {
        m_pStack->addWidget((i == PageKeybindings) ? createKeybindingsPage() : createPage(i));
    }

    QHBoxLayout *pBody = new QHBoxLayout;
    pBody->addWidget(m_pNav);
    pBody->addWidget(m_pStack, 1);

    QDialogButtonBox *pButtons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(pButtons, &QDialogButtonBox::rejected, this, &QDialog::close);

    QVBoxLayout *pRoot = new QVBoxLayout(this);
    pRoot->addLayout(pBody, 1);
    pRoot->addWidget(pButtons);

    connect(m_pNav, &QListWidget::currentRowChanged, m_pStack, &QStackedWidget::setCurrentIndex);
    m_pNav->setCurrentRow(PageGeneral);

    // 偏好变化 -> 主题/语言通知（外壳据此同步菜单）
    connect(m_pStore, &PreferencesStore::preferenceChanged, this,
            [this](const QString &sKey, const QVariant &valueValue) {
        if (sKey == QLatin1String("followSystemTheme") || sKey == QLatin1String("theme")) {
            emit themeChanged(m_pStore->stringValue(QStringLiteral("theme")),
                              m_pStore->boolValue(QStringLiteral("followSystemTheme")));
        } else if (sKey == QLatin1String("language")) {
            emit languageChanged(valueValue.toString());
        }
    });
}

PreferencesDialog::~PreferencesDialog()
{
}

void PreferencesDialog::selectPage(int nPage)
{
    if (m_pNav != nullptr && nPage >= 0 && nPage < PageCount) {
        m_pNav->setCurrentRow(nPage);
    }
}

//=====================================================================
//  生成某一页
//=====================================================================
QWidget *PreferencesDialog::createPage(int nPage)
{
    QWidget *pContent = new QWidget;
    QVBoxLayout *pOuter = new QVBoxLayout(pContent);

    QLabel *pTitle = new QLabel(QString::fromUtf8(kCategoryNames[nPage]), pContent);
    QFont fontTitle = pTitle->font();
    fontTitle.setPointSize(fontTitle.pointSize() + 4);
    fontTitle.setBold(true);
    pTitle->setFont(fontTitle);
    pOuter->addWidget(pTitle);

    QFormLayout *pForm = new QFormLayout;
    pForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    pForm->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    pForm->setHorizontalSpacing(16);
    pForm->setVerticalSpacing(8);

    const QList<ItemSpec> &listSpecs = itemSpecs();
    int nItemCount = 0;
    for (const ItemSpec &spec : listSpecs) {
        if (spec.nPage == nPage) {
            addItem(pForm, spec);
            ++nItemCount;
        }
    }

    pOuter->addLayout(pForm);

    if (nItemCount == 0) {
        pOuter->addWidget(new QLabel(QStringLiteral("（本页暂无外壳可配置项）"), pContent));
    }

    addNote(pForm, QString::fromUtf8(kCategoryNotes[nPage]));
    pOuter->addStretch();

    QScrollArea *pScroll = new QScrollArea;
    pScroll->setWidgetResizable(true);
    pScroll->setFrameShape(QFrame::NoFrame);
    pScroll->setWidget(pContent);
    return pScroll;
}

void PreferencesDialog::addNote(QFormLayout *pForm, const QString &sText)
{
    if (sText.isEmpty()) {
        return;
    }
    QLabel *pNote = new QLabel(sText);
    pNote->setWordWrap(true);
    QPalette pal = pNote->palette();
    pal.setColor(QPalette::WindowText, pal.color(QPalette::WindowText).lighter(150));
    pNote->setPalette(pal);
    pForm->addRow(QString(), pNote);
}

void PreferencesDialog::addItem(QFormLayout *pForm, const ItemSpec &spec)
{
    const QString sKey = QString::fromLatin1(spec.pszKey);
    const QString sLabel = QString::fromUtf8(spec.pszLabel);

    switch (spec.kind) {
    case KindCheck: {
        QCheckBox *pBox = new QCheckBox;
        pBox->setChecked(m_pStore->boolValue(sKey));
        pForm->addRow(sLabel, pBox);
        m_hashControls.insert(sKey, pBox);
        connect(pBox, &QCheckBox::toggled, this, [this, sKey](bool bChecked) {
            onCheckToggled(sKey, bChecked);
        });
        break;
    }
    case KindCombo: {
        QComboBox *pCombo = new QComboBox;
        const QList<QPair<QVariant, QString> > listOptions = optionsOf(spec);
        for (const QPair<QVariant, QString> &pair : listOptions) {
            pCombo->addItem(pair.second, pair.first);
        }
        // 先定位当前值，再接信号，避免构造期误写
        const QVariant valueNow = m_pStore->value(sKey);
        for (int i = 0; i < pCombo->count(); ++i) {
            if (pCombo->itemData(i).toString() == valueNow.toString()) {
                pCombo->setCurrentIndex(i);
                break;
            }
        }
        pForm->addRow(sLabel, pCombo);
        m_hashControls.insert(sKey, pCombo);
        connect(pCombo, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
                this, [this, sKey, pCombo](int) {
            onComboActivated(sKey, pCombo->currentData());
        });
        break;
    }
    case KindSpin: {
        QSpinBox *pSpin = new QSpinBox;
        pSpin->setRange(spec.nMin, spec.nMax);
        pSpin->setValue(m_pStore->intValue(sKey));
        pForm->addRow(sLabel, pSpin);
        m_hashControls.insert(sKey, pSpin);
        connect(pSpin, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged),
                this, [this, sKey](int nValue) {
            onSpinChanged(sKey, nValue);
        });
        break;
    }
    case KindDoubleSpin: {
        QDoubleSpinBox *pSpin = new QDoubleSpinBox;
        pSpin->setRange(spec.nMin, spec.nMax);
        pSpin->setDecimals(spec.nDecimals);
        pSpin->setSingleStep(0.1);
        pSpin->setValue(m_pStore->value(sKey).toDouble());
        pForm->addRow(sLabel, pSpin);
        m_hashControls.insert(sKey, pSpin);
        connect(pSpin, static_cast<void (QDoubleSpinBox::*)(double)>(&QDoubleSpinBox::valueChanged),
                this, [this, sKey](double dValue) {
            onDoubleChanged(sKey, dValue);
        });
        break;
    }
    case KindLineEdit:
    default: {
        QLineEdit *pEdit = new QLineEdit;
        pEdit->setText(m_pStore->stringValue(sKey));
        pForm->addRow(sLabel, pEdit);
        m_hashControls.insert(sKey, pEdit);
        connect(pEdit, &QLineEdit::editingFinished, this, [this, sKey]() {
            onLineEdited(sKey);
        });
        break;
    }
    }
}

//=====================================================================
//  快捷键页（内容由外壳从菜单灌入，零重复）
//=====================================================================
QWidget *PreferencesDialog::createKeybindingsPage()
{
    QWidget *pContent = new QWidget;
    QVBoxLayout *pOuter = new QVBoxLayout(pContent);

    QLabel *pTitle = new QLabel(QString::fromUtf8(kCategoryNames[PageKeybindings]), pContent);
    QFont fontTitle = pTitle->font();
    fontTitle.setPointSize(fontTitle.pointSize() + 4);
    fontTitle.setBold(true);
    pTitle->setFont(fontTitle);
    pOuter->addWidget(pTitle);

    m_pKeyTable = new QTableWidget(0, 2, pContent);
    m_pKeyTable->setHorizontalHeaderLabels(QStringList() << QStringLiteral("菜单项") << QStringLiteral("快捷键"));
    m_pKeyTable->horizontalHeader()->setStretchLastSection(false);
    m_pKeyTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_pKeyTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_pKeyTable->verticalHeader()->setVisible(false);
    m_pKeyTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_pKeyTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    pOuter->addWidget(m_pKeyTable, 1);

    QLabel *pNote = new QLabel(QString::fromUtf8(kCategoryNotes[PageKeybindings]));
    pNote->setWordWrap(true);
    pOuter->addWidget(pNote);

    return pContent;
}

void PreferencesDialog::setKeybindings(const QList<QPair<QString, QString> > &listKeybindings)
{
    if (m_pKeyTable == nullptr) {
        return;
    }
    m_pKeyTable->setRowCount(listKeybindings.size());
    for (int i = 0; i < listKeybindings.size(); ++i) {
        m_pKeyTable->setItem(i, 0, new QTableWidgetItem(listKeybindings.at(i).first));
        m_pKeyTable->setItem(i, 1, new QTableWidgetItem(listKeybindings.at(i).second));
    }
}

//=====================================================================
//  选项解析
//  "<themes:light|dark|all>" 或 "值|显示;值|显示"
//  以 "\;" 表示值里真的含分号（如 frontmatterType 的 ";" 选项）
//=====================================================================
QList<QPair<QVariant, QString> > PreferencesDialog::optionsOf(const ItemSpec &spec)
{
    QList<QPair<QVariant, QString> > listOptions;
    const QString sOptions = QString::fromUtf8(spec.pszOptions != nullptr ? spec.pszOptions : "");

    // 主题：直接从共享主题表取，避免第二份清单
    if (sOptions.startsWith(QLatin1String("<themes:"))) {
        const QString sScope = sOptions.mid(8, sOptions.length() - 9);
        const bool bLight = (sScope != QLatin1String("dark"));
        const bool bDark = (sScope != QLatin1String("light"));
        if (bLight) {
            for (int i = 0; i < MarkTextThemes::kLightThemeCount; ++i) {
                listOptions.append(qMakePair(QVariant(QString::fromLatin1(MarkTextThemes::kLightThemes[i].pszId)),
                                             QString::fromUtf8(MarkTextThemes::kLightThemes[i].pszText)));
            }
        }
        if (bDark) {
            for (int i = 0; i < MarkTextThemes::kDarkThemeCount; ++i) {
                listOptions.append(qMakePair(QVariant(QString::fromLatin1(MarkTextThemes::kDarkThemes[i].pszId)),
                                             QString::fromUtf8(MarkTextThemes::kDarkThemes[i].pszText)));
            }
        }
        return listOptions;
    }

    // 普通选项：按 ';' 分段，'\;' 是转义
    QString sToken;
    QStringList listTokens;
    for (int i = 0; i < sOptions.length(); ++i) {
        const QChar ch = sOptions.at(i);
        if (ch == QLatin1Char('\\') && (i + 1) < sOptions.length()
            && sOptions.at(i + 1) == QLatin1Char(';')) {
            sToken.append(QLatin1Char(';'));
            ++i;
        } else if (ch == QLatin1Char(';')) {
            listTokens.append(sToken);
            sToken.clear();
        } else {
            sToken.append(ch);
        }
    }
    listTokens.append(sToken);

    for (const QString &sOne : listTokens) {
        if (sOne.isEmpty()) {
            continue;
        }
        const int nPos = sOne.indexOf(QLatin1Char('|'));
        const QString sValue = (nPos >= 0) ? sOne.left(nPos) : sOne;
        const QString sText = (nPos >= 0) ? sOne.mid(nPos + 1) : sOne;
        // 纯数字的值按整数存（对照 MarkText：listIndentation 的 1~4 是 number，'dfm'/'tab' 是 string）
        const bool bNumeric = !sValue.isEmpty() && sValue.at(0).isDigit()
                              && (sValue.toInt() > 0 || sValue == QLatin1String("0"));
        listOptions.append(qMakePair(bNumeric ? QVariant(sValue.toInt()) : QVariant(sValue), sText));
    }
    return listOptions;
}

//=====================================================================
//  写回
//=====================================================================
void PreferencesDialog::onCheckToggled(const QString &sKey, bool bChecked)
{
    m_pStore->setValue(sKey, bChecked);
}

void PreferencesDialog::onComboActivated(const QString &sKey, const QVariant &valueValue)
{
    m_pStore->setValue(sKey, valueValue);
}

void PreferencesDialog::onSpinChanged(const QString &sKey, int nValue)
{
    m_pStore->setValue(sKey, nValue);
}

void PreferencesDialog::onDoubleChanged(const QString &sKey, double dValue)
{
    m_pStore->setValue(sKey, dValue);
}

void PreferencesDialog::onLineEdited(const QString &sKey)
{
    QLineEdit *pEdit = qobject_cast<QLineEdit *>(m_hashControls.value(sKey, nullptr));
    if (pEdit != nullptr) {
        m_pStore->setValue(sKey, pEdit->text());
    }
}

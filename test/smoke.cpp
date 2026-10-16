//=====================================================================
//  smoke.cpp —— 真实平台下的启动冒烟测试（不参与正式构建）
//  逐段落盘，用于定位"程序起来后立刻退出"的断点位置。
//  输出：smoke.txt（当前工作目录）
//=====================================================================

#include <QApplication>
#include <QFile>
#include <QTextStream>
#include <QTimer>

#include "editorhost.h"
#include "mainwindow.h"
#include "preferencesstore.h"

static QTextStream *g_pTs = nullptr;

static void log(const QString &sMessage)
{
    if (g_pTs != nullptr) {
        *g_pTs << sMessage << "\n";
        g_pTs->flush();
    }
}

int main(int argc, char **argv)
{
    QFile file(QStringLiteral("smoke.txt"));
    file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text);
    QTextStream ts(&file);
    ts.setCodec("UTF-8");
    g_pTs = &ts;

    log(QStringLiteral("[1] 进入 main"));

    QApplication app(argc, argv);
    log(QStringLiteral("[2] QApplication 构造完成"));

    log(QStringLiteral("[2.1] 平台插件 = %1").arg(QGuiApplication::platformName()));
    log(QStringLiteral("[2.2] Qt 版本 = %1").arg(QLatin1String(qVersion())));

    log(QStringLiteral("[3] ini 路径 = %1").arg(PreferencesStore::iniFilePath()));
    log(QStringLiteral("[3.1] WebView2 运行时 = %1")
            .arg(EditorHost::isWebView2RuntimeInstalled() ? QStringLiteral("已装")
                                                         : QStringLiteral("未装")));
    log(QStringLiteral("[3.2] 选中的引擎 = %1")
            .arg(EditorHost::engineTitle(EditorHost::requestedEngine())));
    log(QStringLiteral("[3.3] autoSave = %1, theme = %2, followSystemTheme = %3")
            .arg(PreferencesStore::instance()->value(QStringLiteral("autoSave")).toString(),
                 PreferencesStore::instance()->value(QStringLiteral("theme")).toString(),
                 PreferencesStore::instance()->value(QStringLiteral("followSystemTheme")).toString()));

    MainWindow window;
    log(QStringLiteral("[4] MainWindow 构造完成"));

    window.show();
    log(QStringLiteral("[5] show() 完成，窗口标题 = %1").arg(window.windowTitle()));

    QTimer::singleShot(2000, &app, &QCoreApplication::quit);
    const int nRc = app.exec();
    log(QStringLiteral("[6] exec() 返回 %1").arg(nRc));

    file.close();
    return 0;
}

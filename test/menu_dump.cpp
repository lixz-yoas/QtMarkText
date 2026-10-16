//=====================================================================
//  menu_dump.cpp —— 无 GUI 核对程序（不参与正式构建，仅用于验收）
//
//  目的：把 MainWindow 实际生成的菜单树、勾选态、快捷键，以及偏好设置
//        窗口的分类页，全部导出到文本文件，用于人工核对是否与 MarkText 一致。
//
//  用法（在影子构建目录里）：
//      g++ <CXXFLAGS> -o menu_dump.o menu_dump.cpp
//      sed 's|main\.o|menu_dump.o|' object_script.QtMarkText > object_script.menuDump
//      g++ <LFLAGS> -o menu_dump.exe @object_script.menuDump <LIBS>
//      QT_QPA_PLATFORM=offscreen ./menu_dump.exe
//
//  输出：menu_dump.txt（放在当前工作目录）
//=====================================================================

#include <QAction>
#include <QApplication>
#include <QDialog>
#include <QFile>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QStackedWidget>
#include <QTextStream>

#include "mainwindow.h"
#include "preferencesdialog.h"

static void dumpMenu(QTextStream &ts, QMenu *pMenu, int nDepth)
{
    const QString sIndent(nDepth * 2, QLatin1Char(' '));
    const QList<QAction *> listActions = pMenu->actions();
    for (QAction *pAction : listActions) {
        if (pAction->isSeparator()) {
            ts << sIndent << "---\n";
            continue;
        }

        QString sLine = sIndent + pAction->text();
        if (pAction->menu() != nullptr) {
            sLine += QStringLiteral("  [子菜单]");
        }
        if (pAction->isCheckable()) {
            sLine += pAction->isChecked() ? QStringLiteral("  [√]") : QStringLiteral("  [ ]");
        }
        if (!pAction->isEnabled()) {
            sLine += QStringLiteral("  (禁用)");
        }
        const QString sShortcut = pAction->shortcut().toString(QKeySequence::NativeText);
        if (!sShortcut.isEmpty()) {
            sLine += QStringLiteral("  <") + sShortcut + QStringLiteral(">");
        }
        if (!pAction->objectName().isEmpty()) {
            sLine += QStringLiteral("   #") + pAction->objectName();
        }
        ts << sLine << "\n";

        if (pAction->menu() != nullptr) {
            dumpMenu(ts, pAction->menu(), nDepth + 1);
        }
    }
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);

    QFile file(QStringLiteral("menu_dump.txt"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return 1;
    }
    QTextStream ts(&file);
    ts.setCodec("UTF-8");

    MainWindow window;
    window.show();

    ts << QStringLiteral("==================== 菜单栏（实际生成） ====================\n");
    QMenuBar *pBar = window.menuBar();
    const QList<QAction *> listTop = pBar->actions();
    for (QAction *pTop : listTop) {
        ts << QStringLiteral("\n■■ ") << pTop->text() << QStringLiteral("\n");
        if (pTop->menu() != nullptr) {
            dumpMenu(ts, pTop->menu(), 1);
        }
    }

    // 顶层菜单个数
    ts << QStringLiteral("\n==================== 汇总 ====================\n");
    ts << QStringLiteral("顶层菜单数：") << listTop.size() << QStringLiteral("\n");

    int nTotal = 0;
    for (QAction *pTop : listTop) {
        if (pTop->menu() != nullptr) {
            nTotal += pTop->menu()->actions().size();
        }
    }
    ts << QStringLiteral("顶层菜单的直接子项总数（含分隔符）：") << nTotal << QStringLiteral("\n");

    // 偏好设置窗口的分类页
    QDialog *pDialog = nullptr;
    const QList<QDialog *> listDialogs = window.findChildren<QDialog *>();
    for (QDialog *pEach : listDialogs) {
        if (pEach->objectName() != QLatin1String("qt_filedialog")) {
            pDialog = pEach;
        }
    }

    // 偏好设置窗口是懒加载的，直接构造一个来核对分类
    ts << QStringLiteral("\n==================== 偏好设置（独立构造） ====================\n");
    {
        PreferencesDialog prefDialog(nullptr);
        const QList<QListWidget *> listNavs = prefDialog.findChildren<QListWidget *>();
        for (QListWidget *pNav : listNavs) {
            if (pNav->objectName() != QLatin1String("prefNav")) {
                continue;
            }
            for (int i = 0; i < pNav->count(); ++i) {
                ts << QStringLiteral("  [") << i << QStringLiteral("] ") << pNav->item(i)->text() << QStringLiteral("\n");
            }
        }
        ts << QStringLiteral("分类页数：") << prefDialog.findChildren<QStackedWidget *>().size() << QStringLiteral("\n");
    }
    Q_UNUSED(pDialog)

    ts.flush();
    file.close();
    return 0;
}

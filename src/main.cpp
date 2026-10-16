#include <QApplication>
#include <QFileInfo>
#include <QString>
#include <QTextCodec>
#include <QTimer>

#include "mainwindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // 源码为 UTF-8，统一本地 8 位编码的解释方式，避免中文乱码
    QTextCodec::setCodecForLocale(QTextCodec::codecForName("UTF-8"));

    app.setApplicationName(QStringLiteral("QtMarkText"));
    app.setApplicationVersion(QStringLiteral("0.1.0"));
    app.setOrganizationName(QStringLiteral("QtMarkText"));

    MainWindow w;
    w.show();

    // 命令行文件参数（对齐 MarkText：marktext.exe xxx.md）
    // 窗口显示后再打开，此时 WebView2 已随首屏 Show 开始挂载
    for (int i = 1; i < argc; ++i) {
        const QString sPath = QString::fromLocal8Bit(argv[i]);
        if (QFileInfo(sPath).isFile()) {
            QTimer::singleShot(0, &w, [&w, sPath]() { w.openFile(sPath); });
            break;   // 只取第一个文件参数
        }
    }

    return app.exec();
}

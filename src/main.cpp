#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QMutex>
#include <QStringList>
#include <QTextCodec>
#include <QTextStream>
#include <QTimer>

#include "mainwindow.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

//=====================================================================
//  运行日志落盘
// ---------------------------------------------------------------------
//  Qt 的 qInfo/qWarning 在 Windows GUI 子系统下没有控制台可写（只在调试器
//  的 OutputDebugString 里可见）。为了让"渲染层垫片握手 / 导航结果 / 未实现
//  通道"这些诊断信息在普通运行下也能查，这里挂一个消息处理器，把全部 Qt
//  日志同时追加到 <exe目录>/config/QtMarkText.log。
//
//  规则：每次启动截断重写（只保留本次运行），避免文件无限增长。
//=====================================================================
namespace {

QFile *g_pLogFile = nullptr;
QMutex g_logMutex;
QtMessageHandler g_pfnPrevious = nullptr;

const char *levelName(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:    return "DEBUG";
    case QtInfoMsg:     return "INFO ";
    case QtWarningMsg:  return "WARN ";
    case QtCriticalMsg: return "CRIT ";
    case QtFatalMsg:    return "FATAL";
    }
    return "?????";
}

void fileMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    const QString sLine = QStringLiteral("[%1] %2 %3\n")
            .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
                 QLatin1String(levelName(type)),
                 msg);

    QMutexLocker locker(&g_logMutex);
    if (g_pLogFile != nullptr && g_pLogFile->isOpen()) {
        g_pLogFile->write(sLine.toUtf8());
        g_pLogFile->flush();   // 崩溃时也能保住最后几行
    }
    if (g_pfnPrevious != nullptr) {
        g_pfnPrevious(type, context, msg);
    }
}

/// 在 exe 同级 config 目录下开日志；失败则静默跳过（不影响功能）
void installFileLogger()
{
    const QString sDir = QCoreApplication::applicationDirPath() + QStringLiteral("/config");
    QDir().mkpath(sDir);

    QFile *pFile = new QFile(sDir + QStringLiteral("/QtMarkText.log"));
    // 追加模式：第二实例把文件转交给第一实例后退出，不能把第一实例的日志冲掉。
    // 超过 2 MB 时清空重来，避免无限增长。
    QIODevice::OpenMode mode = QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text;
    if (pFile->size() > 2 * 1024 * 1024) {
        pFile->remove();
    }
    if (!pFile->open(mode)) {
        delete pFile;
        return;
    }
    g_pLogFile = pFile;
    g_pfnPrevious = qInstallMessageHandler(fileMessageHandler);

    qInfo("========== QtMarkText 启动 ==========");
    qInfo("exe: %s", qPrintable(QCoreApplication::applicationFilePath()));
    qInfo("cwd: %s", qPrintable(QDir::currentPath()));
    qInfo("日志: %s", qPrintable(pFile->fileName()));
}

//=====================================================================
//  单实例保护
// ---------------------------------------------------------------------
//  MarkText（Electron）有 requestSingleInstanceLock：第二次启动时把文件转交
//  给已运行的实例后自行退出。本工程必须同样这么做，原因比"体验"更硬：
//
//    WebView2 的浏览器进程与 user data 目录是一对一关系。两个 QtMarkText
//    实例共用 bin/config/webview2 时，后创建的实例拿不到（或争抢）同一个
//    浏览器进程，controller 创建失败——表现就是**编辑区一片黑、什么都没有**。
//
//  通道用 QLocalServer（Windows 命名管道），不产生任何 TCP/socket 监听，
//  因此不会触发 Windows 防火墙的"允许访问网络"弹窗。
//=====================================================================
const char kInstanceKey[] = "QtMarkText.SingleInstance.v1";

bool forwardToRunningInstance(const QStringList &listFiles)
{
    QLocalSocket socket;
    socket.connectToServer(QLatin1String(kInstanceKey));
    if (!socket.waitForConnected(500)) {
        return false;   // 没有已运行的实例
    }

    // 协议：一行 JSON 字符串数组（待打开的文件）
    const QByteArray baPayload = QJsonDocument(
                                     QJsonArray::fromStringList(listFiles))
                                     .toJson(QJsonDocument::Compact);
    socket.write(baPayload);
    socket.waitForBytesWritten(1000);
    socket.disconnectFromServer();
    return true;
}

/// 在已运行实例里打开来件并前置窗口（主实例侧接收逻辑）
void acceptFromLaterInstances(QCoreApplication &app, MainWindow &w)
{
    Q_UNUSED(app)
    QLocalServer::removeServer(QLatin1String(kInstanceKey));
    QLocalServer *pServer = new QLocalServer(&app);
    if (!pServer->listen(QLatin1String(kInstanceKey))) {
        qWarning("单实例监听失败: %s", qPrintable(pServer->errorString()));
    } else {
        QObject::connect(pServer, &QLocalServer::newConnection, [pServer, &w]() {
            QLocalSocket *pConn = pServer->nextPendingConnection();
            if (pConn == nullptr) {
                return;
            }
            QObject::connect(pConn, &QLocalSocket::disconnected, pConn,
                             &QLocalSocket::deleteLater);
            // 数据很短，等写完再取
            if (pConn->waitForReadyRead(1000)) {
                const QJsonDocument doc = QJsonDocument::fromJson(pConn->readAll());
                if (doc.isArray()) {
                    const QJsonArray arrFiles = doc.array();
                    for (int i = 0; i < arrFiles.size(); ++i) {
                        const QString sFile = arrFiles.at(i).toString();
                        if (sFile.isEmpty()) {
                            continue;
                        }
                        qInfo("单实例转交文件: %s", qPrintable(sFile));
                        w.openFile(sFile);
                    }
                }
            }
            // 第二实例通常连到的是已最大化/被遮挡的窗口，这里前置一次
            w.setWindowState((w.windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
            w.raise();
            w.activateWindow();
        });
    }
}

} // namespace

int main(int argc, char *argv[])
{
    int nExitCode = 0;

    // QApplication 也放进作用域：退出诊断要能区分「事件循环返回」/「app 析构」/
    // 「CRT 退出」三段各花了多久（见 readme 退出分析）。
    {
        QApplication app(argc, argv);

        // 源码为 UTF-8，统一本地 8 位编码的解释方式，避免中文乱码
        QTextCodec::setCodecForLocale(QTextCodec::codecForName("UTF-8"));

        app.setApplicationName(QStringLiteral("QtMarkText"));
        app.setApplicationVersion(QStringLiteral("0.1.0"));
        app.setOrganizationName(QStringLiteral("QtMarkText"));

        installFileLogger();

        // 命令行文件参数（对齐 MarkText：marktext.exe xxx.md）
        QStringList listFiles;
        for (int i = 1; i < argc; ++i) {
            const QString sPath = QString::fromLocal8Bit(argv[i]);
            if (QFileInfo(sPath).isFile()) {
                listFiles.append(QDir::fromNativeSeparators(sPath));
            }
        }

        // 已有实例在跑：转交文件后退出（对齐 MarkText requestSingleInstanceLock）
        if (forwardToRunningInstance(listFiles)) {
            qInfo("检测到已运行的 QtMarkText，转交 %d 个文件后退出", listFiles.size());
            qInfo("[退出] 转交模式：即将析构 QApplication");
        } else {
            MainWindow w;
            w.show();

            // 文件在窗口显示后再打开，此时 WebView2 已随首屏 Show 开始挂载
            for (const QString &sPath : listFiles) {
                QTimer::singleShot(0, &w, [&w, sPath]() { w.openFile(sPath); });
            }

            acceptFromLaterInstances(app, w);

            nExitCode = app.exec();
            qInfo("[退出] app.exec() 已返回 rc=%d，即将析构 MainWindow / QApplication",
                  nExitCode);
        }
    }

    // ---- 退出收尾 ----
    // 到这里 Qt 侧（窗口、引擎、WebView2、偏好与最近文件落盘）已全部清理完毕。
    // 但本机环境下退出会卡在 DLL 卸载（DllMain/PROCESS_DETACH）阶段：
    // 一个只有空窗口的最小 Qt 程序同样如此，与 WebView2、菜单改造无关；
    // 连 ExitProcess 都会被挂住十几秒到数十秒（见 readme「退出分析」）。
    // 用户观感就是"关了窗口进程还在"。所有需要持久化的状态都已在运行期
    // 即时落盘（closeEvent 里又补了偏好/最近文件），故直接结束进程。
    qInfo("[退出] 强制结束进程，跳过挂起的 DLL 卸载阶段");
    ::TerminateProcess(::GetCurrentProcess(), static_cast<UINT>(nExitCode));
    return nExitCode;   // 不会到达
}

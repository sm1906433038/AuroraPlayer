// AuroraPlayer (晨曦影音) — entry point.
//
// We set a few QApplication attributes that matter for video playback:
//   * AA_UseDesktopOpenGL: force the desktop GL path (libmpv's OpenGL render
//     API does not work with ANGLE on Windows).
//   * AA_ShareOpenGLContexts: required by QOpenGLWidget when running with
//     multiple top-level windows.

#include <QApplication>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QIcon>
#include <QLibraryInfo>
#include <QLocale>
#include <QMutex>
#include <QStringList>
#include <QSurfaceFormat>
#include <QStyleFactory>
#include <QTextStream>
#include <QTranslator>

#include "ui/MainWindow.h"

namespace {
QFile         g_logFile;
QTextStream   g_logStream;
QMutex        g_logMutex;

void promp_msg_handler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
    QMutexLocker lk(&g_logMutex);
    if (!g_logFile.isOpen()) return;
    const char* lvl = "INFO ";
    switch (type) {
        case QtDebugMsg:    lvl = "DEBUG"; break;
        case QtInfoMsg:     lvl = "INFO "; break;
        case QtWarningMsg:  lvl = "WARN "; break;
        case QtCriticalMsg: lvl = "CRIT "; break;
        case QtFatalMsg:    lvl = "FATAL"; break;
    }
    g_logStream << QDateTime::currentDateTime().toString("HH:mm:ss.zzz")
                << ' ' << lvl << ' ' << msg;
    if (ctx.file && *ctx.file)
        g_logStream << "    (" << ctx.file << ':' << ctx.line << ')';
    g_logStream << '\n';
    g_logStream.flush();
}
} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication::setOrganizationName("AuroraPlayer");
    QCoreApplication::setApplicationName("AuroraPlayer");
    QCoreApplication::setApplicationVersion("0.1.0");
    QGuiApplication::setApplicationDisplayName(QStringLiteral("晨曦影音"));

    // Install Qt log → file handler. Console is unavailable for WIN32 apps.
    g_logFile.setFileName(QDir::temp().filePath("AuroraPlayer-qt.log"));
    if (g_logFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        g_logStream.setDevice(&g_logFile);
        qInstallMessageHandler(promp_msg_handler);
        qInfo("---- AuroraPlayer (晨曦影音) starting ----");
    }

    QCoreApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    // OpenGL 3.3 Core: matches libmpv embed + QOpenGLWidget reliably on Windows.
    // Requesting GL 4.5 + mpv's "high-quality"/compute/HDR-peak stack can hit
    // INVALID_ENUM in texture paths after fullscreen resizes on some drivers.
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    fmt.setSwapInterval(1);
    fmt.setSamples(0);
    QSurfaceFormat::setDefaultFormat(fmt);

    QApplication app(argc, argv);
    app.setStyle(QStyleFactory::create("Fusion"));

    // Application icon — used in taskbar, alt-tab, window decoration,
    // Win+Tab, and (via Qt) the title-bar of every top-level window.
    app.setWindowIcon(QIcon(QStringLiteral(":/icons/app.png")));

    // ---- Load Qt's built-in zh_CN translations -----------------------------
    // This Sinicises Qt's standard widgets: QFileDialog buttons, QMessageBox
    // OK/Cancel, QShortcut display names, etc. Our app-level strings are
    // already authored directly in Chinese.
    {
        const QString trPath = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
        for (const QString& base : QStringList{ "qtbase_zh_CN", "qt_zh_CN" }) {
            auto* tr = new QTranslator(&app);
            if (tr->load(base, trPath) || tr->load(base, QStringLiteral(":/i18n"))) {
                app.installTranslator(tr);
            } else {
                delete tr;
            }
        }
    }

    QCommandLineParser cli;
    cli.setApplicationDescription("AuroraPlayer (晨曦影音) — 影音播放器");
    cli.addHelpOption();
    cli.addVersionOption();
    cli.addPositionalArgument("files", QStringLiteral("要播放的文件或网址"), "[files...]");
    cli.process(app);

    promp::MainWindow w;
    w.show();

    const auto args = cli.positionalArguments();
    if (!args.isEmpty()) w.openPath(args.first());

    return app.exec();
}

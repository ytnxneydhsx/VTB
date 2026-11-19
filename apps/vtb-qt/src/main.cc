#include "main_window.hh"

#include "theme.hh"

#include <QApplication>
#include <QCommandLineParser>

int main(int argc, char** argv) {
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("VTB 上位机"));
    QCommandLineParser args;
    args.setApplicationDescription(QStringLiteral("RK3588 Live2D Qt 控制台"));
    args.addHelpOption();
    const QCommandLineOption demo(QStringLiteral("demo"), QStringLiteral("本地演示，不连接板卡"));
    args.addOption(demo);
    args.process(application);

    vtb::desktop::applyTheme(application, vtb::desktop::ColorMode::LIGHT);
    vtb::desktop::MainWindow window(args.isSet(demo));
    window.show();
    return application.exec();
}

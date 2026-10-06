#include "mainwindow/MainWindow.h"
#include <QApplication>
#include <QSurfaceFormat>
#include <QStyleFactory>

int main(int argc, char* argv[]) {
    // Request OpenGL 3.3 Core for the waveform renderer
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setSamples(4);  // MSAA x4
    QSurfaceFormat::setDefaultFormat(fmt);

    QApplication app(argc, argv);
    app.setApplicationName("EmbeddedScope");
    app.setApplicationVersion("0.1.0");
    app.setOrganizationName("EmbeddedScope Project");

    // Use Fusion style for a consistent dark-theme-friendly look
    app.setStyle(QStyleFactory::create("Fusion"));

    // Dark palette
    QPalette dark;
    dark.setColor(QPalette::Window,          QColor(30, 30, 30));
    dark.setColor(QPalette::WindowText,      Qt::white);
    dark.setColor(QPalette::Base,            QColor(20, 20, 20));
    dark.setColor(QPalette::AlternateBase,   QColor(45, 45, 45));
    dark.setColor(QPalette::ToolTipBase,     QColor(45, 45, 50));
    dark.setColor(QPalette::ToolTipText,     Qt::white);
    dark.setColor(QPalette::Text,            Qt::white);
    dark.setColor(QPalette::Button,          QColor(53, 53, 53));
    dark.setColor(QPalette::ButtonText,      Qt::white);
    dark.setColor(QPalette::BrightText,      Qt::red);
    dark.setColor(QPalette::Link,            QColor(42, 130, 218));
    dark.setColor(QPalette::Highlight,       QColor(42, 130, 218));
    dark.setColor(QPalette::HighlightedText, Qt::black);
    app.setPalette(dark);
    app.setStyleSheet("QToolTip { color:#eee; background:#2d2d32; border:1px solid #666; padding:3px; }");

    MainWindow w;
    w.show();
    return app.exec();
}

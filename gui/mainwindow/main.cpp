#include "mainwindow/MainWindow.h"
#include "theme/Theme.h"
#include <QApplication>
#include <QDir>
#include <QFontDatabase>
#include <QSurfaceFormat>
#include <QStyleFactory>

namespace {

QString stylesheet() {
    using namespace theme;
    const QString text    = css(kText),    muted = css(kTextMuted), faint = css(kTextFaint);
    const QString window  = css(kWindow),  surf  = css(kSurface),   surfHi = css(kSurfaceHi);
    const QString sep     = css(kSeparator), accent = css(kAccent), canvas = css(kCanvas);
    const QString green   = css(kGreen),   red   = css(kRed);

    QString s = QString(R"(
        * { outline: 0; }
        QToolTip { color:%1; background:%4; border:1px solid %6; border-radius:6px; padding:5px 8px; }

        QMenuBar { background:%3; padding:2px 6px; border-bottom:1px solid %6; }
        QMenuBar::item { padding:5px 11px; border-radius:6px; background:transparent; color:%2; }
        QMenuBar::item:selected { background:%5; }
        QMenu { background:%4; border:1px solid %6; border-radius:10px; padding:6px; }
        QMenu::item { padding:6px 22px 6px 14px; border-radius:6px; color:%1; }
        QMenu::item:selected { background:%7; color:white; }
        QMenu::item:disabled { color:%8; }
        QMenu::separator { height:1px; background:%6; margin:5px 8px; }
        QMenu::indicator { width:14px; height:14px; margin-left:4px; }

        QToolBar { background:%3; border:0; border-bottom:1px solid %6; padding:7px 10px; spacing:6px; }
        QToolBar::separator { width:1px; background:%6; margin:5px 8px; }
        QToolBar QLabel { color:%2; padding:0 2px 0 4px; }
        QToolButton { color:%1; padding:6px 13px; border:0; border-radius:8px; background:transparent; }
        QToolButton:hover { background:%5; }
        QToolButton:pressed { background:%4; }
        QToolButton:checked { background:%7; color:white; }
        QToolButton:disabled { color:%8; }
        QToolButton#runButton { background:%9; color:#07210f; font-weight:600; padding:6px 18px; }
        QToolButton#runButton:hover { background:#4ae077; }
        QToolButton#runButton:disabled { background:%4; color:%8; }
        QToolButton#stopButton { background:%10; color:white; font-weight:600; padding:6px 18px; }
        QToolButton#stopButton:hover { background:#ff6a60; }
        QToolButton#stopButton:disabled { background:%4; color:%8; }

        QPushButton { color:%1; background:%4; border:0; border-radius:8px; padding:6px 14px; }
        QPushButton:hover { background:%5; }
        QPushButton:pressed { background:%3; }
        QPushButton:disabled { color:%8; }

        QComboBox { color:%1; background:%4; border:0; border-radius:8px; padding:6px 10px; min-width:76px; }
        QComboBox:hover { background:%5; }
        QComboBox::drop-down { border:0; width:22px; }
        QComboBox QAbstractItemView { background:%4; border:1px solid %6; border-radius:8px; padding:4px;
                                      selection-background-color:%7; selection-color:white; outline:0; }
        QLineEdit { color:%1; background:%4; border:1px solid %6; border-radius:8px; padding:6px 10px;
                    selection-background-color:%7; }
        QLineEdit:focus { border:1px solid %7; }

        QPlainTextEdit { color:%1; background:%11; border:1px solid %6; border-radius:10px; padding:8px;
                         selection-background-color:%7; }

        QDockWidget { color:%2; }
        QDockWidget > QWidget { background:%3; }
        QMainWindow::separator { background:%6; width:1px; height:1px; }

        QStatusBar { background:%3; color:%2; border-top:1px solid %6; }
        QStatusBar::item { border:0; }
        QStatusBar QLabel { color:%2; padding:2px 8px; }

        QScrollBar:vertical { background:transparent; width:10px; margin:2px; }
        QScrollBar::handle:vertical { background:%5; border-radius:3px; min-height:28px; }
        QScrollBar::handle:vertical:hover { background:%12; }
        QScrollBar:horizontal { background:transparent; height:10px; margin:2px; }
        QScrollBar::handle:horizontal { background:%5; border-radius:3px; min-width:28px; }
        QScrollBar::handle:horizontal:hover { background:%12; }
        QScrollBar::add-line, QScrollBar::sub-line { width:0; height:0; }
        QScrollBar::add-page, QScrollBar::sub-page { background:transparent; }

        QDialog, QMessageBox { background:%3; }
        QLabel#sectionTitle { color:%1; font-weight:600; font-size:11pt; }
        QLabel#caption { color:%2; }
        QLabel#keycap { color:%1; background:%5; border:1px solid %6; border-radius:6px; padding:3px 9px; }
        QLabel#hint { color:%13; font-size:9pt; }
    )");
    // %1..%13 are palette slots; replace from the highest so %1 never eats the start of %10
    const QString vals[13] = {text, muted, window, surf, surfHi, sep, accent, faint,
                              green, red, canvas, css(kTextFaint), css(kTextFaint)};
    for (int i = 13; i >= 1; --i) s.replace("%" + QString::number(i), vals[i - 1]);
    return s;
}

} // namespace

int main(int argc, char* argv[]) {
    // OpenGL 3.3 core with 4x MSAA for the waveform renderer
    QSurfaceFormat fmt;
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setSamples(4);
    QSurfaceFormat::setDefaultFormat(fmt);

    QApplication app(argc, argv);
    app.setApplicationName("EmbeddedScope");
    app.setApplicationVersion("0.1.0");
    app.setOrganizationName("EmbeddedScope");
    // Inter and JetBrains Mono (both SIL Open Font License) ship inside the executable
    for (const QString& f : QDir(":/fonts/fonts").entryList(QDir::Files))
        QFontDatabase::addApplicationFont(":/fonts/fonts/" + f);
    // Any font files placed in a "fonts" folder next to the executable (for example Inter) are
    // picked up automatically and used ahead of the system fallbacks.
    const QDir fonts(QCoreApplication::applicationDirPath() + "/fonts");
    for (const QString& f : fonts.entryList({"*.ttf", "*.otf"}, QDir::Files))
        QFontDatabase::addApplicationFont(fonts.absoluteFilePath(f));
    app.setFont(theme::ui(10.0));

    app.setStyle(QStyleFactory::create("Fusion"));

    QPalette p;
    p.setColor(QPalette::Window,          theme::kWindow);
    p.setColor(QPalette::WindowText,      theme::kText);
    p.setColor(QPalette::Base,            theme::kCanvas);
    p.setColor(QPalette::AlternateBase,   theme::kSurface);
    p.setColor(QPalette::ToolTipBase,     theme::kSurface);
    p.setColor(QPalette::ToolTipText,     theme::kText);
    p.setColor(QPalette::Text,            theme::kText);
    p.setColor(QPalette::PlaceholderText, theme::kTextFaint);
    p.setColor(QPalette::Button,          theme::kSurface);
    p.setColor(QPalette::ButtonText,      theme::kText);
    p.setColor(QPalette::BrightText,      theme::kRed);
    p.setColor(QPalette::Link,            theme::kAccent);
    p.setColor(QPalette::Highlight,       theme::kAccent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, theme::kTextFaint);
    p.setColor(QPalette::Disabled, QPalette::Text,       theme::kTextFaint);
    app.setPalette(p);
    app.setStyleSheet(stylesheet());

    MainWindow w;
    w.show();
    return app.exec();
}

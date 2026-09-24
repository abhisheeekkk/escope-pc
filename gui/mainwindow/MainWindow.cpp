#include "mainwindow/MainWindow.h"
#include "waveform/WaveformWidget.h"
#include "timeline/TimelineWidget.h"
#include "panels/MeasurementPanel.h"
#include "panels/ChannelPanel.h"

#include "session/CaptureSession.h"
#include "session/SessionSerializer.h"
#include "SimulatedSource.h"
#include "hal/IDataSource.h"
#include "hal/StmDataSource.h"
#include "hal/StmDataSource.h"

#include <QApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QPixmap>
#include <QStandardPaths>
#include <QMenuBar>
#include <QToolBar>
#include <QStatusBar>
#include <QDockWidget>
#include <QAction>
#include <QLabel>
#include <QTimer>
#include <QSplitter>
#include <QMessageBox>
#include <thread>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("EmbeddedScope v0.1  [Digital Logic Analyzer]");
    resize(1400, 900);

    session_ = std::make_unique<escope::CaptureSession>();

    escope::SimulatedSourceConfig sim_cfg;
    sim_cfg.digital_rate = 50e6;
    sim_cfg.gen_uart     = true;
    source_ = std::make_unique<escope::SimulatedSource>(sim_cfg);

    // Central widget: waveform + protocol timeline
    auto* splitter   = new QSplitter(Qt::Vertical, this);
    waveform_widget_ = new WaveformWidget(this);
    timeline_widget_ = new TimelineWidget(this);
    splitter->addWidget(waveform_widget_);
    splitter->addWidget(timeline_widget_);
    splitter->setStretchFactor(0, 4);
    splitter->setStretchFactor(1, 1);
    setCentralWidget(splitter);

    setupDockWidgets();
    setupMenuBar();
    setupToolBar();
    setupStatusBar();
    connectSource();

    // 30 Hz display refresh
    display_timer_ = new QTimer(this);
    display_timer_->setInterval(33);
    connect(display_timer_, &QTimer::timeout, this, &MainWindow::onUpdateDisplay);
    display_timer_->start();
}

MainWindow::~MainWindow() {
    source_->stop();
    source_->close();
}

void MainWindow::setupMenuBar() {
    auto* file = menuBar()->addMenu("&File");

    file->addAction("&Open Session...", this, [this]{
        QString dir = QFileDialog::getExistingDirectory(this, "Open Session Directory",
            QStandardPaths::writableLocation(QStandardPaths::HomeLocation));
        if (dir.isEmpty()) return;
        auto loaded = escope::SessionSerializer::load(dir.toStdString());
        if (!loaded) {
            QMessageBox::warning(this, "Open Failed",
                "Could not load session from:\n" + dir);
            return;
        }
        source_->stop();
        session_ = std::move(loaded);
        capturing_ = false;
        waveform_widget_->setFollowLatest(false);
        waveform_widget_->setSession(session_.get());
        waveform_widget_->zoomFit();
        act_start_->setEnabled(true);
        act_stop_->setEnabled(false);
        status_label_->setText("Session loaded -- " + dir);
    });

    file->addAction("&Save Session...", this, [this]{
        QString dir = QFileDialog::getSaveFileName(this, "Save Session",
            QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + "/escope_session");
        if (dir.isEmpty()) return;
        status_label_->setText("Saving...");
        act_start_->setEnabled(false);
        auto* sess = session_.get();
        std::string path = dir.toStdString();
        auto* lbl = status_label_;
        auto* btn = act_start_;
        std::thread([sess, path, lbl, btn]{
            escope::SessionSerializer::save(*sess, path);
            QMetaObject::invokeMethod(lbl, [lbl, btn, d=QString::fromStdString(path)]{
                lbl->setText("Saved: " + QFileInfo(d).fileName());
                btn->setEnabled(true);
            }, Qt::QueuedConnection);
        }).detach();
    });

    file->addSeparator();

    file->addAction("Export &PNG...", this, [this]{
        QString path = QFileDialog::getSaveFileName(this, "Export Waveform Image",
            QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + "/waveform.png",
            "PNG Image (*.png)");
        if (path.isEmpty()) return;
        QPixmap px = waveform_widget_->grab();
        if (px.save(path, "PNG"))
            status_label_->setText("PNG saved: " + QFileInfo(path).fileName());
        else
            QMessageBox::warning(this, "Export Failed", "Could not save PNG to:\n" + path);
    });

    file->addAction("Export &CSV...", this, [this]{
        QString path = QFileDialog::getSaveFileName(this, "Export CSV",
            QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + "/waveform.csv",
            "CSV files (*.csv)");
        if (path.isEmpty()) return;
        double t0 = 0, t1 = 0;
        int ref = waveform_widget_->measRef();
        int tgt = waveform_widget_->measTarget();
        const auto& curs = waveform_widget_->cursors();
        bool has_pair = ref >= 0 && tgt >= 0
            && ref < (int)curs.size() && tgt < (int)curs.size()
            && curs[ref].active && curs[tgt].active
            && curs[ref].type == WaveformCursor::Type::Vertical
            && curs[tgt].type == WaveformCursor::Type::Vertical;
        if (has_pair) {
            t0 = std::min(curs[ref].t_ns, curs[tgt].t_ns);
            t1 = std::max(curs[ref].t_ns, curs[tgt].t_ns);
        }
        status_label_->setText("Exporting CSV...");
        auto* sess = session_.get();
        std::string csv = path.toStdString();
        auto* lbl = status_label_;
        std::thread([sess, csv, t0, t1, lbl]{
            escope::SessionSerializer::export_csv(*sess, csv, t0, t1, 10000);
            QMetaObject::invokeMethod(lbl, [lbl, p=QString::fromStdString(csv)]{
                lbl->setText("CSV saved: " + QFileInfo(p).fileName());
            }, Qt::QueuedConnection);
        }).detach();
    });

    file->addSeparator();
    file->addAction("E&xit", qApp, &QApplication::quit);

    auto* view = menuBar()->addMenu("&View");
    view->addAction("Measurements", measure_dock_, &QDockWidget::setVisible);
    view->addAction("Channels",     channel_dock_, &QDockWidget::setVisible);

    auto* help = menuBar()->addMenu("&Help");
    help->addAction("&About", this, [this]{
        QMessageBox::about(this, "EmbeddedScope",
            "<b>EmbeddedScope v0.1 -- Digital Logic Analyzer</b><br><br>"
            "<b>16 channels simulated:</b><br>"
            "D0 UART 115200 / D1 UART 9600<br>"
            "D2/3/4 SPI 1MHz / D5/6 I2C 400kHz<br>"
            "D7 PWM 10kHz 50% / D8 PWM 1kHz 25% / D9 PWM 500Hz 75%<br>"
            "D10 10kHz / D11 1kHz / D12 100Hz / D13 10Hz<br>"
            "D14 IRQ pulse 5us/2ms / D15 LED 100ms<br><br>"
            "<b>Controls:</b><br>"
            "Click = place cursor | Drag cursor = move | RClick cursor = delete<br>"
            "Del = clear all cursors | L = resume rolling<br>"
            "Scroll = zoom time | Drag (right-click+drag) = pan<br>"
            "F = fit view | +/- = zoom in/out | arrow keys = pan");
    });
}

void MainWindow::setupToolBar() {
    auto* tb = addToolBar("Capture");
    tb->setMovable(false);

    act_auto_   = tb->addAction("AUTO");
    act_single_ = tb->addAction("SINGLE");
    tb->addSeparator();
    act_start_  = tb->addAction("Run");
    act_stop_   = tb->addAction("Stop");
    act_stop_->setEnabled(false);
    tb->addSeparator();

    auto* trig_label = new QLabel("  Trigger: ch7 (10kHz)", tb);
    trig_label->setStyleSheet("color:#888;");
    tb->addWidget(trig_label);

    connect(act_start_,  &QAction::triggered, this, &MainWindow::onStartCapture);
    connect(act_stop_,   &QAction::triggered, this, &MainWindow::onStopCapture);
    connect(act_single_, &QAction::triggered, this, &MainWindow::onTriggerSingle);
    connect(act_auto_,   &QAction::triggered, this, &MainWindow::onTriggerAuto);

    // STM32 hardware toggle
    tb->addSeparator();
    auto* act_hw = tb->addAction("Connect STM32");
    act_hw->setCheckable(true);
    connect(act_hw, &QAction::toggled, this, [this, act_hw](bool checked) {
        if (checked) {
            source_->stop();
            source_->close();
            auto stm = std::make_unique<escope::StmDataSource>();
            if (stm->enumerate().empty()) {
                act_hw->setChecked(false);
                status_label_->setText("STM32 not found -- check /dev/ttyACM*");
                return;
            }
            source_ = std::move(stm);
            connectSource();
            /* Start acquisition immediately on the new source */
            session_->reset();
            source_->configure(*session_);
            source_->start(*session_);
            capturing_ = true;
            display_frame_ = 0;
            waveform_widget_->setFollowLatest(true);
            act_stop_->setEnabled(true);
            act_start_->setEnabled(false);
            act_hw->setText("Disconnect STM32");
            status_label_->setText("Connected to STM32 EmbeddedScope");
        } else {
            source_->stop();
            source_->close();
            capturing_ = false;
            escope::SimulatedSourceConfig cfg;
            cfg.digital_rate = 50e6;
            cfg.gen_uart = true;
            source_ = std::make_unique<escope::SimulatedSource>(cfg);
            connectSource();
            act_start_->setEnabled(true);
            act_stop_->setEnabled(false);
            act_hw->setText("Connect STM32");
            status_label_->setText("Simulated device");
        }
    });
}

void MainWindow::setupDockWidgets() {
    measure_panel_ = new MeasurementPanel(this);
    measure_dock_  = new QDockWidget("Measurements", this);
    measure_dock_->setWidget(measure_panel_);
    measure_dock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::RightDockWidgetArea, measure_dock_);

    channel_panel_ = new ChannelPanel(this);
    channel_dock_  = new QDockWidget("Channels", this);
    channel_dock_->setWidget(channel_panel_);
    channel_dock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    addDockWidget(Qt::LeftDockWidgetArea, channel_dock_);
}

void MainWindow::setupStatusBar() {
    status_label_ = new QLabel("Ready -- Simulated 16-channel device");
    statusBar()->addWidget(status_label_);
}

void MainWindow::connectSource() {
    source_->set_data_callback([this]{
        QMetaObject::invokeMethod(this, &MainWindow::onNewData, Qt::QueuedConnection);
    });
    source_->set_trigger_callback([this](const escope::TriggerEvent& evt){
        Q_UNUSED(evt);
        QMetaObject::invokeMethod(this, [this]{
            status_label_->setText("Triggered");
        }, Qt::QueuedConnection);
    });
    source_->open();
}

void MainWindow::onStartCapture() {
    session_->reset();
    source_->configure(*session_);
    source_->start(*session_);
    capturing_     = true;
    display_frame_ = 0;
    waveform_widget_->setFollowLatest(true);
    act_start_->setEnabled(false);
    act_stop_->setEnabled(true);
    status_label_->setText("Capturing -- Simulated 16ch  50 MS/s  [Rolling]");
}

void MainWindow::onStopCapture() {
    source_->stop();
    capturing_ = false;
    act_start_->setEnabled(true);
    act_stop_->setEnabled(false);
    status_label_->setText("Stopped");
}

void MainWindow::onNewData() {}

void MainWindow::onUpdateDisplay() {
    if (!session_) return;
    waveform_widget_->setSession(session_.get());
    waveform_widget_->update();
    ++display_frame_;

    // Status bar
    if (capturing_ && display_frame_ % 10 == 0) {
        if (waveform_widget_->followLatest())
            status_label_->setText("Capturing -- Simulated 16ch  50 MS/s  [Rolling]");
        else
            status_label_->setText("Capturing -- Simulated 16ch  50 MS/s  [Paused -- L to resume]");
    }

    // Panels at ~6 Hz
    if (display_frame_ % 5 == 0) {
        measure_panel_->updateFrom(*session_);
        channel_panel_->updateFrom(*session_);
    }
}

void MainWindow::onTriggerSingle() {
    escope::TriggerConfig cfg = session_->trigger().config();
    cfg.mode = escope::TriggerMode::Single;
    session_->trigger().set_config(cfg);
    status_label_->setText("Single trigger armed");
}

void MainWindow::onTriggerAuto() {
    escope::TriggerConfig cfg = session_->trigger().config();
    cfg.mode = escope::TriggerMode::Auto;
    session_->trigger().set_config(cfg);
}

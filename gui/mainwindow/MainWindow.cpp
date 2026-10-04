#include "mainwindow/MainWindow.h"
#include "waveform/WaveformWidget.h"
#include "timeline/TimelineWidget.h"
#include "panels/MeasurementPanel.h"
#include "panels/ChannelPanel.h"
#include "panels/ProtocolPanel.h"

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
#include <QActionGroup>
#include <QLabel>
#include <QComboBox>
#include <QPushButton>
#include <QMenu>
#include <QTimer>
#include <QSplitter>
#include <QMessageBox>
#include <QLineEdit>
#include <QDoubleValidator>
#include <QDialog>
#include <QFormLayout>
#include <QDialogButtonBox>
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
            "[ / ] = jump to previous / next edge | Home / End = start / end of capture<br>"
            "Drag a rectangle = zoom to that area<br>"
            "Scroll = zoom time | Drag (right-click+drag) = pan<br>"
            "F = fit view | +/- = zoom in/out | arrow keys = pan");
    });
}

void MainWindow::setupToolBar() {
    auto* tb = addToolBar("Capture");
    tb->setMovable(false);

    // Momentary buttons, not persistent toggles -- AUTO/SINGLE fire an
    // action (autoset T/div, arm a single capture) rather than representing
    // a mode that should stay visually highlighted afterwards.
    act_auto_   = tb->addAction("AUTO");
    act_single_ = tb->addAction("SINGLE");
    tb->addSeparator();
    act_start_  = tb->addAction("Run");
    act_stop_   = tb->addAction("Stop");
    act_stop_->setEnabled(false);
    tb->addSeparator();

    connect(act_start_,  &QAction::triggered, this, &MainWindow::onStartCapture);
    connect(act_stop_,   &QAction::triggered, this, &MainWindow::onStopCapture);
    connect(act_single_, &QAction::triggered, this, &MainWindow::onTriggerSingle);
    connect(act_auto_,   &QAction::triggered, this, &MainWindow::onTriggerAuto);

    /* ---- Time/div dropdown ---- */
    tb->addSeparator();
    auto* tdiv_label = new QLabel("  T/div:", tb);
    tdiv_label->setStyleSheet("color:#aaa;");
    tb->addWidget(tdiv_label);

    auto* tdiv_combo = new QComboBox(tb);
    tdiv_combo->setStyleSheet("color:#eee; background:#333; min-width:80px;");
    const QStringList tdiv_labels = {
        "10 ns","50 ns","100 ns","500 ns","1 us","5 us","10 us","50 us",
        "100 us","500 us","1 ms","5 ms","10 ms","50 ms",
        "100 ms","500 ms","1 s","5 s","10 s","50 s"
    };
    const QList<double> tdiv_values = {
        10,50,100,500,1e3,5e3,10e3,50e3,
        100e3,500e3,1e6,5e6,10e6,50e6,
        100e6,500e6,1e9,5e9,10e9,50e9
    };
    const int custom_idx = tdiv_labels.size(); /* "Custom..." lives past every preset */
    for (const auto& s : tdiv_labels) tdiv_combo->addItem(s);
    tdiv_combo->addItem("Custom...");
    tdiv_combo->setCurrentIndex(10); /* 1 ms default */
    tb->addWidget(tdiv_combo);
    connect(waveform_widget_, &WaveformWidget::timeDivChanged,
        this, [tdiv_combo, tdiv_values](double ns) {
            /* Find closest preset and update dropdown without triggering setTimePerDiv */
            int best = 0;
            double best_d = 1e18;
            for (int i = 0; i < tdiv_values.size(); i++) {
                double d = std::abs(tdiv_values[i] - ns);
                if (d < best_d) { best_d = d; best = i; }
            }
            QSignalBlocker blocker(tdiv_combo);
            tdiv_combo->setCurrentIndex(best);
        });

    connect(tdiv_combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, [this, tdiv_combo, tdiv_values, custom_idx](int idx) {
            if (!waveform_widget_) return;
            if (idx != custom_idx) { waveform_widget_->setTimePerDiv(tdiv_values[idx]); return; }

            // "Custom..." selected -- pop a small dialog for value + unit.
            QDialog dlg(this);
            dlg.setWindowTitle("Custom T/div");
            auto* form = new QFormLayout(&dlg);

            auto* value_edit = new QLineEdit(&dlg);
            value_edit->setValidator(new QDoubleValidator(0.001, 1e9, 6, value_edit));
            value_edit->setText("300");
            form->addRow("Value:", value_edit);

            auto* unit_combo = new QComboBox(&dlg);
            unit_combo->addItems({"ns", "us", "ms", "s"});
            unit_combo->setCurrentIndex(0);
            form->addRow("Unit:", unit_combo);

            auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
            form->addRow(buttons);
            connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
            connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
            value_edit->setFocus();
            value_edit->selectAll();

            if (dlg.exec() == QDialog::Accepted) {
                bool ok = false;
                double value = value_edit->text().toDouble(&ok);
                static const double unit_ns[] = {1.0, 1e3, 1e6, 1e9}; // ns, us, ms, s
                if (ok && value > 0.0)
                    waveform_widget_->setTimePerDiv(value * unit_ns[unit_combo->currentIndex()]);
            }
            // Leaving "Custom..." selected would re-open the dialog the next
            // time this index fires; timeDivChanged (emitted by
            // setTimePerDiv) moves the combo back to the nearest matching
            // preset. If the dialog was cancelled, do that ourselves so the
            // combo doesn't sit stuck on "Custom...".
            if (tdiv_combo->currentIndex() == custom_idx) {
                QSignalBlocker blocker(tdiv_combo);
                tdiv_combo->setCurrentIndex(10);
            }
        });

    /* ---- Cursors ---- */
    tb->addSeparator();
    auto* act_add_cursor   = tb->addAction("Add Cursor");
    auto* act_clear_cursor = tb->addAction("Clear Cursors");
    connect(act_add_cursor, &QAction::triggered, this, [this]() {
        if (waveform_widget_) waveform_widget_->addCursorAtCenter();
    });
    connect(act_clear_cursor, &QAction::triggered, this, [this]() {
        if (waveform_widget_) waveform_widget_->clearCursors();
    });

    /* ---- Channel selector ---- */
    tb->addSeparator();
    auto* ch_label = new QLabel("  Ch:", tb);
    ch_label->setStyleSheet("color:#aaa;");
    tb->addWidget(ch_label);

    auto* ch_btn = new QPushButton("Select", tb);
    ch_btn->setStyleSheet("color:#eee; background:#333; padding:2px 6px;");
    tb->addWidget(ch_btn);

    auto* ch_menu = new QMenu(ch_btn);
    ch_menu->setStyleSheet("color:#eee; background:#222;");
    for (int i = 0; i < 8; i++) {
        auto* act = ch_menu->addAction(QString("D%1").arg(i));
        act->setCheckable(true);
        act->setChecked(i == 0); /* only D0 shown by default -- see ch_visible_ */
        connect(act, &QAction::toggled, this, [this, i](bool checked) {
            if (waveform_widget_) waveform_widget_->setChannelVisible(i, checked);
        });
    }
    /* Select All / None */
    ch_menu->addSeparator();
    auto* all_act  = ch_menu->addAction("All");
    auto* none_act = ch_menu->addAction("None");
    connect(all_act,  &QAction::triggered, this, [ch_menu]() {
        for (auto* a : ch_menu->actions()) if (a->isCheckable()) a->setChecked(true);
    });
    connect(none_act, &QAction::triggered, this, [ch_menu]() {
        for (auto* a : ch_menu->actions()) if (a->isCheckable()) a->setChecked(false);
    });
    connect(ch_btn, &QPushButton::clicked, ch_btn, [ch_menu, ch_btn]() {
        ch_menu->exec(ch_btn->mapToGlobal(QPoint(0, ch_btn->height())));
    });

    /* ---- Protocol decoder selector (dock stays hidden until one is chosen) ---- */
    tb->addSeparator();
    auto* proto_btn = new QPushButton("Protocol", tb);
    proto_btn->setStyleSheet("color:#eee; background:#333; padding:2px 6px;");
    tb->addWidget(proto_btn);

    auto* proto_menu  = new QMenu(proto_btn);
    proto_menu->setStyleSheet("color:#eee; background:#222;");
    auto* proto_group = new QActionGroup(proto_menu);
    auto* proto_off   = proto_menu->addAction("Off");
    auto* proto_uart  = proto_menu->addAction("UART");
    auto* proto_i2c   = proto_menu->addAction("I2C");
    for (auto* a : {proto_off, proto_uart, proto_i2c}) {
        a->setCheckable(true);
        proto_group->addAction(a);
    }
    proto_off->setChecked(true);
    auto selectProtocol = [this, proto_btn](const QString& name) {
        protocol_panel_->setProtocol(name);
        protocol_dock_->setVisible(!name.isEmpty());
        proto_btn->setText(name.isEmpty() ? "Protocol" : "Protocol: " + name);
    };
    connect(proto_off,  &QAction::triggered, this, [selectProtocol]{ selectProtocol({}); });
    connect(proto_uart, &QAction::triggered, this, [selectProtocol]{ selectProtocol("UART"); });
    connect(proto_i2c,  &QAction::triggered, this, [selectProtocol]{ selectProtocol("I2C"); });
    connect(proto_btn, &QPushButton::clicked, proto_btn, [proto_menu, proto_btn]() {
        proto_menu->exec(proto_btn->mapToGlobal(QPoint(0, proto_btn->height())));
    });

    // STM32 hardware toggle
    tb->addSeparator();
    auto* act_hw = tb->addAction("Connect eScope");
    act_hw->setCheckable(true);
    connect(act_hw, &QAction::toggled, this, [this, act_hw](bool checked) {
        if (checked) {
            source_->stop();
            source_->close();
            auto stm = std::make_unique<escope::StmDataSource>();
            if (stm->enumerate().empty()) {
                act_hw->setChecked(false);
                status_label_->setText("eScope not found -- check /dev/ttyACM*");
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
            waveform_widget_->resetCaptureTime();
            act_stop_->setEnabled(true);
            act_start_->setEnabled(false);
            act_hw->setText("Disconnect eScope");
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
            act_hw->setText("Connect eScope");
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

    protocol_panel_ = new ProtocolPanel(this);
    protocol_dock_  = new QDockWidget("Protocol", this);
    protocol_dock_->setWidget(protocol_panel_);
    protocol_dock_->setAllowedAreas(Qt::RightDockWidgetArea);
    // Fixed in place: no float/close/move buttons. It appears and disappears
    // via the toolbar's Protocol menu and is resized by dragging its edge.
    protocol_dock_->setFeatures(QDockWidget::NoDockWidgetFeatures);
    addDockWidget(Qt::RightDockWidgetArea, protocol_dock_);
    protocol_dock_->hide();   // shown only once a protocol is picked in the toolbar
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
            bool single = session_->trigger().config().mode == escope::TriggerMode::Single;
            status_label_->setText(single ? "Triggered -- single capture stopped"
                                           : "Triggered");
            // SINGLE mode: capture exactly one triggered burst, then stop.
            // Previously AUTO/SINGLE only recorded a mode nobody ever acted
            // on, so SINGLE never actually stopped anything.
            if (single && capturing_) onStopCapture();
        }, Qt::QueuedConnection);
    });
    source_->open();
}

void MainWindow::onStartCapture() {
    // Run always means continuous capture until Stop, regardless of
    // whatever mode a previous SINGLE press left behind -- otherwise
    // pressing Run after a single-shot capture would immediately arm
    // and auto-stop again on the very next trigger.
    escope::TriggerConfig cfg = session_->trigger().config();
    cfg.mode = escope::TriggerMode::Auto;
    session_->trigger().set_config(cfg);

    session_->reset();
    source_->configure(*session_);
    source_->start(*session_);
    capturing_     = true;
    display_frame_ = 0;
    waveform_widget_->resetCaptureTime();
    act_start_->setEnabled(false);
    act_stop_->setEnabled(true);
    status_label_->setText("Capturing -- STM32 8ch 48 MS/s  [Rolling]");
}

void MainWindow::onStopCapture() {
    source_->stop();
    capturing_ = false;
    /* Freeze the view -- stop scrolling, keep data visible, keep whatever
     * T/div the user had selected (zoomFit() used to reset it to fit the
     * whole capture, overriding the user's choice the moment they stopped). */
    if (waveform_widget_) {
        waveform_widget_->setFollowLatest(false);
        waveform_widget_->snapToSignal();   // don't leave the view parked on an idle gap
    }
    act_start_->setEnabled(true);
    act_stop_->setEnabled(false);
    status_label_->setText("Stopped -- right-click to add cursors, scroll to zoom");
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
            status_label_->setText("Capturing -- STM32 8ch 48 MS/s  [Rolling]");
        else
            status_label_->setText("Capturing -- STM32 8ch 48 MS/s  [Paused -- L to resume]");
    }

    // Panels at ~6 Hz
    if (display_frame_ % 5 == 0) {
        measure_panel_->updateFrom(*session_);
        channel_panel_->updateFrom(*session_);
    }
    // Protocol decode is heavier: ~2 Hz
    if (display_frame_ % 15 == 0) {
        const double t0 = waveform_widget_->timeOffset();
        protocol_panel_->updateFrom(*session_, t0,
            t0 + waveform_widget_->timePerDiv() * WaveformWidget::HDIVS);
    }
}

void MainWindow::onTriggerSingle() {
    // SINGLE is self-contained: run continuously for a moment first --
    // arming single-shot mode immediately would usually stop after the
    // very next burst (bursts arrive fast), before there's anything useful
    // on screen. Instead start (or keep) running normally, then arm
    // single-shot mode after a short settle delay so the *next* trigger
    // after that stops it -- the trigger callback in connectSource() checks
    // the mode and calls onStopCapture() the first time it fires.
    if (!capturing_) {
        escope::TriggerConfig cfg = session_->trigger().config();
        cfg.mode = escope::TriggerMode::Auto;
        session_->trigger().set_config(cfg);

        session_->reset();
        source_->configure(*session_);
        source_->start(*session_);
        capturing_     = true;
        display_frame_ = 0;
        waveform_widget_->resetCaptureTime();
        act_start_->setEnabled(false);
        act_stop_->setEnabled(true);
    }
    status_label_->setText("Single -- running, will grab one capture shortly");

    static constexpr int SINGLE_SETTLE_MS = 1000;
    QTimer::singleShot(SINGLE_SETTLE_MS, this, [this]() {
        if (!capturing_) return; // Stop was pressed during the settle delay
        escope::TriggerConfig cfg = session_->trigger().config();
        cfg.mode = escope::TriggerMode::Single;
        session_->trigger().set_config(cfg);
        source_->configure(*session_);          // device: wait for a real trigger
        status_label_->setText("Single trigger armed -- waiting for one capture");
    });
}

void MainWindow::onTriggerAuto() {
    escope::TriggerConfig cfg = session_->trigger().config();
    cfg.mode = escope::TriggerMode::Auto;
    session_->trigger().set_config(cfg);
    source_->configure(*session_);

    // AUTO also autosets T/div from the signal itself, like a scope's
    // "Autoset" -- measure the visible channel's period from recent edges
    // and fit a few cycles across the screen.
    if (waveform_widget_) {
        if (waveform_widget_->autoScaleTimeDiv())
            status_label_->setText("Auto -- T/div scaled to signal frequency");
        else
            status_label_->setText("Auto -- not enough signal to measure frequency yet");
    }
}

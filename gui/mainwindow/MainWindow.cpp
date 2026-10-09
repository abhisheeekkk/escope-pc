#include "mainwindow/MainWindow.h"
#include "waveform/WaveformWidget.h"
#include "panels/ChannelPanel.h"
#include "panels/ProtocolPanel.h"
#include "theme/Theme.h"

#include "session/CaptureSession.h"
#include "session/SessionSerializer.h"
#include "hal/IDataSource.h"
#include "hal/StmDataSource.h"

#include <QApplication>
#include <QCloseEvent>
#include <QSettings>
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
#include <QMessageBox>
#include <QLineEdit>
#include <QDoubleValidator>
#include <QDialog>
#include <QFormLayout>
#include <QDialogButtonBox>
#include <QProgressBar>
#include <QPointer>
#include <QLocale>
#include <chrono>
#include <QShortcut>
#include <QGridLayout>
#include <QHBoxLayout>
#include <thread>


MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("EmbeddedScope");
    resize(1400, 900);

    session_ = std::make_unique<escope::CaptureSession>();

    waveform_widget_ = new WaveformWidget(this);
    setCentralWidget(waveform_widget_);

    setupDockWidgets();
    setupMenuBar();
    setupToolBar();
    setupStatusBar();

    // 30 Hz display refresh
    display_timer_ = new QTimer(this);
    display_timer_->setInterval(33);
    connect(display_timer_, &QTimer::timeout, this, &MainWindow::onUpdateDisplay);
    display_timer_->start();

    // Restore the previous window layout
    QSettings st;
    if (st.contains("window/geometry")) restoreGeometry(st.value("window/geometry").toByteArray());
    if (st.contains("window/state"))    restoreState(st.value("window/state").toByteArray());
}

void MainWindow::closeEvent(QCloseEvent* e) {
    QSettings st;
    st.setValue("window/geometry", saveGeometry());
    st.setValue("window/state", saveState());
    QMainWindow::closeEvent(e);
}

MainWindow::~MainWindow() {
    if (source_) { source_->stop(); source_->close(); }
}

void MainWindow::setupMenuBar() {
    auto* file = menuBar()->addMenu("&File");

    file->addAction("&Open Session…", this, [this]{
        if (io_busy_) { setStatus("Another file operation is still running."); return; }
        const QString dir = QFileDialog::getExistingDirectory(this, "Open Session",
            QStandardPaths::writableLocation(QStandardPaths::HomeLocation),
            QFileDialog::ShowDirsOnly | QFileDialog::DontUseNativeDialog);
        if (!dir.isEmpty()) openSession(dir);
    });

    file->addAction("&Save Session…", this, [this]{
        if (io_busy_) { setStatus("Another file operation is still running."); return; }
        const QString dir = QFileDialog::getSaveFileName(this, "Save Session",
            QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + "/escope_session",
            QString(), nullptr, QFileDialog::DontUseNativeDialog);
        if (!dir.isEmpty()) saveSession(dir);
    });

    file->addSeparator();

    file->addAction("Export &Image…", this, [this]{
        QString path = QFileDialog::getSaveFileName(this, "Export Image",
            QStandardPaths::writableLocation(QStandardPaths::PicturesLocation) + "/waveform.png",
            "PNG Image (*.png)", nullptr, QFileDialog::DontUseNativeDialog);
        if (path.isEmpty()) return;
        // Take the picture from the render buffer so the overlay (labels, cursors, readout) is included
        QImage img = waveform_widget_->grabFramebuffer();
        if (img.isNull()) img = waveform_widget_->grab().toImage();
        if (img.save(path, "PNG"))
            setStatus("Image saved: " + QFileInfo(path).fileName());
        else
            QMessageBox::warning(this, "Export Image", "The image could not be saved to:\n" + path);
    });

    file->addAction("Export &CSV…", this, [this]{
        QString path = QFileDialog::getSaveFileName(this, "Export CSV",
            QStandardPaths::writableLocation(QStandardPaths::HomeLocation) + "/waveform.csv",
            "CSV files (*.csv)", nullptr, QFileDialog::DontUseNativeDialog);
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
        setStatus("Exporting CSV…");
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
    view->addAction(channel_dock_->toggleViewAction());

    auto* help = menuBar()->addMenu("&Help");
    help->addAction("&Controls", this, [this]{ showControlsSheet(); });
    help->addAction("&About EmbeddedScope", this, [this]{
        QMessageBox::about(this, "About EmbeddedScope",
            "<b>EmbeddedScope</b> " + QApplication::applicationVersion() + "<br>"
            "Logic analyzer and protocol decoder.<br><br>"
            "8 channels at 48 MS/s over USB.<br>"
            "UART, I2C, SPI and CAN decoding.");
    });
}

void MainWindow::setStatus(const QString& text) {
    // The dot shows the state at a glance: green while capturing, amber when a trigger is armed
    // or has fired, grey otherwise.
    QColor c = theme::kTextFaint;
    if (capturing_) c = theme::kGreen;
    if (text.startsWith("Single") || text.startsWith("Triggered")) c = theme::kOrange;
    status_label_->setText(QString("<span style='color:%1; font-size:13pt'>&#9679;</span>&nbsp; %2")
                               .arg(c.name(), text.toHtmlEscaped()));
}

void MainWindow::beginBusy(const QString& text) {
    io_busy_ = true;
    io_cancel_ = std::make_shared<std::atomic<bool>>(false);
    setStatus(text);
    busy_bar_->setValue(0);
    busy_bar_->show();
    busy_cancel_->show();
}

void MainWindow::setBusyProgress(double fraction) {
    busy_bar_->setValue(static_cast<int>(std::clamp(fraction, 0.0, 1.0) * 1000.0));
}

void MainWindow::endBusy() {
    io_busy_ = false;
    busy_bar_->hide();
    busy_cancel_->hide();
}

// Opening happens on a worker thread (the file is memory-mapped and decoded on every core); this
// thread only shows progress and, at the end, swaps the finished session in.
void MainWindow::openSession(const QString& dir) {
    beginBusy("Opening " + QFileInfo(dir).fileName() + "\u2026");
    const std::string path = dir.toStdString();
    auto cancel = io_cancel_;
    QPointer<MainWindow> self(this);
    std::thread([self, path, dir, cancel] {
        escope::SessionSerializer::LoadReport rep;
        auto last = std::chrono::steady_clock::now();
        auto progress = [&](double f) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last > std::chrono::milliseconds(40)) {           // at most about 25 updates a second
                last = now;
                QMetaObject::invokeMethod(self.data(), [self, f] { if (self) self->setBusyProgress(f); },
                                          Qt::QueuedConnection);
            }
            return !cancel->load();
        };
        auto* raw = escope::SessionSerializer::load(path, &rep, progress).release();
        QMetaObject::invokeMethod(self.data(), [self, raw, rep, dir] {
            std::unique_ptr<escope::CaptureSession> loaded(raw);
            if (!self) return;
            self->endBusy();
            if (!loaded) {
                if (rep.message == "Cancelled") self->setStatus("Open cancelled.");
                else QMessageBox::warning(self, "Open Session",
                         "The session could not be opened.\n\n" + QString::fromStdString(rep.message));
                return;
            }
            if (self->source_) self->source_->stop();
            self->session_ = std::move(loaded);
            self->capturing_ = false;
            self->waveform_widget_->setFollowLatest(false);
            self->waveform_widget_->setSession(self->session_.get());
            self->waveform_widget_->zoomFit();
            self->act_start_->setEnabled(self->source_ != nullptr);
            self->act_stop_->setEnabled(false);
            self->setStatus(QString("Opened %1: %2 edges in %3 s")
                                .arg(QFileInfo(dir).fileName())
                                .arg(QLocale().toString(static_cast<qulonglong>(rep.edges)))
                                .arg(rep.seconds, 0, 'f', 2));
            if (rep.damaged_blocks > 0)
                QMessageBox::warning(self, "Opened with problems",
                    QString::fromStdString(rep.message) + "\n\nThe rest of the session is intact.");
        }, Qt::QueuedConnection);
    }).detach();
}

void MainWindow::saveSession(const QString& dir) {
    beginBusy("Saving " + QFileInfo(dir).fileName() + "\u2026");
    const std::string path = dir.toStdString();
    auto cancel = io_cancel_;
    auto* sess = session_.get();
    QPointer<MainWindow> self(this);
    std::thread([self, sess, path, dir, cancel] {
        auto last = std::chrono::steady_clock::now();
        auto progress = [&](double f) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last > std::chrono::milliseconds(40)) {
                last = now;
                QMetaObject::invokeMethod(self.data(), [self, f] { if (self) self->setBusyProgress(f); },
                                          Qt::QueuedConnection);
            }
            return !cancel->load();
        };
        const auto rep = escope::SessionSerializer::save(*sess, path, progress);
        QMetaObject::invokeMethod(self.data(), [self, rep, dir] {
            if (!self) return;
            self->endBusy();
            if (rep.ok)
                self->setStatus(QString("Saved %1 (%2 MB, %3 s)").arg(QFileInfo(dir).fileName())
                                    .arg(rep.bytes / 1e6, 0, 'f', 1).arg(rep.seconds, 0, 'f', 2));
            else if (rep.message == "Cancelled")
                self->setStatus("Save cancelled. Nothing was changed.");
            else
                QMessageBox::warning(self, "Save Session",
                    "The session could not be saved.\n\n" + QString::fromStdString(rep.message));
        }, Qt::QueuedConnection);
    }).detach();
}

// History is kept only for the channels that are shown. Hidden channels would otherwise use up the
// per-channel edge budget (busy ones fill it first) and shorten what a session can hold.
void MainWindow::applyStoreMask() {
    uint8_t mask = 0;
    for (int i = 0; i < 8; ++i)
        if (ch_actions_[i] && ch_actions_[i]->isChecked()) mask |= static_cast<uint8_t>(1u << i);
    if (session_) session_->digital_buffer().set_store_mask(mask);
}

void MainWindow::showControlsSheet() {
    if (controls_dlg_) { controls_dlg_->show(); controls_dlg_->raise(); controls_dlg_->activateWindow(); return; }
    controls_dlg_ = new QDialog(this);
    controls_dlg_->setWindowTitle("Controls");
    controls_dlg_->setModal(false);                      // never blocks the main window
    controls_dlg_->setAttribute(Qt::WA_DeleteOnClose, false);
    auto* grid = new QGridLayout(controls_dlg_);
    grid->setContentsMargins(24, 22, 24, 22);
    grid->setHorizontalSpacing(28);
    grid->setVerticalSpacing(11);

    struct Row { const char* what; QStringList keys; };
    const std::vector<std::pair<QString, std::vector<Row>>> groups = {
        {"Capture", {{"Start or stop", {"Space"}}, {"Resume the live view", {"L"}}}},
        {"View", {{"Zoom in or out", {"+", "-"}}, {"Zoom about the pointer", {"Ctrl", "Scroll"}},
                  {"Zoom to an area", {"Drag"}}, {"Pan", {"Scroll"}}, {"Fit the capture", {"F"}},
                  {"Previous or next edge", {"[", "]"}}, {"Start or end of capture", {"Home", "End"}}}},
        {"Cursors", {{"Move a cursor", {"Drag"}}, {"Delete a cursor", {"Right-click"}},
                     {"Clear all cursors", {"Delete"}}, {"Choose the pair shown", {"Click a badge"}}}},
    };
    int r = 0;
    for (const auto& g : groups) {
        auto* h = new QLabel(g.first.toUpper());
        QFont f = theme::ui(8.0, QFont::DemiBold);
        f.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
        h->setFont(f);
        h->setStyleSheet(QString("color:%1; padding-top:%2px;").arg(theme::css(theme::kTextMuted)).arg(r ? 10 : 0));
        grid->addWidget(h, r++, 0, 1, 2);
        for (const auto& row : g.second) {
            grid->addWidget(new QLabel(row.what), r, 0);
            auto* keys = new QHBoxLayout;
            keys->setSpacing(5);
            keys->addStretch();
            for (const auto& k : row.keys) {
                auto* cap = new QLabel(k);
                cap->setObjectName("keycap");
                cap->setFont(theme::mono(9.0, QFont::DemiBold));
                keys->addWidget(cap);
            }
            grid->addLayout(keys, r++, 1);
        }
    }
    controls_dlg_->show();
}

void MainWindow::setupToolBar() {
    auto* tb = addToolBar("Capture");
    tb->setMovable(false);
    tb->setToolButtonStyle(Qt::ToolButtonTextOnly);

    // Momentary buttons: Auto and Single fire an action (autoset T/div, arm a
    // single capture) rather than a mode that stays highlighted.
    act_auto_   = tb->addAction("Auto");
    act_single_ = tb->addAction("Single");
    act_auto_->setToolTip("Auto: scale the time base to the signal");
    act_single_->setToolTip("Single: capture one triggered burst, then stop");
    tb->addSeparator();
    act_start_  = tb->addAction(QString::fromUtf8("\u25B6  Run"));
    act_stop_   = tb->addAction(QString::fromUtf8("\u25A0  Stop"));
    act_start_->setToolTip("Start continuous capture (Space)");
    act_stop_->setToolTip("Stop capture (Space)");
    act_stop_->setEnabled(false);
    act_start_->setEnabled(false);       // until a device is connected
    act_single_->setEnabled(false);
    tb->addSeparator();

    if (auto* b = tb->widgetForAction(act_start_)) b->setObjectName("runButton");
    if (auto* b = tb->widgetForAction(act_stop_))  b->setObjectName("stopButton");

    // Space starts or stops, the way media and design tools behave
    auto* space = new QShortcut(QKeySequence(Qt::Key_Space), this);
    connect(space, &QShortcut::activated, this, [this] { capturing_ ? onStopCapture() : onStartCapture(); });

    connect(act_start_,  &QAction::triggered, this, &MainWindow::onStartCapture);
    connect(act_stop_,   &QAction::triggered, this, &MainWindow::onStopCapture);
    connect(act_single_, &QAction::triggered, this, &MainWindow::onTriggerSingle);
    connect(act_auto_,   &QAction::triggered, this, &MainWindow::onTriggerAuto);

    /* ---- Time/div dropdown ---- */
    tb->addSeparator();
    tb->addWidget(new QLabel("Time/div ", tb));

    auto* tdiv_combo = new QComboBox(tb);
    tdiv_combo->setToolTip("Time per division");
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
    const int custom_idx = tdiv_labels.size(); /* "Custom…" lives past every preset */
    for (const auto& s : tdiv_labels) tdiv_combo->addItem(s);
    tdiv_combo->addItem("Custom…");
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

            // "Custom…" selected -- pop a small dialog for value + unit.
            QDialog dlg(this);
            dlg.setWindowTitle("Custom Time/div");
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
            // Leaving "Custom…" selected would re-open the dialog the next
            // time this index fires; timeDivChanged (emitted by
            // setTimePerDiv) moves the combo back to the nearest matching
            // preset. If the dialog was cancelled, do that ourselves so the
            // combo doesn't sit stuck on "Custom…".
            if (tdiv_combo->currentIndex() == custom_idx) {
                QSignalBlocker blocker(tdiv_combo);
                tdiv_combo->setCurrentIndex(10);
            }
        });

    /* ---- Cursors ---- */
    tb->addSeparator();
    auto* act_add_cursor   = tb->addAction("Add Cursor");
    auto* act_clear_cursor = tb->addAction("Clear Cursors");
    act_add_cursor->setToolTip("Place a cursor at the centre of the view");
    act_clear_cursor->setToolTip("Remove all cursors");
    connect(act_add_cursor, &QAction::triggered, this, [this]() {
        if (waveform_widget_) waveform_widget_->addCursorAtCenter();
    });
    connect(act_clear_cursor, &QAction::triggered, this, [this]() {
        if (waveform_widget_) waveform_widget_->clearCursors();
    });

    /* ---- Channel selector ---- */
    tb->addSeparator();
    auto* ch_btn = new QPushButton("Channels", tb);
    ch_btn->setToolTip("Choose which channels are displayed");
    tb->addWidget(ch_btn);

    auto* ch_menu = new QMenu(ch_btn);
    for (int i = 0; i < 8; i++) {
        auto* act = ch_menu->addAction(QString("D%1").arg(i));
        act->setCheckable(true);
        act->setChecked(i == 0); /* only D0 shown by default -- see ch_visible_ */
        ch_actions_[i] = act;
        connect(act, &QAction::toggled, this, [this, i](bool checked) {
            if (waveform_widget_) waveform_widget_->setChannelVisible(i, checked);
            if (channel_panel_)   channel_panel_->setChannelVisible(i, checked);
            applyStoreMask();
        });
        channel_panel_->setChannelVisible(i, i == 0);
    }
    connect(channel_panel_, &ChannelPanel::channelClicked, this, [this](int i) {
        if (ch_actions_[i]) ch_actions_[i]->toggle();
    });
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
    proto_btn->setToolTip("Choose a protocol decoder");
    tb->addWidget(proto_btn);

    auto* proto_menu  = new QMenu(proto_btn);
    auto* proto_group = new QActionGroup(proto_menu);
    auto* proto_off   = proto_menu->addAction("Off");
    auto* proto_uart  = proto_menu->addAction("UART");
    auto* proto_i2c   = proto_menu->addAction("I2C");
    auto* proto_can   = proto_menu->addAction("CAN");
    auto* proto_spi   = proto_menu->addAction("SPI");
    for (auto* a : {proto_off, proto_uart, proto_i2c, proto_can, proto_spi}) {
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
    connect(proto_can,  &QAction::triggered, this, [selectProtocol]{ selectProtocol("CAN"); });
    connect(proto_spi,  &QAction::triggered, this, [selectProtocol]{ selectProtocol("SPI"); });
    connect(proto_btn, &QPushButton::clicked, proto_btn, [proto_menu, proto_btn]() {
        proto_menu->exec(proto_btn->mapToGlobal(QPoint(0, proto_btn->height())));
    });

    // Hardware connection toggle
    tb->addSeparator();
    auto* act_hw = tb->addAction("Connect Device");
    act_hw->setToolTip("Connect to an eScope device over USB");
    act_hw->setCheckable(true);
    connect(act_hw, &QAction::toggled, this, [this, act_hw](bool checked) {
        if (checked) {
            auto stm = std::make_unique<escope::StmDataSource>();
            if (stm->enumerate().empty()) {
                act_hw->setChecked(false);
                setStatus("No eScope device found. Check the USB connection.");
                return;
            }
            source_ = std::move(stm);
            connectSource();
            /* Start acquisition immediately on the new source */
            session_->reset();
            applyStoreMask();
            source_->configure(*session_);
            source_->start(*session_);
            capturing_ = true;
            display_frame_ = 0;
            waveform_widget_->resetCaptureTime();
            act_stop_->setEnabled(true);
            act_start_->setEnabled(false);
            act_single_->setEnabled(true);
            act_hw->setText("Disconnect Device");
            setStatus("Connected to eScope device");
        } else {
            if (source_) { source_->stop(); source_->close(); source_.reset(); }
            capturing_ = false;
            act_start_->setEnabled(false);
            act_single_->setEnabled(false);
            act_stop_->setEnabled(false);
            act_hw->setText("Connect Device");
            setStatus("No device connected. Connect a device to start capturing.");
        }
    });
}

// A quiet, small-caps style title in place of the stock dock title bar.
static QLabel* dockHeader(const QString& text) {
    auto* l = new QLabel(text.toUpper());
    QFont f = theme::ui(8.0, QFont::DemiBold);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 1.2);
    l->setFont(f);
    l->setStyleSheet(QString("color:%1; padding:14px 16px 8px 16px; background:transparent;")
                         .arg(theme::css(theme::kTextMuted)));
    return l;
}

void MainWindow::setupDockWidgets() {
    channel_panel_ = new ChannelPanel(this);
    channel_dock_  = new QDockWidget("Channels", this);
    channel_dock_->setWidget(channel_panel_);
    channel_dock_->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);
    channel_dock_->setFeatures(QDockWidget::NoDockWidgetFeatures);
    channel_dock_->setTitleBarWidget(dockHeader("Channels"));
    addDockWidget(Qt::LeftDockWidgetArea, channel_dock_);

    protocol_panel_ = new ProtocolPanel(this);
    protocol_dock_  = new QDockWidget("Protocol", this);
    protocol_dock_->setWidget(protocol_panel_);
    protocol_dock_->setAllowedAreas(Qt::RightDockWidgetArea);
    // Fixed in place: no float/close/move buttons. It appears and disappears
    // via the toolbar's Protocol menu and is resized by dragging its edge.
    protocol_dock_->setFeatures(QDockWidget::NoDockWidgetFeatures);
    protocol_dock_->setTitleBarWidget(dockHeader("Protocol"));
    addDockWidget(Qt::RightDockWidgetArea, protocol_dock_);
    protocol_dock_->hide();   // shown only once a protocol is picked in the toolbar

    // Decoded protocol events are drawn on the waveform, on a lane under the channel
    // (S, P, hex bytes, A / N); the channels the protocol uses are shown automatically.
    connect(protocol_panel_, &ProtocolPanel::annotationsChanged, this,
        [this](int ch, std::shared_ptr<const std::vector<escope::DecodedEvent>> ev) {
            waveform_widget_->setAnnotations(ch, std::move(ev));
        });
    connect(protocol_panel_, &ProtocolPanel::annotationsCleared, this,
        [this]{ waveform_widget_->clearAnnotations(); });
    connect(protocol_panel_, &ProtocolPanel::pinRolesChanged, channel_panel_, &ChannelPanel::setRoles);
    connect(protocol_panel_, &ProtocolPanel::pinsChanged, this,
        [this](QVector<int> chans) {
            for (int ch : chans)
                if (ch >= 0 && ch < 8 && ch_actions_[ch] && !ch_actions_[ch]->isChecked())
                    ch_actions_[ch]->setChecked(true);      // also shows it via the existing toggle
        });
}

void MainWindow::setupStatusBar() {
    status_label_ = new QLabel();
    status_label_->setTextFormat(Qt::RichText);
    statusBar()->addWidget(status_label_, 1);

    busy_bar_ = new QProgressBar();
    busy_bar_->setRange(0, 1000);
    busy_bar_->setTextVisible(false);
    busy_bar_->setFixedSize(180, 6);
    busy_bar_->hide();
    busy_cancel_ = new QPushButton("Cancel");
    busy_cancel_->setFlat(true);
    busy_cancel_->hide();
    connect(busy_cancel_, &QPushButton::clicked, this, [this] {
        if (io_cancel_) io_cancel_->store(true);
        busy_cancel_->hide();
        setStatus("Cancelling\u2026");
    });
    statusBar()->addPermanentWidget(busy_bar_);
    statusBar()->addPermanentWidget(busy_cancel_);
    setStatus("No device connected. Connect a device to start capturing.");
}

void MainWindow::connectSource() {
    source_->set_data_callback([this]{
        QMetaObject::invokeMethod(this, &MainWindow::onNewData, Qt::QueuedConnection);
    });
    source_->set_trigger_callback([this](const escope::TriggerEvent& evt){
        Q_UNUSED(evt);
        QMetaObject::invokeMethod(this, [this]{
            bool single = session_->trigger().config().mode == escope::TriggerMode::Single;
            setStatus(single ? "Triggered. Single capture complete."
                                           : "Triggered");
            // Single mode: capture exactly one triggered burst, then stop.
            if (single && capturing_) onStopCapture();
        }, Qt::QueuedConnection);
    });
    source_->open();
}

void MainWindow::onStartCapture() {
    if (io_busy_) { setStatus("Wait for the file operation to finish, or cancel it."); return; }
    if (!source_) { setStatus("No device connected. Connect a device to start capturing."); return; }
    // Run always means continuous capture until Stop, regardless of
    // whatever mode a previous SINGLE press left behind -- otherwise
    // pressing Run after a single-shot capture would immediately arm
    // and auto-stop again on the very next trigger.
    escope::TriggerConfig cfg = session_->trigger().config();
    cfg.mode = escope::TriggerMode::Auto;
    session_->trigger().set_config(cfg);

    session_->reset();
    applyStoreMask();
    source_->configure(*session_);
    source_->start(*session_);
    capturing_     = true;
    display_frame_ = 0;
    waveform_widget_->resetCaptureTime();
    act_start_->setEnabled(false);
    act_stop_->setEnabled(true);
    setStatus("Capturing: 8 channels, 48 MS/s (live)");
}

void MainWindow::onStopCapture() {
    if (source_) source_->stop();
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
    setStatus("Stopped. Drag to zoom into an area, scroll to pan.");
}

void MainWindow::onNewData() {}

void MainWindow::onUpdateDisplay() {
    if (!session_) return;
    waveform_widget_->setSession(session_.get());
    // Repaint only when something changed (new data, a different session) plus a slow heartbeat for
    // the overlays; an idle window then costs nothing. Interactions repaint themselves.
    const uint64_t ver = session_->digital_buffer().version();
    if (ver != last_version_ || session_.get() != last_session_ || display_frame_ % 15 == 0) {
        waveform_widget_->update();
        last_version_ = ver;
        last_session_ = session_.get();
    }
    ++display_frame_;

    // Status bar
    if (capturing_ && display_frame_ % 10 == 0) {
        if (waveform_widget_->followLatest())
            setStatus("Capturing: 8 channels, 48 MS/s (live)");
        else
            setStatus("Capturing: 8 channels, 48 MS/s (view paused, press L to follow)");
    }

    // Panels at ~6 Hz
    if (display_frame_ % 5 == 0) {
        channel_panel_->updateFrom(*session_);
    }
    // Protocol decode is heavier: ~5 Hz (skipped when nothing changed)
    if (display_frame_ % 6 == 0) {
        const double t0 = waveform_widget_->timeOffset();
        protocol_panel_->setLive(capturing_ && waveform_widget_->followLatest());
        protocol_panel_->updateFrom(*session_, t0,
            t0 + waveform_widget_->timePerDiv() * WaveformWidget::HDIVS);
    }
}

void MainWindow::onTriggerSingle() {
    if (io_busy_) { setStatus("Wait for the file operation to finish, or cancel it."); return; }
    if (!source_) { setStatus("No device connected. Connect a device to start capturing."); return; }
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
        applyStoreMask();
        source_->configure(*session_);
        source_->start(*session_);
        capturing_     = true;
        display_frame_ = 0;
        waveform_widget_->resetCaptureTime();
        act_start_->setEnabled(false);
        act_stop_->setEnabled(true);
    }
    setStatus("Single: waiting to arm");

    static constexpr int SINGLE_SETTLE_MS = 1000;
    QTimer::singleShot(SINGLE_SETTLE_MS, this, [this]() {
        if (!capturing_ || !source_) return; // Stop was pressed during the settle delay
        escope::TriggerConfig cfg = session_->trigger().config();
        cfg.mode = escope::TriggerMode::Single;
        session_->trigger().set_config(cfg);
        source_->configure(*session_);          // device: wait for a real trigger
        setStatus("Single: armed, waiting for a trigger");
    });
}

void MainWindow::onTriggerAuto() {
    escope::TriggerConfig cfg = session_->trigger().config();
    cfg.mode = escope::TriggerMode::Auto;
    session_->trigger().set_config(cfg);
    if (source_) source_->configure(*session_);

    // AUTO also autosets T/div from the signal itself, like a scope's
    // "Autoset" -- measure the visible channel's period from recent edges
    // and fit a few cycles across the screen.
    if (waveform_widget_) {
        if (waveform_widget_->autoScaleTimeDiv())
            setStatus("Auto: time base scaled to the signal");
        else
            setStatus("Auto: not enough signal to measure yet");
    }
}

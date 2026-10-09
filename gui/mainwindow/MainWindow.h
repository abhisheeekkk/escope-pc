#pragma once
#include <atomic>

#include <QMainWindow>
#include <QVariantAnimation>
#include <QCloseEvent>
#include <memory>
#include "acquisition/ProtocolTrigger.h"

class QAction;
class QComboBox;
class QPushButton;
class QMenu;
class QDockWidget;
class QLabel;
class QDialog;
class QProgressBar;
class QTimer;

namespace escope {
    class CaptureSession;
    class IDataSource;
}

class WaveformWidget;
class ChannelPanel;
class ProtocolPanel;
class TriggerDialog;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* e) override;

private slots:
    void onStartCapture();
    void onStopCapture();
    void onNewData();
    void onUpdateDisplay();
    void onTriggerSingle();
    void onTriggerAuto();

private:
    void setupMenuBar();
    void setupToolBar();
    void setupDockWidgets();
    void setupStatusBar();
    void connectSource();
    void setStatus(const QString& text);   ///< status bar message with a state dot
    void showControlsSheet();
    void showTriggerDialog();
    void applyProtocolTrigger(const escope::ProtocolTriggerConfig& cfg, bool announce = true);
    void applyStoreMask();   ///< record only the channels that are shown

    // Background file work: a thin progress bar with a cancel button in the status bar
    void beginBusy(const QString& text);
    void setBusyProgress(double fraction);
    void endBusy();
    void openSession(const QString& dir);
    void saveSession(const QString& dir);

    std::unique_ptr<escope::CaptureSession>  session_;
    std::unique_ptr<escope::IDataSource>     source_;

    WaveformWidget*    waveform_widget_  = nullptr;
    ChannelPanel*      channel_panel_    = nullptr;
    ProtocolPanel*     protocol_panel_   = nullptr;

    QDockWidget*       channel_dock_     = nullptr;
    QDockWidget*       protocol_dock_    = nullptr;

    QAction*           act_start_        = nullptr;
    QAction*           act_stop_         = nullptr;
    QAction*           act_single_       = nullptr;
    QAction*           act_auto_         = nullptr;
    QAction*           act_ptrig_        = nullptr;   ///< protocol trigger; lit while armed
    TriggerDialog*     trigger_dlg_      = nullptr;
    std::shared_ptr<escope::ProtocolTrigger> proto_trigger_;
    QString            last_trig_status_;
    QString            status_text_;            ///< what the status bar says now
    QVariantAnimation* dot_pulse_ = nullptr;    ///< breathing dot while a trigger waits
    double             dot_alpha_ = 1.0;
    void               renderStatus();

    QAction*           ch_actions_[8]    = {};   ///< the toolbar Select menu entries D0-D7
    QLabel*            status_label_     = nullptr;
    QDialog*           controls_dlg_     = nullptr;
    QTimer*            display_timer_    = nullptr;

    bool  capturing_     = false;
    uint64_t                 last_version_ = ~0ULL;   ///< buffer version at the last repaint
    const void*              last_session_ = nullptr;
    bool  io_busy_       = false;   ///< a session is being saved or opened in the background
    std::shared_ptr<std::atomic<bool>> io_cancel_;   ///< set by the Cancel button, polled by the worker
    QProgressBar* busy_bar_    = nullptr;
    QPushButton*  busy_cancel_ = nullptr;
    int   display_frame_ = 0;   // proper member, not a static local
    bool  auto_scaled_   = false; // track whether we've auto-scaled this capture
};

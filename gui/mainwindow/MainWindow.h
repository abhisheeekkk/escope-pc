#pragma once

#include <QMainWindow>
#include <memory>

class QAction;
class QComboBox;
class QPushButton;
class QMenu;
class QDockWidget;
class QLabel;
class QDialog;
class QTimer;

namespace escope {
    class CaptureSession;
    class IDataSource;
}

class WaveformWidget;
class ChannelPanel;
class ProtocolPanel;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

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

    QAction*           ch_actions_[8]    = {};   ///< the toolbar Select menu entries D0-D7
    QLabel*            status_label_     = nullptr;
    QDialog*           controls_dlg_     = nullptr;
    QTimer*            display_timer_    = nullptr;

    bool  capturing_     = false;
    bool  io_busy_       = false;   ///< a session is being saved or opened in the background
    int   display_frame_ = 0;   // proper member, not a static local
    bool  auto_scaled_   = false; // track whether we've auto-scaled this capture
};

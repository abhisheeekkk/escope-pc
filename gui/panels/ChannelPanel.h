#pragma once
#include <QWidget>
#include <QScrollArea>

namespace escope { class CaptureSession; }

/// Shows all 16 digital channels with their names, current state, and frequency.
class ChannelPanel : public QWidget {
    Q_OBJECT
public:
    explicit ChannelPanel(QWidget* parent = nullptr);
    void updateFrom(const escope::CaptureSession& session);
private:
    struct Row { class QLabel* name; class QLabel* freq; class QLabel* level; };
    Row rows_[16];
};

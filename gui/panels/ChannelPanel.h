#pragma once
#include <QWidget>
#include <QStringList>
#include <array>

namespace escope { class CaptureSession; }

/// Lists the eight capture channels with their colour, protocol role, frequency and current level.
class ChannelPanel : public QWidget {
    Q_OBJECT
public:
    explicit ChannelPanel(QWidget* parent = nullptr);
    void updateFrom(const escope::CaptureSession& session);
    /// Protocol role of each channel (CLK, MOSI, SDA...), empty when none. Set by the decoder panel.
    void setRoles(const QStringList& roles);

    /// Whether a channel is drawn on the waveform; hidden channels are dimmed.
    void setChannelVisible(int ch, bool visible);

    QSize sizeHint() const override { return {260, 360}; }

signals:
    /// The user clicked a row to show or hide that channel.
    void channelClicked(int channel);

protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    struct Row { QString name; QString freq = "N/A"; bool level = false; bool known = false; };
    std::array<Row, 8> rows_;
    QStringList roles_;
    std::array<bool, 8> visible_ = {true, false, false, false, false, false, false, false};
    int hover_ = -1;
    int rowAt(const QPoint& p) const;
};

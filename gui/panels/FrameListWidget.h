#pragma once
#include <QWidget>
#include <memory>
#include <vector>
#include <QKeyEvent>
#include "decoders/base/IDecoder.h"

class QLineEdit;
class QPushButton;
class QTableView;
class QLabel;
class FrameModel;

/// Searchable table of every decoded frame. Typing in the filter box narrows the list (all words must
/// match the frame text, so  0x50 nack  finds NACKs from address 0x50); Errors limits it to damaged
/// frames. Selecting a row asks the waveform to show that frame.
class FrameListWidget : public QWidget {
    Q_OBJECT
public:
    explicit FrameListWidget(QWidget* parent = nullptr);

    /// Replace the list. @p events must be sorted by start time. When @p follow_tail is set and the
    /// user has not picked a row, the view stays on the newest frame.
    void setEvents(std::shared_ptr<const std::vector<escope::DecodedEvent>> events, bool follow_tail);
    void clear();

    /// Frames shown / frames in total / errors in total.
    std::size_t shownCount() const;
    std::size_t totalCount() const;
    std::size_t errorCount() const { return errors_; }

signals:
    /// The user picked a frame: show [start_ns, end_ns] in the waveform.
    void frameActivated(double start_ns, double end_ns);
    /// The picked frame is gone (filter cleared, Esc): remove its marker from the waveform.
    void selectionCleared();
    void countsChanged();

private:
    void applyFilter() { rebuild(false, true); }
    void rebuild(bool follow_tail, bool filter_changed);
    void keyPressEvent(QKeyEvent* e) override;
    void updateCount();

    std::shared_ptr<const std::vector<escope::DecodedEvent>> events_;
    FrameModel*  model_   = nullptr;
    QTableView*  table_   = nullptr;
    QLineEdit*   filter_  = nullptr;
    QPushButton* errors_btn_ = nullptr;
    QLabel*      count_   = nullptr;
    std::size_t  errors_  = 0;
    bool         user_picked_ = false;
    bool         was_filtering_ = false;
    double       anchor_t_ = -1;      ///< frame at the top of the list when a search began
};

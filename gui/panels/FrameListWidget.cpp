#include "panels/FrameListWidget.h"
#include "theme/Theme.h"

#include <QAbstractTableModel>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>

using escope::DecodedEvent;

namespace {
QString formatTime(double ns) {
    if (ns >= 1e9) return QString::number(ns / 1e9, 'f', 6) + " s";
    if (ns >= 1e6) return QString::number(ns / 1e6, 'f', 4) + " ms";
    return QString::number(ns / 1e3, 'f', 2) + " µs";
}
const char* typeName(DecodedEvent::Type t) {
    switch (t) {
    case DecodedEvent::Type::Data:       return "Data";
    case DecodedEvent::Type::Address:    return "Address";
    case DecodedEvent::Type::Control:    return "Control";
    case DecodedEvent::Type::Error:      return "Error";
    case DecodedEvent::Type::Annotation: return "Note";
    }
    return "";
}
}

/// Rows are indices into the shared event list, so filtering never copies a frame.
class FrameModel : public QAbstractTableModel {
public:
    using Events = std::shared_ptr<const std::vector<DecodedEvent>>;

    int rowCount(const QModelIndex& p = {}) const override { return p.isValid() ? 0 : int(rows_.size()); }
    int columnCount(const QModelIndex& p = {}) const override { return p.isValid() ? 0 : 3; }

    QVariant headerData(int s, Qt::Orientation o, int role) const override {
        if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
        static const char* h[] = {"Time", "Type", "Frame"};
        return h[s];
    }
    QVariant data(const QModelIndex& ix, int role) const override {
        if (!ix.isValid() || ix.row() >= int(rows_.size()) || !events_) return {};
        const DecodedEvent& e = (*events_)[rows_[ix.row()]];
        if (role == Qt::DisplayRole) {
            switch (ix.column()) {
            case 0: return formatTime(e.start_ns);
            case 1: return typeName(e.type);
            default: return QString::fromStdString(e.label);
            }
        }
        if (role == Qt::ToolTipRole) return QString::fromStdString(e.detail);
        if (role == Qt::ForegroundRole && e.is_error) return theme::kRed;
        if (role == Qt::ForegroundRole && ix.column() == 0) return theme::kTextMuted;
        if (role == Qt::FontRole && ix.column() != 1) return theme::mono(9.5);
        return {};
    }

    const DecodedEvent* at(int row) const {
        return (events_ && row >= 0 && row < int(rows_.size())) ? &(*events_)[rows_[row]] : nullptr;
    }
    std::size_t total() const { return events_ ? events_->size() : 0; }

    /// Keep the frames whose text contains every word of @p words (already lower case).
    void setData(Events ev, const QStringList& words, bool errors_only) {
        beginResetModel();
        events_ = std::move(ev);
        rows_.clear();
        if (events_) {
            rows_.reserve(events_->size());
            for (std::size_t i = 0; i < events_->size(); ++i) {
                const DecodedEvent& e = (*events_)[i];
                if (errors_only && !e.is_error) continue;
                if (!words.isEmpty()) {
                    const QString hay = (QString::fromStdString(e.label) + ' ' + QString::fromStdString(e.detail) + ' ' +
                                         typeName(e.type)).toLower();
                    bool ok = true;
                    for (const QString& w : words) if (!hay.contains(w)) { ok = false; break; }
                    if (!ok) continue;
                }
                rows_.push_back(uint32_t(i));
            }
        }
        endResetModel();
    }

private:
    Events                events_;
    std::vector<uint32_t> rows_;
};

FrameListWidget::FrameListWidget(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(6);

    auto* bar = new QHBoxLayout;
    bar->setSpacing(6);
    filter_ = new QLineEdit(this);
    filter_->setPlaceholderText("Filter frames, e.g. 0x50 nack");
    filter_->setClearButtonEnabled(true);
    errors_btn_ = new QPushButton("Errors", this);
    errors_btn_->setCheckable(true);
    errors_btn_->setToolTip("Show only damaged frames");
    bar->addWidget(filter_, 1);
    bar->addWidget(errors_btn_);
    root->addLayout(bar);

    model_ = new FrameModel;
    model_->setParent(this);
    table_ = new QTableView(this);
    table_->setModel(model_);
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setShowGrid(false);
    table_->setAlternatingRowColors(false);
    table_->verticalHeader()->hide();
    table_->verticalHeader()->setDefaultSectionSize(22);
    table_->horizontalHeader()->setStretchLastSection(true);
    table_->horizontalHeader()->setHighlightSections(false);
    table_->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    table_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    table_->setColumnWidth(0, 104);
    table_->setColumnWidth(1, 64);
    table_->setFocusPolicy(Qt::StrongFocus);
    root->addWidget(table_, 1);

    count_ = new QLabel(this);
    count_->setObjectName("caption");
    root->addWidget(count_);

    // Typing re-filters after a short pause, so a long list never stutters
    auto* debounce = new QTimer(this);
    debounce->setSingleShot(true);
    debounce->setInterval(120);
    connect(debounce, &QTimer::timeout, this, &FrameListWidget::applyFilter);
    connect(filter_, &QLineEdit::textChanged, debounce, qOverload<>(&QTimer::start));
    connect(errors_btn_, &QPushButton::toggled, this, &FrameListWidget::applyFilter);

    connect(table_->selectionModel(), &QItemSelectionModel::currentRowChanged, this,
            [this](const QModelIndex& cur, const QModelIndex&) {
                if (!cur.isValid()) return;
                if (const DecodedEvent* e = model_->at(cur.row())) {
                    user_picked_ = true;
                    emit frameActivated(e->start_ns, e->end_ns);
                }
            });
}

void FrameListWidget::setEvents(std::shared_ptr<const std::vector<DecodedEvent>> events, bool follow_tail) {
    errors_ = 0;
    if (events) for (const auto& e : *events) errors_ += e.is_error;
    events_ = std::move(events);
    rebuild(follow_tail, false);
}

void FrameListWidget::rebuild(bool follow_tail, bool filter_changed) {
    // Remember where the user was: the picked frame, and the frame at the top of the view. Both are
    // kept by time, so they survive the list being re-filtered or refreshed.
    double pick_t = -1, top_t = -1;
    if (user_picked_ && table_->currentIndex().isValid())
        if (const DecodedEvent* e = model_->at(table_->currentIndex().row())) pick_t = e->start_ns;
    if (const int r = table_->rowAt(0); r >= 0)
        if (const DecodedEvent* e = model_->at(r)) top_t = e->start_ns;
    const bool at_end = !filter_changed &&
        table_->verticalScrollBar()->value() >= table_->verticalScrollBar()->maximum() - 2;

    // With no filter left, nothing is searched for any more: the marker goes away and the list
    // returns to where it was before the search, instead of staying on the old match.
    const QStringList words = filter_->text().toLower().split(' ', Qt::SkipEmptyParts);
    const bool filtering = !words.isEmpty() || errors_btn_->isChecked();
    const bool drop_pick = filter_changed && !filtering;

    // Entering a search remembers the place in the full list; leaving it goes straight back there
    if (filter_changed && filtering && !was_filtering_) anchor_t_ = top_t;
    const double back_to = (drop_pick && anchor_t_ >= 0) ? anchor_t_ : -1;
    if (filter_changed) was_filtering_ = filtering;
    if (drop_pick) anchor_t_ = -1;

    int pick_row = -1;
    {
        const QSignalBlocker block(table_->selectionModel());     // restoring must not re-jump the waveform
        model_->setData(events_, words, errors_btn_->isChecked());
        if (pick_t >= 0 && !drop_pick) {
            for (int r = 0; r < model_->rowCount(); ++r)
                if (model_->at(r)->start_ns == pick_t) {
                    pick_row = r;
                    table_->selectionModel()->setCurrentIndex(model_->index(r, 0),
                        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                    break;
                }
        }
    }
    if (pick_t >= 0 && pick_row < 0 && !drop_pick) {                            // the picked frame no longer matches
        user_picked_ = false;
        emit selectionCleared();
    }

    table_->doItemsLayout();          // refresh the scroll range now, or scrollTo clamps to the old one

    // First frame at or after a time, in the new list
    auto row_at = [this](double t) {
        int lo = 0, hi = model_->rowCount();
        while (lo < hi) { int m = (lo + hi) / 2; if (model_->at(m)->start_ns < t) lo = m + 1; else hi = m; }
        return std::min(lo, std::max(0, model_->rowCount() - 1));
    };
    if (model_->rowCount() > 0) {
        if (pick_row >= 0 && filter_changed)  table_->scrollTo(model_->index(pick_row, 0), QAbstractItemView::PositionAtCenter);
        else if (back_to >= 0)                table_->scrollTo(model_->index(row_at(back_to), 0), QAbstractItemView::PositionAtTop);
        else if (follow_tail || at_end)       table_->scrollToBottom();
        else if (top_t >= 0)                  table_->scrollTo(model_->index(row_at(top_t), 0), QAbstractItemView::PositionAtTop);
    }
    if (drop_pick && pick_t >= 0) { user_picked_ = false; table_->clearSelection(); emit selectionCleared(); }
    updateCount();
    emit countsChanged();
}

void FrameListWidget::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Escape && user_picked_) {            // Esc puts the marker away
        user_picked_ = false;
        table_->clearSelection();
        emit selectionCleared();
        e->accept();
        return;
    }
    QWidget::keyPressEvent(e);
}

void FrameListWidget::clear() {
    if (user_picked_) emit selectionCleared();
    user_picked_ = false;
    events_.reset();
    errors_ = 0;
    rebuild(false, false);
}

std::size_t FrameListWidget::shownCount() const { return std::size_t(model_->rowCount()); }
std::size_t FrameListWidget::totalCount() const { return model_->total(); }

void FrameListWidget::updateCount() {
    if (totalCount() == 0) { count_->clear(); return; }
    QString s = shownCount() == totalCount() ? QString("%1 frames").arg(totalCount())
                                             : QString("%1 of %2 frames").arg(shownCount()).arg(totalCount());
    if (errors_) s += QString(", %1 errors").arg(errors_);
    count_->setText(s);
}

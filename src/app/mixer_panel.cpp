#include "mixer_panel.h"

#include <cmath>

#include <QDateTime>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QScrollArea>
#include <QTimer>
#include <QVBoxLayout>

#include "timeline_model.h"
#include "ui_theme.h"
#include "ui_widgets.h"

// ui_theme.h tokens live in fc::ui; the panel addresses them as ui::...
using namespace fc;

namespace {

constexpr int kMeterHeight = 14;     // the compact horizontal meter strip
constexpr double kSilentDb = -60.0;  // meter floor / idle level
constexpr double kAmberAtDb = -12.0; // green -> amber transition point
constexpr double kRedAtDb = -3.0;    // amber -> red transition point
constexpr int kHoldDecayMs = 100;    // 10 Hz hold-decay tick
constexpr int kHoldIdleMs = 1500;    // decay the hold after 1.5 s idle
constexpr double kHoldDecayDb = 0.5; // dB per tick once idle

// DPR-marked glyph pixmap (#206) for the panel header (#166/#116).
QPixmap glyphPixmap(const QString &name, const QColor &color, int logicalSize, qreal dpr) {
    return icons::makeIcon(name, color, logicalSize, dpr)
        .pixmap(qRound(logicalSize * dpr), qRound(logicalSize * dpr));
}

// #157: the mixer's hairline slider language - 2px kLine groove with the
// accent fill on the filled side and a light round handle. Same colors
// as the app-wide sheet (and #148's quick-mode position slider), one
// notch tighter; the local sheet wins where it is set.
QString hairlineSliderSheet() {
    return QStringLiteral(
               "QSlider::groove:horizontal { height: 2px; background: %1; border-radius: 1px; }"
               "QSlider::sub-page:horizontal { background: %2; border-radius: 1px; }"
               "QSlider::handle:horizontal { width: 14px; height: 14px; margin: -6px 0;"
               " border-radius: 7px; background: %3; }"
               "QSlider::handle:horizontal:hover { background: %4; }"
               "QSlider::groove:vertical { width: 2px; background: %1; border-radius: 1px; }"
               "QSlider::sub-page:vertical { background: %2; border-radius: 1px; }"
               "QSlider::handle:vertical { height: 14px; width: 14px; margin: 0 -6px;"
               " border-radius: 7px; background: %3; }")
        .arg(ui::color(ui::kLine).name())
        .arg(ui::color(ui::kAccent).name())
        .arg(ui::color(ui::kText).name())
        .arg(ui::color(ui::kAccentBright).name());
}

QString dbText(int db) {
    return db <= -60 ? QObject::tr("mute") : QString("%1 dB").arg(db);
}

QString panText(int pan) {
    if (pan == 0) {
        return QObject::tr("Pan C");
    }
    return pan < 0 ? QString("L%1").arg(std::abs(pan)) : QString("R%1").arg(std::abs(pan));
}

} // namespace

// ---------------------------------------------------------------------------
// LevelMeter (suggestion #45): a compact horizontal dBFS meter painted in
// paintEvent only - green -> amber at -12 dBFS -> red at -3 dBFS over a
// -60..0 scale, a current-level bar, a peak-hold line and a numeric
// dBFS label on the right. Clicking the meter resets its hold. No timers
// of its own: the panel's hold-decay timer calls decayTick().
// ---------------------------------------------------------------------------
class LevelMeter : public QWidget {
public:
    explicit LevelMeter(QWidget *parent = nullptr) : QWidget(parent) {
        setFixedHeight(kMeterHeight);
        setMinimumWidth(48);
        setMouseTracking(false);
    }

    void setLabel(const QString &text) { label_ = text; }

    void setLevel(double dbfs) {
        level_ = clampDb(dbfs);
        if (level_ > hold_) {
            hold_ = level_; // the hold rides every new peak
            lastRiseMs_ = nowMs();
        }
        update();
    }

    // The panel's 10 Hz tick: once the level has been idle for 1.5 s the
    // hold falls back toward the current level (classic peak-hold reset).
    void decayTick() {
        const qint64 now = nowMs();
        if (now - lastRiseMs_ < kHoldIdleMs || hold_ <= level_) {
            return;
        }
        hold_ = std::max(level_, hold_ - kHoldDecayDb);
        update();
    }

    // True when the meter shows silence with the hold decayed to the
    // floor - the panel stops its decay timer when EVERY meter settles.
    bool isSettled() const { return level_ <= kSilentDb + 0.01 && hold_ <= kSilentDb + 0.01; }

    void resetHold() {
        hold_ = level_;
        update();
    }

protected:
    void paintEvent(QPaintEvent * /*event*/) override {
        QPainter p(this);

        constexpr int kLabelW = 40;
        constexpr int kGap = 4;
        const QRect barRect(0, 0, std::max(4, width() - kLabelW - kGap), height());
        const QRect labelRect(barRect.right() + 1 + kGap, 0, kLabelW, height());

        // Dim full-scale track, then the bright bar up to the level, so
        // the scale (and its color stops) stays readable at any level.
        paintScale(&p, barRect, 0.35);
        if (level_ > kSilentDb) {
            QRect levelRect = barRect;
            levelRect.setWidth(std::max(1, static_cast<int>(barRect.width() * fraction(level_))));
            paintScale(&p, levelRect, 1.0);
        }
        if (hold_ > kSilentDb) {
            const int hx = barRect.left() + static_cast<int>(barRect.width() * fraction(hold_)) - 1;
            p.fillRect(hx, 0, 2, barRect.height(), ui::color(ui::kText));
        }

        p.setPen(ui::color(ui::kTextDim));
        p.drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter, label_);
    }

    void mousePressEvent(QMouseEvent *event) override {
        // Click anywhere on the meter: clear the peak hold.
        if (event->button() == Qt::LeftButton) {
            resetHold();
        }
        QWidget::mousePressEvent(event);
    }

private:
    static double clampDb(double db) { return std::min(0.0, std::max(kSilentDb, db)); }
    static double fraction(double db) { return (db - kSilentDb) / -kSilentDb; }
    static qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }

    // One colored vertical strip per pixel column: kSuccess below
    // -12 dBFS blending to kWarning there, kDanger from -3 dBFS up.
    void paintScale(QPainter *p, const QRect &rect, double alphaFactor) {
        for (int x = rect.left(); x <= rect.right(); ++x) {
            const double t = static_cast<double>(x - rect.left()) /
                             std::max(1, rect.width() - 1); // 0..1 across the scale
            const double db = kSilentDb + t * -kSilentDb;
            QColor c;
            if (db <= kAmberAtDb) {
                c = ui::mix(ui::color(ui::kSuccess), ui::color(ui::kWarning),
                            t * (0.0 - kAmberAtDb) / -kSilentDb);
            } else if (db <= kRedAtDb) {
                c = ui::mix(ui::color(ui::kWarning), ui::color(ui::kDanger),
                            (db - kAmberAtDb) / (kRedAtDb - kAmberAtDb));
            } else {
                c = ui::color(ui::kDanger);
            }
            c.setAlpha(static_cast<int>(255 * alphaFactor));
            p->fillRect(x, rect.top(), 1, rect.height(), c);
        }
    }

    double level_ = kSilentDb;
    double hold_ = kSilentDb;
    qint64 lastRiseMs_ = 0;
    QString label_ = QStringLiteral("-inf");
};

// Tiny indirection so LevelMeter needs no QTime/QDateTime include of its
// own in this file - std::chrono monotonic-ish wall clock in ms.
struct QDateTimeWrapper {
    static long long currentMSecs() {
        using namespace std::chrono;
        return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    }
};

MixerPanel::MixerPanel(QWidget *parent) : QWidget(parent) {
    // #164: 12px content margins; the header row (#166) sits above the
    // strips + master row.
    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(12, 12, 12, 12);
    outerLayout->setSpacing(8);

    // Panel header (#166): 40px row - 16px waveform glyph in kTextDim +
    // 13px semibold title in kText. No count caption: the strip count
    // only exists after refreshFromModel, so the header stays static.
    auto *header = new QWidget(this);
    header->setFixedHeight(40);
    auto *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(8);
    auto *headerGlyph = new QLabel(header);
    headerGlyph->setPixmap(
        glyphPixmap("waveform", ui::color(ui::kTextDim), 16, devicePixelRatioF()));
    auto *headerTitle = new QLabel(tr("Audio mixer"), header);
    headerTitle->setStyleSheet(QStringLiteral("font-size:13px;font-weight:600;color:%1;")
                                   .arg(ui::color(ui::kText).name()));
    headerLayout->addWidget(headerGlyph);
    headerLayout->addWidget(headerTitle);
    headerLayout->addStretch(1);
    outerLayout->addWidget(header);

    auto *layout = new QHBoxLayout;
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    // Strips live in a scroll area so an unusual number of audio lanes
    // never breaks the layout.
    auto *scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    stripsHost_ = new QWidget(scroll);
    auto *stripsLayout = new QHBoxLayout(stripsHost_);
    stripsLayout->setContentsMargins(0, 0, 0, 0);
    stripsLayout->setSpacing(12);
    stripsLayout->addStretch(1);
    scroll->setWidget(stripsHost_);
    scroll->setMinimumHeight(210);
    layout->addWidget(scroll, 1);

    // Master strip (#46): RIGHTMOST (last in the outer layout) and
    // visually distinct - 1 px accent border over a raised surface,
    // rounded to the control radius (8px).
    auto *master = new QWidget(this);
    master->setObjectName(QStringLiteral("fcMasterStrip"));
    master->setAttribute(Qt::WA_StyledBackground, true);
    master->setStyleSheet(QStringLiteral("QWidget#fcMasterStrip{background:%1;"
                                         "border:1px solid %2;border-radius:%3px;}")
                              .arg(ui::color(ui::kSurface3).name(), ui::color(ui::kAccent).name())
                              .arg(ui::kRadiusControl));
    auto *masterLayout = new QVBoxLayout(master);
    masterLayout->setContentsMargins(6, 6, 6, 6);
    auto *masterTitle = new QLabel(tr("Master"), master);
    masterTitle->setAlignment(Qt::AlignCenter);
    masterFader_ = new QSlider(Qt::Vertical, master);
    masterFader_->setRange(-60, 6);
    masterFader_->setValue(0);
    masterFader_->setToolTip(tr("Master fader - double-click resets to 0 dB"));
    masterFader_->setAccessibleName(tr("Master fader"));
    // #157: hairline groove + accent fill; the 40px width is the
    // fader's grab strip (the vertical slider's hit area).
    masterFader_->setStyleSheet(hairlineSliderSheet());
    masterFader_->setFixedWidth(40);
    masterFader_->installEventFilter(this);
    masterMeter_ = new LevelMeter(master);
    masterMeter_->setToolTip(tr("Master output level (dBFS) - click to reset the peak hold"));
    masterMeter_->setAccessibleName(tr("Master level meter"));
    masterDbLabel_ = new QLabel(dbText(0), master);
    masterDbLabel_->setAlignment(Qt::AlignCenter);
    // #157: tabular figures - Qt 5.12 exposes no font-feature API
    // (QFont::setFeature is Qt 6.7+); Segoe UI's default figures are
    // tabular and the fixed-width centered label keeps the strip from
    // jittering as the value changes.
    masterDbLabel_->setMinimumWidth(48);
    masterLayout->addWidget(masterTitle);
    masterLayout->addWidget(masterFader_, 1);
    masterLayout->addWidget(masterMeter_);
    masterLayout->addWidget(masterDbLabel_);
    master->setFixedWidth(84);
    layout->addWidget(master); // LAST: the master strip is the rightmost
    outerLayout->addLayout(layout, 1);

    connect(masterFader_, &QSlider::valueChanged, this, [this](int db) {
        masterDbLabel_->setText(dbText(db));
        if (model_) {
            model_->setMasterGainDb(double(db));
            emit mixerChanged();
        }
    });

    // The single hold-decay timer (10 Hz) - started when levels go live,
    // stopped when every meter has settled back to silence.
    holdTimer_ = new QTimer(this);
    holdTimer_->setInterval(kHoldDecayMs);
    connect(holdTimer_, &QTimer::timeout, this, [this] {
        for (const Strip &s : strips_) {
            if (s.meter) {
                s.meter->decayTick();
            }
        }
        if (masterMeter_) {
            masterMeter_->decayTick();
        }
        bool allSettled = true;
        for (const Strip &s : strips_) {
            if (s.meter && !s.meter->isSettled()) {
                allSettled = false;
                break;
            }
        }
        if (masterMeter_ && !masterMeter_->isSettled()) {
            allSettled = false;
        }
        if (allSettled) {
            holdTimer_->stop();
        }
    });
}

bool MixerPanel::eventFilter(QObject *watched, QEvent *event) {
    // #46: double-click any fader -> 0 dB. setValue() runs the SAME
    // valueChanged path a drag uses (model write + mixerChanged); a
    // fader already at 0 dB stays untouched (setValue is a no-op there).
    if (event->type() == QEvent::MouseButtonDblClick) {
        auto *fader = qobject_cast<QSlider *>(watched);
        if (fader && static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton) {
            fader->setValue(0);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void MixerPanel::buildStrip(const QString &name, bool withPan, Strip &out, QWidget * /*host*/) {
    auto *strip = new QWidget(stripsHost_);
    strip->setFixedWidth(84);
    auto *stripLayout = new QVBoxLayout(strip);
    stripLayout->setContentsMargins(6, 6, 6, 6);

    auto *title = new QLabel(name, strip);
    title->setAlignment(Qt::AlignCenter);

    out.fader = new QSlider(Qt::Vertical, strip);
    out.fader->setRange(-60, 6);
    out.fader->setValue(0);
    out.fader->setToolTip(tr("%1 fader - double-click resets to 0 dB").arg(name));
    out.fader->setAccessibleName(tr("%1 fader").arg(name));
    // #157: hairline groove + accent fill; the 40px width is the
    // fader's grab strip (the vertical slider's hit area).
    out.fader->setStyleSheet(hairlineSliderSheet());
    out.fader->setFixedWidth(40);
    out.fader->installEventFilter(this);

    out.dbLabel = new QLabel(dbText(0), strip);
    out.dbLabel->setAlignment(Qt::AlignCenter);
    // #157: tabular figures via a fixed-width centered label (see the
    // master strip comment - Qt 5.12 exposes no font-feature API).
    out.dbLabel->setMinimumWidth(48);

    out.meter = new LevelMeter(strip);
    out.meter->setAccessibleName(tr("%1 level meter").arg(name));
    out.meter->setToolTip(tr("%1 level (dBFS) - click to reset the peak hold").arg(name));

    stripLayout->addWidget(title);
    stripLayout->addWidget(out.fader, 1);
    stripLayout->addWidget(out.meter); // #45: compact meter under the fader
    stripLayout->addWidget(out.dbLabel);

    if (withPan) {
        out.pan = new QSlider(Qt::Horizontal, strip);
        out.pan->setRange(-100, 100);
        out.pan->setValue(0);
        // #157: the same hairline language as the faders so the strip's
        // two sliders read as one family.
        out.pan->setStyleSheet(hairlineSliderSheet());
        out.panLabel = new QLabel(panText(0), strip);
        out.panLabel->setAlignment(Qt::AlignCenter);
        stripLayout->addWidget(out.pan);
        stripLayout->addWidget(out.panLabel);
    }

    // Mute / solo pair (checkable, mutually aware at the model level -
    // the solo LOGIC lives in the flatten, not the buttons). #157: both
    // restyle to 24px circles - mute stays neutral (brightens when
    // checked), solo keeps its success color and gains the accent
    // border when active. The repo carries no icon assets, so the
    // letter glyphs ARE the icons.
    auto *row = new QWidget(strip);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(4);
    out.mute = new QToolButton(row);
    out.mute->setText(tr("M"));
    out.mute->setCheckable(true);
    out.mute->setToolTip(tr("Mute this track"));
    out.mute->setAccessibleName(tr("Mute %1").arg(name));
    out.mute->setStyleSheet(
        QStringLiteral("QToolButton{background:%1;border:1px solid %2;border-radius:12px;"
                       "padding:0;color:%3;font-size:11px;}"
                       "QToolButton:hover{background:%4;}"
                       "QToolButton:checked{background:%4;color:%5;border-color:%3;}")
            .arg(ui::color(ui::kSurface3).name(), ui::color(ui::kLine).name(),
                 ui::color(ui::kTextDim).name(), ui::color(ui::kSurfaceHover).name(),
                 ui::color(ui::kText).name()));
    out.solo = new QToolButton(row);
    out.solo->setText(tr("S"));
    out.solo->setCheckable(true);
    out.solo->setToolTip(tr("Solo this track (other audio tracks fall silent)"));
    out.solo->setAccessibleName(tr("Solo %1").arg(name));
    out.solo->setStyleSheet(
        QStringLiteral("QToolButton{background:%1;border:1px solid %2;border-radius:12px;"
                       "padding:0;color:%3;font-size:11px;}"
                       "QToolButton:hover{background:%4;}"
                       "QToolButton:checked{background:%5;color:%6;border-color:%6;}")
            .arg(ui::color(ui::kSurface3).name(), ui::color(ui::kLine).name(),
                 ui::color(ui::kTextDim).name(), ui::color(ui::kSurfaceHover).name(),
                 ui::tint(ui::kSuccess).name(), ui::color(ui::kSuccess).name()));
    for (QToolButton *cell : {out.mute, out.solo}) {
        cell->setFixedSize(24, 24); // #157: 24px circles
        rowLayout->addWidget(cell, 1, Qt::AlignHCenter);
    }
    stripLayout->addWidget(row);

    const int trackIndex = out.trackIndex;
    connect(out.fader, &QSlider::valueChanged, this, [this, out, trackIndex](int db) {
        out.dbLabel->setText(dbText(db));
        if (!model_) {
            return;
        }
        const fc::Track *track = model_->trackAt(trackIndex);
        if (!track) {
            return;
        }
        model_->setTrackAudio(trackIndex, double(db), track->pan);
        emit mixerChanged();
    });
    if (out.pan) {
        connect(out.pan, &QSlider::valueChanged, this, [this, out, trackIndex](int pan) {
            out.panLabel->setText(panText(pan));
            if (!model_) {
                return;
            }
            const fc::Track *track = model_->trackAt(trackIndex);
            if (!track) {
                return;
            }
            model_->setTrackAudio(trackIndex, track->gainDb, double(pan) / 100.0);
            emit mixerChanged();
        });
    }
    connect(out.mute, &QToolButton::toggled, this, [this, trackIndex](bool muted) {
        if (!model_) {
            return;
        }
        const fc::Track *track = model_->trackAt(trackIndex);
        if (!track) {
            return;
        }
        model_->setTrackState(trackIndex, track->locked, muted, track->solo);
        emit mixerChanged();
    });
    connect(out.solo, &QToolButton::toggled, this, [this, trackIndex](bool solo) {
        if (!model_) {
            return;
        }
        const fc::Track *track = model_->trackAt(trackIndex);
        if (!track) {
            return;
        }
        model_->setTrackState(trackIndex, track->locked, track->muted, solo);
        emit mixerChanged();
    });

    // Strips go before the trailing stretch (the layout keeps it last).
    if (auto *h = qobject_cast<QHBoxLayout *>(stripsHost_->layout())) {
        h->insertWidget(h->count() - 1, strip);
    }
}

void MixerPanel::refreshFromModel(const fc::TimelineModel *model) {
    model_ = const_cast<fc::TimelineModel *>(model);

    // Drop the old strips (every direct child of the strips host is a
    // strip; the layout's stretch is an item, not a widget).
    const auto olds = stripsHost_->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly);
    for (QWidget *w : olds) {
        w->deleteLater();
    }
    strips_.clear();

    if (model) {
        for (const fc::Track &track : model->tracks()) {
            if (!track.isAudio) {
                continue; // video/text lanes carry no strip
            }
            Strip strip;
            strip.trackIndex = track.index;
            buildStrip(QString::fromStdString(track.name), true, strip, stripsHost_);
            strip.fader->blockSignals(true);
            strip.fader->setValue(int(std::llround(track.gainDb)));
            strip.fader->blockSignals(false);
            strip.dbLabel->setText(dbText(int(std::llround(track.gainDb))));
            if (strip.pan) {
                strip.pan->blockSignals(true);
                strip.pan->setValue(int(std::llround(track.pan * 100.0)));
                strip.pan->blockSignals(false);
                strip.panLabel->setText(panText(int(std::llround(track.pan * 100.0))));
            }
            strip.mute->blockSignals(true);
            strip.mute->setChecked(track.muted);
            strip.mute->blockSignals(false);
            strip.solo->blockSignals(true);
            strip.solo->setChecked(track.solo);
            strip.solo->blockSignals(false);
            strips_.push_back(strip);
        }
    }

    if (masterFader_ && model) {
        masterFader_->blockSignals(true);
        masterFader_->setValue(int(std::llround(model->masterGainDb())));
        masterFader_->blockSignals(false);
        masterDbLabel_->setText(dbText(int(std::llround(model->masterGainDb()))));
    }
}

// ---- Meter feed (suggestion #45): slots the audio pull path taps ----

void MixerPanel::setLevels(const QVector<double> &dbfsPerTrack) {
    for (size_t i = 0; i < strips_.size(); ++i) {
        if (!strips_[i].meter) {
            continue;
        }
        // A short vector idles the strips it does not cover; every
        // value clamps into the meter's -60..0 dBFS window.
        strips_[i].meter->setLevel(i < static_cast<size_t>(dbfsPerTrack.size())
                                       ? dbfsPerTrack[static_cast<int>(i)]
                                       : kSilentDb);
    }
    startHoldTimer();
}

void MixerPanel::setMasterLevel(double dbfs) {
    if (masterMeter_) {
        masterMeter_->setLevel(dbfs);
    }
    startHoldTimer();
}

void MixerPanel::startHoldTimer() {
    if (holdTimer_ && !holdTimer_->isActive()) {
        holdTimer_->start();
    }
}

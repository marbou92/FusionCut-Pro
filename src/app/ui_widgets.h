#pragma once

#include <QCache>
#include <QColor>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPoint>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QResizeEvent>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QtGlobal>

#include <functional>
#include <vector>

#include "ui_theme.h"

namespace fc {

// Reusable inline surfaces for the cross-cutting suggestions:
//   #60 error surfaces - styled inline banners with a recovery action
//   #61 empty states  - illustrated hint with the next action
// Round-22 additions (Apple-style round 2):
//   icons::makeIcon   - devicePixelRatio-aware stroke icon factory (#76/#97)
//   SegmentedControl  - pill segmented control (#69/#81/#85/#86)
//   EmptyState v2     - icon + buttons + fade-in (#74/#84/#92)
//   Toast             - bottom-center pill feedback (#94)
// All Q_OBJECT-free (actions arrive as std::function), header-only, no
// AUTOMOC run needed.

// ---------------------------------------------------------------------------
// icons (suggestion #76/#97): one stroke language for every toolbar button.
// Drawn at devicePixelRatio so 125/150% Windows scaling stays crisp. The
// 24-unit design grid maps to the requested pixel size; stroke width 1.6
// grid units, round caps/joins - the SF Symbols proportions.
// ---------------------------------------------------------------------------
namespace icons {

// Draws `name` in `color` and returns the QIcon. Supported names are the
// union of every toolbar/transport glyph the app needs; unknown names fall
// back to a small circle so a missing glyph is visible, not silent.
inline QIcon makeIcon(const QString &name, const QColor &color, int logicalSize = 20,
                      qreal dpr = 1.0) {
    // #208: a glyph is deterministic in (name, color, size, DPR), so the
    // rendered pixmap is cached - re-styling sweeps and repeated builds
    // of toolbars/panels never re-render the same stroke path. GUI-thread
    // only (a UI helper), so the plain QCache needs no mutex; 512 slots
    // cover every (name, color, size, DPR) tuple the app can ask for.
    static QCache<QString, QPixmap> cache;
    static const bool cacheReady = (cache.setMaxCost(512), true);
    Q_UNUSED(cacheReady);
    const QString key = QStringLiteral("%1|%2|%3|%4")
                            .arg(name, color.name(QColor::HexArgb))
                            .arg(logicalSize)
                            .arg(dpr, 0, 'f', 2);
    if (QPixmap *const hit = cache.object(key)) {
        return QIcon(*hit);
    }
    const int px = qMax(1, qRound(logicalSize * dpr));
    QPixmap pm(px, px);
    pm.setDevicePixelRatio(dpr == 0 ? 1.0 : dpr);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    const qreal s = qreal(px) / 24.0;
    p.scale(s, s);
    QPen pen(color, 1.6);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);

    auto line = [&p](qreal x1, qreal y1, qreal x2, qreal y2) {
        p.drawLine(QPointF(x1, y1), QPointF(x2, y2));
    };
    auto tri = [&p](const QPointF &a, const QPointF &b, const QPointF &c) {
        QPainterPath t;
        t.moveTo(a);
        t.lineTo(b);
        t.lineTo(c);
        t.closeSubpath();
        p.drawPath(t);
    };

    if (name == QLatin1String("film")) { // movie frame with sprockets
        p.drawRoundedRect(QRectF(3, 5, 18, 14), 2, 2);
        line(7, 5, 7, 19);
        line(17, 5, 17, 19);
        line(7, 12, 17, 12);
    } else if (name == QLatin1String("plus")) {
        line(12, 5, 12, 19);
        line(5, 12, 19, 12);
    } else if (name == QLatin1String("text")) {
        line(5, 6, 19, 6);
        line(12, 6, 12, 19);
    } else if (name == QLatin1String("sticker")) {
        p.drawEllipse(QRectF(4.5, 4.5, 15, 15));
        p.drawArc(QRectF(8.5, 9, 7, 7), 200 * 16, 140 * 16);
        p.drawEllipse(QRectF(8.7, 8.7, 1.6, 1.6));
        p.drawEllipse(QRectF(13.7, 8.7, 1.6, 1.6));
    } else if (name == QLatin1String("effects")) { // 4-point sparkle
        QPainterPath sp;
        sp.moveTo(12, 3);
        sp.cubicTo(12.8, 9.2, 14.8, 11.2, 21, 12);
        sp.cubicTo(14.8, 12.8, 12.8, 14.8, 12, 21);
        sp.cubicTo(11.2, 14.8, 9.2, 12.8, 3, 12);
        sp.cubicTo(9.2, 11.2, 11.2, 9.2, 12, 3);
        p.drawPath(sp);
        line(19, 4, 19, 7);
        line(17.5, 5.5, 20.5, 5.5);
    } else if (name == QLatin1String("transitions")) {
        p.drawRoundedRect(QRectF(3, 7, 6, 10), 1.5, 1.5);
        p.drawRoundedRect(QRectF(15, 7, 6, 10), 1.5, 1.5);
        line(10.5, 12, 13.5, 12);
        tri(QPointF(13.5, 9.8), QPointF(13.5, 14.2), QPointF(16.2, 12));
    } else if (name == QLatin1String("filters")) { // three slider rows
        line(4, 7, 20, 7);
        line(4, 12, 20, 12);
        line(4, 17, 20, 17);
        p.drawEllipse(QRectF(9, 5.2, 3.6, 3.6));
        p.drawEllipse(QRectF(14.5, 10.2, 3.6, 3.6));
        p.drawEllipse(QRectF(7, 15.2, 3.6, 3.6));
    } else if (name == QLatin1String("captions")) {
        QPainterPath bubble;
        bubble.addRoundedRect(QRectF(3, 5, 18, 12), 3, 3);
        p.drawPath(bubble);
        line(9, 17, 9, 20);
        line(9, 20, 13.5, 17);
        line(7, 9, 17, 9);
        line(7, 13, 13, 13);
    } else if (name == QLatin1String("enhance")) { // wand + sparkles
        line(5, 19, 14, 10);
        line(12.6, 8.6, 15.4, 11.4);
        line(17, 3.5, 17, 7.5);
        line(15, 5.5, 19, 5.5);
        line(20.5, 10, 20.5, 13);
        line(19, 11.5, 22, 11.5);
    } else if (name == QLatin1String("crop")) {
        line(6, 3, 6, 18);
        line(6, 18, 21, 18);
        line(18, 6, 3, 6);
        line(18, 6, 18, 21);
    } else if (name == QLatin1String("templates")) {
        p.drawRoundedRect(QRectF(4, 4, 7, 7), 1.5, 1.5);
        p.drawRoundedRect(QRectF(13, 4, 7, 7), 1.5, 1.5);
        p.drawRoundedRect(QRectF(4, 13, 7, 7), 1.5, 1.5);
        p.drawRoundedRect(QRectF(13, 13, 7, 7), 1.5, 1.5);
    } else if (name == QLatin1String("play")) {
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        tri(QPointF(8, 5.5), QPointF(8, 18.5), QPointF(19, 12));
    } else if (name == QLatin1String("pause")) {
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        QPainterPath b;
        b.addRoundedRect(QRectF(7, 5.5, 3.4, 13), 1.4, 1.4);
        b.addRoundedRect(QRectF(13.6, 5.5, 3.4, 13), 1.4, 1.4);
        p.drawPath(b);
    } else if (name == QLatin1String("step-back")) {
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        QPainterPath b;
        b.addRoundedRect(QRectF(5, 5.5, 2.4, 13), 1.2, 1.2);
        p.drawPath(b);
        p.setPen(pen);
        tri(QPointF(19, 5.8), QPointF(19, 18.2), QPointF(9.5, 12));
    } else if (name == QLatin1String("step-fwd")) {
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        QPainterPath b;
        b.addRoundedRect(QRectF(16.6, 5.5, 2.4, 13), 1.2, 1.2);
        p.drawPath(b);
        p.setPen(pen);
        tri(QPointF(5, 5.8), QPointF(5, 18.2), QPointF(14.5, 12));
    } else if (name == QLatin1String("volume")) {
        QPainterPath spk;
        spk.moveTo(4, 9.5);
        spk.lineTo(8, 9.5);
        spk.lineTo(13, 5);
        spk.lineTo(13, 19);
        spk.lineTo(8, 14.5);
        spk.lineTo(4, 14.5);
        spk.closeSubpath();
        p.drawPath(spk);
        p.drawArc(QRectF(14, 8, 6, 8), -55 * 16, 110 * 16);
    } else if (name == QLatin1String("mute")) {
        QPainterPath spk;
        spk.moveTo(4, 9.5);
        spk.lineTo(8, 9.5);
        spk.lineTo(13, 5);
        spk.lineTo(13, 19);
        spk.lineTo(8, 14.5);
        spk.lineTo(4, 14.5);
        spk.closeSubpath();
        p.drawPath(spk);
        line(15.5, 9.5, 20.5, 14.5);
        line(20.5, 9.5, 15.5, 14.5);
    } else if (name == QLatin1String("check")) {
        QPainterPath c;
        c.moveTo(5, 12.5);
        c.lineTo(10, 17.5);
        c.lineTo(19, 7);
        p.drawPath(c);
    } else if (name == QLatin1String("chevron-down")) {
        QPainterPath c;
        c.moveTo(6, 9.5);
        c.lineTo(12, 15.5);
        c.lineTo(18, 9.5);
        p.drawPath(c);
    } else if (name == QLatin1String("chevron-right")) {
        QPainterPath c;
        c.moveTo(9.5, 6);
        c.lineTo(15.5, 12);
        c.lineTo(9.5, 18);
        p.drawPath(c);
    } else if (name == QLatin1String("search")) {
        p.drawEllipse(QRectF(5, 5, 10.5, 10.5));
        line(14.2, 14.2, 19.5, 19.5);
    } else if (name == QLatin1String("grid")) {
        p.drawRoundedRect(QRectF(4.5, 4.5, 6, 6), 1.2, 1.2);
        p.drawRoundedRect(QRectF(13.5, 4.5, 6, 6), 1.2, 1.2);
        p.drawRoundedRect(QRectF(4.5, 13.5, 6, 6), 1.2, 1.2);
        p.drawRoundedRect(QRectF(13.5, 13.5, 6, 6), 1.2, 1.2);
    } else if (name == QLatin1String("list")) {
        p.drawEllipse(QRectF(4.4, 5.4, 1.8, 1.8));
        p.drawEllipse(QRectF(4.4, 11.1, 1.8, 1.8));
        p.drawEllipse(QRectF(4.4, 16.8, 1.8, 1.8));
        line(9.5, 6.3, 19.5, 6.3);
        line(9.5, 12, 19.5, 12);
        line(9.5, 17.7, 19.5, 17.7);
    } else if (name == QLatin1String("star")) {
        QPainterPath st;
        st.moveTo(12, 3.6);
        st.lineTo(14.5, 9);
        st.lineTo(20.4, 9.7);
        st.lineTo(16.1, 13.8);
        st.lineTo(17.2, 19.7);
        st.lineTo(12, 16.8);
        st.lineTo(6.8, 19.7);
        st.lineTo(7.9, 13.8);
        st.lineTo(3.6, 9.7);
        st.lineTo(9.5, 9);
        st.closeSubpath();
        p.drawPath(st);
    } else if (name == QLatin1String("close")) {
        line(6, 6, 18, 18);
        line(18, 6, 6, 18);
    } else if (name == QLatin1String("reset")) { // circular arrow
        p.drawArc(QRectF(5, 5, 14, 14), 45 * 16, 270 * 16);
        tri(QPointF(19.6, 6.2), QPointF(19.6, 12.6), QPointF(14.6, 9.4));
    } else if (name == QLatin1String("redo")) { // circular arrow, mirrored (quick toolbar)
        p.drawArc(QRectF(5, 5, 14, 14), 225 * 16, 270 * 16);
        tri(QPointF(4.4, 6.2), QPointF(4.4, 12.6), QPointF(9.4, 9.4));
    } else if (name == QLatin1String("waveform")) {
        line(4, 10, 4, 14);
        line(8, 7, 8, 17);
        line(12, 4, 12, 20);
        line(16, 8, 16, 16);
        line(20, 10.5, 20, 13.5);
    } else if (name == QLatin1String("lock")) {
        p.drawRoundedRect(QRectF(6, 10.5, 12, 9), 2, 2);
        p.drawArc(QRectF(8.5, 4, 7, 9), 0, 180 * 16);
        line(12, 14, 12, 16.5);
    } else if (name == QLatin1String("folder")) {
        QPainterPath f;
        f.moveTo(3.5, 7);
        f.lineTo(3.5, 18);
        f.lineTo(20.5, 18);
        f.lineTo(20.5, 9);
        f.lineTo(12, 9);
        f.lineTo(10, 7);
        f.closeSubpath();
        p.drawPath(f);
    } else if (name == QLatin1String("image")) {
        p.drawRoundedRect(QRectF(3.5, 4.5, 17, 15), 2, 2);
        p.drawEllipse(QRectF(7, 7.5, 2.6, 2.6));
        QPainterPath m;
        m.moveTo(4.5, 17);
        m.lineTo(10, 11.5);
        m.lineTo(13.5, 15);
        m.lineTo(16, 12.5);
        m.lineTo(19.6, 16.1);
        p.drawPath(m);
    } else if (name == QLatin1String("music")) {
        p.drawEllipse(QRectF(6, 15, 4.4, 4.4));
        p.drawEllipse(QRectF(15, 13, 4.4, 4.4));
        line(10.4, 17.2, 10.4, 6.5);
        line(10.4, 6.5, 19.4, 4.8);
        line(19.4, 4.8, 19.4, 15.2);
    } else if (name == QLatin1String("cursor")) { // pointer arrow (select tool)
        QPainterPath ar;
        ar.moveTo(7, 4);
        ar.lineTo(7, 18.5);
        ar.lineTo(11, 14.8);
        ar.lineTo(13.6, 20);
        ar.lineTo(16, 18.8);
        ar.lineTo(13.4, 13.8);
        ar.lineTo(18.5, 13.2);
        ar.closeSubpath();
        p.drawPath(ar);
    } else if (name == QLatin1String("blade")) { // razor (split tool)
        line(5, 19, 15.5, 8.5);
        line(13.5, 6.5, 17.5, 10.5);
        p.drawEllipse(QRectF(6.5, 14.5, 5, 5));
        p.drawEllipse(QRectF(12.5, 16.5, 5, 5));
    } else {
        p.drawEllipse(QRectF(8, 8, 8, 8)); // unknown glyph: visible dot
    }
    cache.insert(key, new QPixmap(pm));
    return QIcon(pm);
}

} // namespace icons

// ---------------------------------------------------------------------------
// SegmentedControl (Apple NSSegmentedControl look): a pill track holding
// auto-exclusive checkable buttons. One accent-lit segment; callback style
// keeps the class Q_OBJECT-free.
// ---------------------------------------------------------------------------
class SegmentedControl : public QFrame {
public:
    explicit SegmentedControl(QWidget *parent = nullptr) : QFrame(parent) {
        setObjectName(QStringLiteral("fcSegmented"));
        track_ = new QHBoxLayout(this);
        track_->setContentsMargins(3, 3, 3, 3);
        track_->setSpacing(2);
    }

    // Adds a segment; `icon` may be null for text-only segments. Returns
    // the segment index for setCurrent()/callbacks.
    int addSegment(const QString &text, const QIcon &icon = QIcon(),
                   const QString &tooltip = QString()) {
        auto *b = new QPushButton(text, this);
        b->setCheckable(true);
        b->setAutoExclusive(true); // siblings share `this` parent -> works
        b->setCursor(Qt::PointingHandCursor);
        b->setProperty("fcSegment", true);
        b->setFocusPolicy(Qt::NoFocus); // the control, not a segment, is focusable
        if (!icon.isNull()) {
            b->setIcon(icon);
            b->setIconSize(QSize(16, 16));
        }
        if (!tooltip.isEmpty()) {
            b->setToolTip(tooltip);
        }
        connect(b, &QPushButton::toggled, this, [this, index = segments_.size()](bool on) {
            if (on && onSelect_) {
                onSelect_(index);
            }
        });
        track_->addWidget(b);
        segments_.push_back(b);
        if (segments_.size() == 1) {
            b->setChecked(true);
        }
        return static_cast<int>(segments_.size()) - 1;
    }

    void setCurrent(int index) {
        if (index < 0 || index >= static_cast<int>(segments_.size())) {
            return;
        }
        if (!segments_[static_cast<size_t>(index)]->isChecked()) {
            segments_[static_cast<size_t>(index)]->setChecked(true);
        }
    }

    int current() const {
        for (size_t i = 0; i < segments_.size(); ++i) {
            if (segments_[i]->isChecked()) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    QPushButton *button(int index) const {
        return (index >= 0 && index < static_cast<int>(segments_.size()))
                   ? segments_[static_cast<size_t>(index)]
                   : nullptr;
    }

    void onSelected(std::function<void(int)> handler) { onSelect_ = std::move(handler); }

private:
    QHBoxLayout *track_ = nullptr;
    std::vector<QPushButton *> segments_;
    std::function<void(int)> onSelect_;
};

// ---------------------------------------------------------------------------
// Empty-state placeholder v2 (#61 + #74): centered icon/glyph + title +
// hint + optional action-button row ("Import Media..." / "Use a Template").
// Fades in over 140ms (#92). Call refresh(isEmpty) or drive show()/hide()
// from the panel's refresh path.
// ---------------------------------------------------------------------------
class EmptyState : public QWidget {
public:
    explicit EmptyState(QWidget *parent = nullptr) : QWidget(parent) {
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(16, 16, 16, 16);
        layout->setSpacing(6);
        glyph_ = new QLabel(this);
        glyph_->setAlignment(Qt::AlignCenter);
        title_ = new QLabel(this);
        title_->setAlignment(Qt::AlignCenter);
        title_->setWordWrap(true);
        hint_ = new QLabel(this);
        hint_->setAlignment(Qt::AlignCenter);
        hint_->setWordWrap(true);
        actions_ = new QWidget(this);
        actionsRow_ = new QHBoxLayout(actions_);
        actionsRow_->setContentsMargins(0, 6, 0, 0);
        actionsRow_->setSpacing(8);
        actionsRow_->setAlignment(Qt::AlignCenter);
        actions_->hide();
        layout->addStretch(1);
        layout->addWidget(glyph_);
        layout->addWidget(title_);
        layout->addWidget(hint_);
        layout->addWidget(actions_, 0, Qt::AlignHCenter);
        layout->addStretch(2);
        restyle();
    }

    void setGlyph(const QString &glyph) {
        iconLabel_ = QIcon(); // Qt 5 QIcon has no clear(); assign-empty instead
        glyph_->setText(glyph);
        glyph_->setVisible(!glyph.isEmpty());
    }

    void setTitle(const QString &title) { title_->setText(title); }

    void setHint(const QString &hint) { hint_->setText(hint); }

    // v2 (#74): an icon instead of (or above) the text glyph. Sized in
    // logical pixels; pass the host's devicePixelRatioF() for crispness.
    void setIcon(const QIcon &icon, int logicalSize = 44, qreal dpr = 1.0) {
        const qreal ratio = dpr > 0 ? dpr : 1.0;
        glyph_->setText(QString());
        glyph_->setPixmap(icon.pixmap(qRound(logicalSize * ratio), qRound(logicalSize * ratio)));
        glyph_->setVisible(true);
    }

    // v2 (#74): action buttons. `primary` uses the app-wide accent style
    // (QPushButton[fcAccent]); otherwise a neutral ghost pill.
    void addAction(const QString &text, bool primary, std::function<void()> handler) {
        auto *b = new QPushButton(text, actions_);
        b->setProperty("fcAccent", primary);
        b->setProperty("fcGhost", !primary);
        b->setCursor(Qt::PointingHandCursor);
        connect(b, &QPushButton::clicked, this, [handler] { handler(); });
        actionsRow_->addWidget(b);
        actions_->show();
    }

    // Keeps the empty state visible exactly while `condition` holds by
    // watching the host widget's resize (cheap refresh point panels
    // already have) AND every explicit refresh() call.
    void refresh(bool isEmpty) { setVisible(isEmpty); }

protected:
    void showEvent(QShowEvent *event) override {
        QWidget::showEvent(event);
        // #92: 140ms ease-out fade-in; nothing bounces, nothing blocks.
        if (!fade_) {
            fade_ = new QGraphicsOpacityEffect(this);
            fade_->setOpacity(1.0);
            setGraphicsEffect(fade_);
        }
        anim_ = new QPropertyAnimation(fade_, "opacity", this);
        anim_->setDuration(140);
        anim_->setStartValue(0.0);
        anim_->setEndValue(1.0);
        anim_->setEasingCurve(QEasingCurve::OutCubic);
        anim_->start(QAbstractAnimation::DeleteWhenStopped);
    }

private:
    void restyle() {
        glyph_->setStyleSheet(
            QStringLiteral("font-size:30px;color:%1;").arg(ui::color(ui::kTextDisabled).name()));
        title_->setStyleSheet(QStringLiteral("font-size:15px;font-weight:600;color:%1;")
                                  .arg(ui::color(ui::kText).name()));
        hint_->setStyleSheet(
            QStringLiteral("font-size:12px;color:%1;").arg(ui::color(ui::kTextDim).name()));
    }

    QLabel *glyph_ = nullptr;
    QLabel *title_ = nullptr;
    QLabel *hint_ = nullptr;
    QWidget *actions_ = nullptr;
    QHBoxLayout *actionsRow_ = nullptr;
    QIcon iconLabel_;
    QGraphicsOpacityEffect *fade_ = nullptr;
    QPropertyAnimation *anim_ = nullptr;
};

// ---------------------------------------------------------------------------
// Toast (#94): a bottom-center rounded pill for transient feedback
// (undo/redo/import). Fades in, holds ~2s, fades out; parent resize keeps
// it docked. Status bar keeps persistent state only.
// ---------------------------------------------------------------------------
class Toast : public QLabel {
public:
    static void showOn(QWidget *parent, const QString &text, int holdMs = 2200) {
        if (parent == nullptr) {
            return;
        }
        // Reuse a live toast so rapid feedback replaces, never stacks.
        auto *existing = parent->findChild<Toast *>(QStringLiteral("fcToast"));
        if (existing != nullptr) {
            existing->restart(text, holdMs);
            return;
        }
        auto *t = new Toast(parent);
        t->restart(text, holdMs);
    }

private:
    explicit Toast(QWidget *parent) : QLabel(parent) {
        setObjectName(QStringLiteral("fcToast"));
        setAlignment(Qt::AlignCenter);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        effect_ = new QGraphicsOpacityEffect(this);
        setGraphicsEffect(effect_);
        timer_ = new QTimer(this);
        timer_->setSingleShot(true);
        connect(timer_, &QTimer::timeout, this, [this] { fadeTo(0.0, true); });
        parent->installEventFilter(this);
        hide();
    }

    void restart(const QString &text, int holdMs) {
        setText(text);
        adjustSize();
        reposition();
        effect_->setOpacity(0.0);
        show();
        raise();
        fadeTo(1.0, false);
        timer_->start(holdMs);
    }

    void reposition() {
        if (parentWidget() == nullptr) {
            return;
        }
        move(qMax(8, (parentWidget()->width() - width()) / 2),
             qMax(8, parentWidget()->height() - height() - 44));
    }

    void fadeTo(qreal target, bool closeAfter) {
        auto *a = new QPropertyAnimation(effect_, "opacity", this);
        a->setDuration(150);
        a->setStartValue(effect_->opacity());
        a->setEndValue(target);
        a->setEasingCurve(QEasingCurve::OutCubic);
        if (closeAfter) {
            connect(a, &QPropertyAnimation::finished, this, &QObject::deleteLater);
        }
        a->start(QAbstractAnimation::DeleteWhenStopped);
    }

    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched == parentWidget() && event->type() == QEvent::Resize) {
            reposition();
        }
        return QLabel::eventFilter(watched, event);
    }

    QGraphicsOpacityEffect *effect_ = nullptr;
    QTimer *timer_ = nullptr;
};

// Inline error banner (suggestion #60): a danger-tinted rounded strip
// with the message, an optional recovery action button ("Reveal
// file...", "Reconnect drive..."), and a dismiss X. Hidden until
// showError(); clear() hides it again. Panels embed it above their
// content; it never steals focus.
class ErrorBanner : public QFrame {
public:
    explicit ErrorBanner(QWidget *parent = nullptr) : QFrame(parent) {
        setObjectName(QStringLiteral("fcErrorBanner"));
        setStyleSheet(QStringLiteral("QFrame#fcErrorBanner{background:%1;border:1px solid %2;"
                                     "border-radius:4px;}")
                          .arg(ui::tint(ui::kDanger).name(), ui::color(ui::kDanger).name()));
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(8, 6, 8, 6);
        layout->setSpacing(8);
        message_ = new QLabel(this);
        message_->setWordWrap(true);
        message_->setStyleSheet(
            QStringLiteral("color:%1;border:none;").arg(ui::color(ui::kText).name()));
        action_ = new QPushButton(this);
        action_->setFlat(true);
        action_->setCursor(Qt::PointingHandCursor);
        action_->setStyleSheet(
            QStringLiteral(
                "QPushButton{color:%1;border:1px solid %1;border-radius:3px;padding:2px 8px;}"
                "QPushButton:hover{background:%2;}")
                .arg(ui::color(ui::kDanger).name(), ui::withAlpha(ui::kDanger, 40).name()));
        dismiss_ = new QPushButton(this);
        dismiss_->setFlat(true);
        dismiss_->setCursor(Qt::PointingHandCursor);
        dismiss_->setText(QStringLiteral("\u00D7")); // multiplication sign
        dismiss_->setToolTip(tr("Dismiss"));
        dismiss_->setFixedSize(18, 18);
        dismiss_->setStyleSheet(
            QStringLiteral("QPushButton{color:%1;border:none;font-weight:bold;}")
                .arg(ui::color(ui::kTextDim).name()));
        layout->addWidget(message_, 1);
        layout->addWidget(action_);
        layout->addWidget(dismiss_);
        hide();
        connect(dismiss_, &QPushButton::clicked, this, [this] { clear(); });
        connect(action_, &QPushButton::clicked, this, [this] {
            if (handler_) {
                handler_();
            }
        });
    }

    void showError(const QString &message, const QString &actionText,
                   std::function<void()> onAction) {
        message_->setText(message);
        handler_ = std::move(onAction);
        action_->setVisible(!actionText.isEmpty());
        action_->setText(actionText);
        setToolTip(message);
        show();
    }

    void showError(const QString &message) { showError(message, QString(), {}); }

    void clear() {
        handler_ = {};
        hide();
    }

private:
    QLabel *message_ = nullptr;
    QPushButton *action_ = nullptr;
    QPushButton *dismiss_ = nullptr;
    std::function<void()> handler_;
};

} // namespace fc

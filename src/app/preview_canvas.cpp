#include "preview_canvas.h"

#include <algorithm>
#include <cmath>

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QToolButton>
#include <QVBoxLayout>

#include "ui_theme.h"

// ui_theme.h tokens live in fc::ui; the panel addresses them as ui::...
using namespace fc;

// Shared design tokens (suggestion #55) live in fc::ui; alias once so the
// paint-side call sites stay readable.
namespace ui = fc::ui;

PreviewCanvas::PreviewCanvas(QWidget *parent) : QWidget(parent) {
    setMinimumSize(160, 90);
    buildOverlays();
}

QSize PreviewCanvas::minimumSizeHint() const {
    return QSize(160, 90);
}

void PreviewCanvas::setAspectHint(double aspect) {
    if (aspect > 0.05 && aspect < 20.0) {
        aspectHint_ = aspect;
        update();
    }
}

void PreviewCanvas::setGuidesVisible(bool on) {
    if (guidesVisible_ == on) {
        return;
    }
    guidesVisible_ = on;
    update();
}

void PreviewCanvas::setQuality(Quality quality) {
    if (quality_ == quality) {
        return;
    }
    quality_ = quality;
    // Keep the dropdown in sync with programmatic changes without
    // re-entering the combo's own currentIndexChanged handler.
    if (qualityBox_->currentIndex() != static_cast<int>(quality)) {
        qualityBox_->blockSignals(true);
        qualityBox_->setCurrentIndex(static_cast<int>(quality));
        qualityBox_->blockSignals(false);
    }
    update();
}

void PreviewCanvas::setMiniTransportVisible(bool on) {
    if (miniTransport_ != nullptr) {
        miniTransport_->setVisible(on);
    }
    if (on) {
        // Auto-raised: the dock must stay above the frame paint (it is
        // a child widget, so an explicit raise() keeps it on top even
        // after other children are created later).
        miniTransport_->raise();
        positionOverlays();
    }
}

bool PreviewCanvas::miniTransportVisible() const {
    return miniTransport_ != nullptr && miniTransport_->isVisible();
}

void PreviewCanvas::setFrame(const QImage &frame, double ptsSeconds) {
    current_ = frame;
    lastPts_ = ptsSeconds;
    if (!current_.isNull() && current_.height() > 0) {
        aspectHint_ = static_cast<double>(current_.width()) / current_.height();
    }
    if (miniPos_ != nullptr) {
        // The canvas has no frame rate of its own, so the mini dock
        // reports the raw source position in seconds (honest, no
        // invented rate).
        miniPos_->setText(QString::number(lastPts_, 'f', 2) + QStringLiteral(" s"));
    }
    update();
}

void PreviewCanvas::buildOverlays() {
    // Quality dropdown (#22): a canvas CHILD, so it rides on top of the
    // paint without any mainwindow wiring. NoFocus keeps the canvas's
    // keyboard behavior untouched.
    qualityBox_ = new QComboBox(this);
    qualityBox_->addItem(tr("Full"));
    qualityBox_->addItem(tr("1/2"));
    qualityBox_->addItem(tr("1/4"));
    qualityBox_->setToolTip(tr("Preview quality - never affects export"));
    qualityBox_->setAccessibleName(tr("Preview quality"));
    qualityBox_->setFocusPolicy(Qt::NoFocus);
    qualityBox_->setStyleSheet(
        QStringLiteral(
            "QComboBox{background:%1;color:%2;border:1px solid %3;border-radius:3px;padding:1px "
            "6px;}"
            "QComboBox QAbstractItemView{background:%1;color:%2;selection-background-color:%4;}")
            .arg(ui::withAlpha(ui::kSurface2, 235).name(), ui::color(ui::kText).name(),
                 ui::color(ui::kLine).name(), ui::color(ui::kAccent).name()));
    connect(qualityBox_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
            [this](int index) {
                quality_ = static_cast<Quality>(index);
                update();
            });

    // Source mini transport (#24). Buttons only emit; the coordinator
    // connects sourceStepRequested/sourcePlayToggled (SOURCE monitor
    // only). Everything stays inert until setMiniTransportVisible(true).
    miniTransport_ = new QFrame(this);
    miniTransport_->setObjectName(QStringLiteral("fcMiniTransport"));
    miniTransport_->setStyleSheet(
        QStringLiteral(
            "QFrame#fcMiniTransport{background:%1;border:1px solid %2;border-radius:4px;}"
            "QFrame#fcMiniTransport QToolButton{background:transparent;color:%2;border:none;}"
            "QFrame#fcMiniTransport QToolButton:checked{background:%3;color:%4;border-radius:3px;}")
            .arg(ui::withAlpha(ui::kSurface2, 235).name(), ui::color(ui::kText).name(),
                 ui::color(ui::kAccent).name(), ui::color(ui::kOnAccent).name()));
    auto *miniLayout = new QHBoxLayout(miniTransport_);
    miniLayout->setContentsMargins(8, 4, 8, 4); // 4/8 rhythm
    miniLayout->setSpacing(4);

    auto *miniBack = new QToolButton(miniTransport_);
    miniBack->setText(QStringLiteral("-1"));
    miniBack->setToolTip(tr("Step back 1 frame"));
    miniBack->setAccessibleName(tr("Step back 1 frame"));
    miniBack->setFixedSize(28, 28); // #63 touch target
    connect(miniBack, &QToolButton::clicked, this, [this] { emit sourceStepRequested(-1); });

    miniPlay_ = new QToolButton(miniTransport_);
    miniPlay_->setText(tr("Play"));
    miniPlay_->setCheckable(true);
    miniPlay_->setToolTip(tr("Play"));
    miniPlay_->setAccessibleName(tr("Play"));
    miniPlay_->setMinimumSize(28, 28);
    connect(miniPlay_, &QToolButton::clicked, this, [this](bool playing) {
        // The button owns its checked look; real play state feedback
        // flows back through the coordinator's wiring.
        miniPlay_->setText(playing ? tr("Pause") : tr("Play"));
        emit sourcePlayToggled(playing);
    });

    auto *miniFwd = new QToolButton(miniTransport_);
    miniFwd->setText(QStringLiteral("+1"));
    miniFwd->setToolTip(tr("Step forward 1 frame"));
    miniFwd->setAccessibleName(tr("Step forward 1 frame"));
    miniFwd->setFixedSize(28, 28);
    connect(miniFwd, &QToolButton::clicked, this, [this] { emit sourceStepRequested(1); });

    miniPos_ = new QLabel(QStringLiteral("0.00 s"), miniTransport_);
    miniPos_->setStyleSheet(
        QStringLiteral("color:%1;border:none;").arg(ui::color(ui::kTextDim).name()));

    miniLayout->addWidget(miniBack);
    miniLayout->addWidget(miniPlay_);
    miniLayout->addWidget(miniFwd);
    miniLayout->addWidget(miniPos_);

    miniTransport_->hide(); // opt-in (#24): inert until enabled
    positionOverlays();
    // The combo is always-on: an explicit show() makes that intent visible
    // even when the canvas is already realized when this runs.
    qualityBox_->show();
}

void PreviewCanvas::positionOverlays() {
    if (qualityBox_ != nullptr) {
        qualityBox_->adjustSize();
        qualityBox_->move(width() - qualityBox_->width() - 8, 8);
    }
    if (miniTransport_ != nullptr && miniTransport_->isVisible()) {
        miniTransport_->adjustSize();
        miniTransport_->move((width() - miniTransport_->width()) / 2,
                             height() - miniTransport_->height() - 8);
    }
}

void PreviewCanvas::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), ui::color(ui::kCanvas));

    if (current_.isNull()) {
        painter.setPen(ui::color(ui::kTextDisabled));
        painter.drawText(rect(), Qt::AlignCenter, tr("No media loaded\nImport a file to begin"));
        return;
    }

    // Letterbox: largest rect with the frame aspect inside the widget.
    const double widgetAspect = static_cast<double>(width()) / std::max(1, height());
    int drawW = width();
    int drawH = height();
    if (widgetAspect > aspectHint_) {
        drawW = static_cast<int>(height() * aspectHint_);
    } else {
        drawH = static_cast<int>(width() / aspectHint_);
    }
    const int x = (width() - drawW) / 2;
    const int y = (height() - drawH) / 2;

    // Preview quality (#22): the factor lives ONLY in the scale of the
    // frame image - the letterbox math above keeps the SAME displayed
    // rect regardless of quality, and the downscaled image is painted
    // back into that rect. Full is bit-identical to the old behavior.
    // (Deviation, documented: the suggestion's "rendering..." state for
    // slow frames is not honestly implementable here - the canvas only
    // sees frames that already arrived from the decode worker.)
    const double factor = quality_ == Quality::Full ? 1.0 : quality_ == Quality::Half ? 0.5 : 0.25;
    const int srcW = std::max(1, static_cast<int>(std::llround(drawW * factor)));
    const int srcH = std::max(1, static_cast<int>(std::llround(drawH * factor)));

    // Fast transform: proxies are 360p, scaling cost stays negligible on
    // legacy CPUs (the 1 GB target machines).
    buffer_ = QPixmap::fromImage(
        current_.scaled(srcW, srcH, Qt::IgnoreAspectRatio, Qt::FastTransformation));
    painter.drawPixmap(x, y, drawW, drawH, buffer_);

    // Monitor overlays (#21) are canvas-paint-only: they live in this
    // paintEvent and never modify the QImage the core pipeline renders,
    // so exports (which render through the core pipeline, never through
    // this widget) can never contain them - nothing to do at export
    // time by construction.
    if (guidesVisible_) {
        drawGuides(painter, QRect(x, y, drawW, drawH));
    }
}

void PreviewCanvas::drawGuides(QPainter &painter, const QRect &frameRect) const {
    // Crisp 1 px hairlines - no antialiasing on overlay geometry.
    painter.setRenderHint(QPainter::Antialiasing, false);

    // Action-safe at 90% of the displayed frame.
    const int actionW = std::max(1, static_cast<int>(std::llround(frameRect.width() * 0.90)));
    const int actionH = std::max(1, static_cast<int>(std::llround(frameRect.height() * 0.90)));
    painter.setPen(QPen(ui::withAlpha(ui::kText, 70), 1));
    painter.drawRect(frameRect.x() + (frameRect.width() - actionW) / 2,
                     frameRect.y() + (frameRect.height() - actionH) / 2, actionW, actionH);

    // Title-safe at 80%.
    const int titleW = std::max(1, static_cast<int>(std::llround(frameRect.width() * 0.80)));
    const int titleH = std::max(1, static_cast<int>(std::llround(frameRect.height() * 0.80)));
    painter.setPen(QPen(ui::withAlpha(ui::kText, 50), 1));
    painter.drawRect(frameRect.x() + (frameRect.width() - titleW) / 2,
                     frameRect.y() + (frameRect.height() - titleH) / 2, titleW, titleH);

    // Center cross: two 12 px lines, faint.
    painter.setPen(QPen(ui::withAlpha(ui::kText, 70), 1));
    const int cx = frameRect.x() + frameRect.width() / 2;
    const int cy = frameRect.y() + frameRect.height() / 2;
    painter.drawLine(cx - 6, cy, cx + 6, cy);
    painter.drawLine(cx, cy - 6, cx, cy + 6);
}

void PreviewCanvas::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    positionOverlays();
}

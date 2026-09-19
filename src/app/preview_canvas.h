#pragma once

#include <QImage>
#include <QWidget>

class QComboBox;
class QFrame;
class QLabel;
class QPainter;
class QRect;
class QToolButton;

// Custom frame canvas. Paints the latest decoded QImage letterboxed into
// the widget with an aspect hint (defaults to 16:9 until a frame arrives).
// This is the custom-canvas approach discussed for the program monitor:
// no QML, no QGraphicsView - one QWidget, one QPainter.
//
// MainWindow reuses this class for the Source monitor, the Program
// monitor and the Quick Mode page, so every opt-in overlay below is
// per-instance and OFF by default:
//   - quality dropdown (#22): self-contained, appears on every canvas
//     instance (accepted; it never affects anything but the paint);
//   - safe-area guides (#21): canvas-paint-only, View-menu toggle is
//     wave-2 wiring;
//   - mini transport (#24): intended for the SOURCE monitor only, the
//     coordinator connects sourceStepRequested/sourcePlayToggled.
class PreviewCanvas : public QWidget {
    Q_OBJECT

public:
    explicit PreviewCanvas(QWidget *parent = nullptr);

    // Aspect ratio hint used before the first frame arrives (w/h).
    void setAspectHint(double aspect);
    double aspectHint() const { return aspectHint_; }

    // Safe-area overlays (suggestion #21): action/title-safe rectangles
    // plus a center cross painted INSIDE the displayed frame rect.
    // Canvas-paint-only by construction - never reaches export.
    void setGuidesVisible(bool on);
    bool guidesVisible() const { return guidesVisible_; }

    // Preview quality (suggestion #22): the frame is downscaled to
    // half/quarter of the displayed width before it is painted back
    // into the SAME displayed rect. Paint-side only - never affects
    // export. Self-contained (no wiring needed).
    enum class Quality { Full, Half, Quarter };
    void setQuality(Quality quality);
    Quality quality() const { return quality_; }

    // Source mini transport (suggestion #24): a small dock at the
    // bottom-center with step/play buttons and a position label.
    // Opt-in per instance (default OFF, fully inert until enabled);
    // buttons only EMIT - playback/stepping is wired by the coordinator
    // for the Source monitor.
    void setMiniTransportVisible(bool on);
    // Out-of-line on purpose: the body dereferences miniTransport_ (a
    // QFrame only forward-declared above). An inline body would require
    // every includer of this header to have pulled <QFrame> FIRST
    // (quick_mode_view.cpp does not - CI preview_canvas.h:58).
    bool miniTransportVisible() const;

    QSize minimumSizeHint() const override;

public slots:
    // Stores a copy of nothing (frame data already belongs to the QImage
    // copy emitted by the decode worker) and schedules a repaint.
    void setFrame(const QImage &frame, double ptsSeconds);

signals:
    // Emitted on user scrub inside the canvas.
    void seekRequested(double seconds);

    // Mini transport (#24): step the SOURCE by +1/-1 frames.
    void sourceStepRequested(int frames);
    // Mini transport (#24): the play button's own checked state.
    void sourcePlayToggled(bool playing);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void buildOverlays();
    void positionOverlays();
    void drawGuides(QPainter &painter, const QRect &frameRect) const;

    QImage current_;
    QPixmap buffer_;
    double aspectHint_ = 16.0 / 9.0;
    double lastPts_ = 0.0;
    bool guidesVisible_ = false;
    Quality quality_ = Quality::Full;
    QComboBox *qualityBox_ = nullptr;
    QFrame *miniTransport_ = nullptr;
    QToolButton *miniPlay_ = nullptr;
    QLabel *miniPos_ = nullptr;
};

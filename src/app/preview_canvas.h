#pragma once

#include <QImage>
#include <QWidget>

// Custom frame canvas. Paints the latest decoded QImage letterboxed into
// the widget with an aspect hint (defaults to 16:9 until a frame arrives).
// This is the custom-canvas approach discussed for the program monitor:
// no QML, no QGraphicsView - one QWidget, one QPainter.
class PreviewCanvas : public QWidget {
    Q_OBJECT

public:
    explicit PreviewCanvas(QWidget *parent = nullptr);

    // Aspect ratio hint used before the first frame arrives (w/h).
    void setAspectHint(double aspect);
    double aspectHint() const { return aspectHint_; }

    QSize minimumSizeHint() const override;

public slots:
    // Stores a copy of nothing (frame data already belongs to the QImage
    // copy emitted by the decode worker) and schedules a repaint.
    void setFrame(const QImage &frame, double ptsSeconds);

signals:
    // Emitted on user scrub inside the canvas.
    void seekRequested(double seconds);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QImage current_;
    QPixmap buffer_;
    double aspectHint_ = 16.0 / 9.0;
    double lastPts_ = 0.0;
};

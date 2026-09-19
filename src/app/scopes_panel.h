#pragma once

#include <QWidget>

#include <QImage>

class QTimer;

namespace fc {

// Video scopes dock (suggestion #38): a compact RGB parade (left) and a
// luma waveform (right) rendered from the same frame the program monitor
// shows.
//
// Throttled by design: setFrame() only stores the latest frame; a 100 ms
// single-shot timer picks it up, so playback analysis runs at ~10 Hz no
// matter how fast frames arrive. The analysis itself works on a copy
// downscaled to <= 320x180 (FastTransformation) - the trace resolution is
// independent of the widget size, and paintEvent just stretches the
// pre-rendered trace images into the widget rect.
//
// Trace buffers are member images reused across analyses (re-created only
// when the analysis geometry changes), keeping per-frame allocation at
// zero in steady state.
class ScopesPanel : public QWidget {
    Q_OBJECT

public:
    explicit ScopesPanel(QWidget *parent = nullptr);

public slots:
    // Queue a program frame for analysis (shallow copy; the throttling
    // timer decides when it is actually processed).
    void setFrame(const QImage &frame);

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    // Analyzes the pending frame into the trace images (or clears them
    // for a null/empty frame).
    void analyzePending();

    static constexpr int kMaxAnalysisW = 320;
    static constexpr int kMaxAnalysisH = 180;
    static constexpr int kLumaBins = 256; // y axis: luma 0..255
    static constexpr int kThrottleMs = 100;
    static constexpr int kParadeChannels = 3;

    QTimer *analysisTimer_ = nullptr;
    QImage pendingFrame_; // latest queued frame (shallow copy)
    bool pendingValid_ = false;

    QImage lumaTrace_;   // anaW_ x kLumaBins, Format_RGBA8888
    QImage paradeTrace_; // kParadeChannels * anaW_ x kLumaBins, Format_RGBA8888
    int anaW_ = 0;
    int anaH_ = 0;
};

} // namespace fc

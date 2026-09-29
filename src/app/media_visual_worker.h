#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <QVector>

namespace fc {

// ---------------------------------------------------------------------------
// MediaVisualWorker: the round-5 timeline visual pump. ONE dedicated
// low-priority worker turns an imported source into the two assets the
// CapCut-style timeline paints:
//
// - a FILMSTRIP: frames sampled evenly over the source duration, tiled
//   into ONE image at 2x devicePixelRatio (the timeline paints it at
//   half size inside a clip body, so it stays sharp on scaled displays);
// - a WAVEFORM: the audio stream decoded at 8 kHz mono and reduced to
//   100 peak buckets per second (per-bucket absolute maximum,
//   peak-normalized at the end).
//
// Jobs run strictly sequentially on the worker's own thread (queued
// slot invocations on one QObject serialize naturally), so a batch of
// imports costs a few background decode passes and never touches the
// playback decoders. Every job opens its own AudioDecoder /
// VideoDecoder and closes it before returning - the class holds no
// state between calls, so MainWindow can queue paths freely and
// re-queue safely.
//
// A job whose source cannot serve the request (a video without an
// audio stream, an audio-only file asked for a filmstrip) completes
// silently: the signal is simply not emitted, and the timeline keeps
// painting its flat fallback for that clip.
// ---------------------------------------------------------------------------
class MediaVisualWorker : public QObject {
    Q_OBJECT

public:
    explicit MediaVisualWorker(QObject *parent = nullptr);

public slots:
    // Sample frames evenly over the source (durationSeconds 0 = use the
    // decoder's own probe) and emit ONE tiled strip image. `keyPath` is
    // what the timeline looks clips up by (the SOURCE path); `decodePath`
    // is what the decoder opens (the proxy when one exists) - the emit
    // is keyed by keyPath either way.
    void makeStrip(const QString &keyPath, const QString &decodePath, double durationSeconds);

    // Decode the whole audio stream and emit 100 peak buckets / second
    // (values 0..1, peak-normalized). Capped at two hours of audio.
    void makeWaveform(const QString &keyPath, const QString &decodePath);

signals:
    void stripReady(const QString &keyPath, const QImage &strip, double durationSeconds);
    void waveformReady(const QString &keyPath, const QVector<float> &peaks, double durationSeconds);

private:
    // One evenly sampled, aspect-filled tile of the strip image.
    QImage tileAt(class VideoDecoder &decoder, double seconds) const;
};

} // namespace fc

#pragma once

#include <QImage>
#include <QObject>
#include <QString>

// Decoding service. Lives on a background QThread (moveToThread) and owns
// the single VideoDecoder instance for the program monitor. Also runs
// proxy-generation jobs sequentially on the same thread so the UI never
// blocks on FFmpeg work.
class DecodeWorker : public QObject {
    Q_OBJECT

public:
    explicit DecodeWorker(QObject *parent = nullptr);
    ~DecodeWorker() override;

public slots:
    // Opens a media file (prefer passing the proxy path when one exists).
    // Emits mediaInfo() and frameReady() with the frame at t=0.
    void open(const QString &path);

    // Displays the frame nearest to `seconds`; seeks when the request is
    // not contiguous with the last decoded position.
    void requestFrame(double seconds);

    // Runs a proxy transcode job (blocking for this worker's thread).
    // Emits proxyProgress(int) and proxyDone(bool, QString) once.
    void runProxyJob(const QString &src, const QString &dst);

    // Stops playback loops and releases the decoder.
    void shutdown();

signals:
    void mediaInfo(const QString &summary, double durationSeconds, double fps, int64_t frameCount);
    void frameReady(const QImage &frame, double ptsSeconds);
    void failed(const QString &error);
    void proxyProgress(int percent);
    void proxyDone(bool ok, const QString &errorOrPath);

private:
    struct Impl;
    Impl *d = nullptr;
};

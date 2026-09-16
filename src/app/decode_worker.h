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
    // `token` rides the mediaInfo() signal back to the caller: it lets
    // MainWindow tie a probe to the exact request that caused it (a
    // stale probe from a superseded open can no longer act on state
    // armed for a newer one).
    void open(const QString &path, qint64 token = 0);

    // opens a media file WITHOUT the open-time t=0 display
    // frame (still emits mediaInfo()). The transition held-frame worker
    // uses this - it only wants the frame it explicitly requests next,
    // not the frame at the file head - and the program source-switch
    // path too (the re-resolved seek paints the switch; an open-time
    // frame 0 would flash for the frames until it lands).
    void openQuiet(const QString &path, qint64 token = 0);

    // Displays the frame nearest to `seconds`; seeks when the request is
    // not contiguous with the last decoded position.
    void requestFrame(double seconds);

    // Runs a proxy transcode job (blocking for this worker's thread).
    // Emits proxyProgress(int) and proxyDone(bool, QString) once.
    void runProxyJob(const QString &src, const QString &dst);

    // Stops playback loops and releases the decoder.
    void shutdown();

signals:
    // hasAudio: the file carries at least one audio stream (the import
    // flow places a matching audio clip on an audio track when it does).
    // token: echoes the open()/openQuiet() request token (0 when the
    // caller did not arm one).
    void mediaInfo(const QString &summary, double durationSeconds, double fps, int64_t frameCount,
                   bool hasAudio, qint64 token);
    void frameReady(const QImage &frame, double ptsSeconds);
    void failed(const QString &error);
    void proxyProgress(int percent);
    void proxyDone(bool ok, const QString &errorOrPath);

private:
    struct Impl;
    Impl *d = nullptr;
};

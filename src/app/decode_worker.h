#pragma once

#include <QImage>
#include <QObject>
#include <QString>

// Decoding service. Lives on a background QThread (moveToThread) and owns
// the single VideoDecoder instance for the program monitor. Also runs
// proxy-generation jobs sequentially on the same thread so the UI never
// blocks on FFmpeg work.
//
// Playback smoothness contract (round 4): requestFrame() is LATEST-WINS.
// During playback the tick queues a request every clock beat; when the
// decoder is slower than the beat, the old queued-event chain decoded
// every stale target back-to-back and the picture lagged further and
// further behind the audio clock. Now the request only stores the newest
// target and schedules ONE drain job - the decoder always works toward
// the freshest position and a backlog can no longer build.
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

    // Display the frame nearest to `seconds`. Thread-safe from any
    // thread: coalesces into at most one queued drain, keeping only the
    // newest target (see the class contract above).
    void requestFrame(double seconds);

    // Output scale for the preview pipeline: 1.0 = full resolution,
    // 0.5 / 0.25 = the decoder scales frames on the way out so reduced
    // preview quality cuts real decode-to-paint work. Never affects
    // export (the exporter renders through its own pipeline).
    void setOutputScale(double factor);

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
    // This signal crosses threads via QUEUED connections, so every
    // argument must be a Qt metatype by NAME: qint64 registers as a
    // builtin everywhere, while a raw int64_t only coincides with it on
    // Windows (Linux g++ resolves int64_t to long - unregistered - and
    // the queued delivery aborts with "Cannot queue arguments").
    void mediaInfo(const QString &summary, double durationSeconds, double fps, qint64 frameCount,
                   bool hasAudio, qint64 token);
    void frameReady(const QImage &frame, double ptsSeconds);
    void failed(const QString &error);
    void proxyProgress(int percent);
    void proxyDone(bool ok, const QString &errorOrPath);

private slots:
    // The single coalesced decode job (see the class contract): marks
    // the queue empty, then decodes toward the newest requested target.
    void drainQueuedSlot();

private:
    // Worker-thread body: decodes the frame nearest `seconds` and emits
    // it. The drain loop and the open paths call this directly.
    void decodeAt(double seconds);

    struct Impl;
    Impl *d = nullptr;
};

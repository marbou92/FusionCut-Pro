#include "decode_worker.h"

#include <atomic>
#include <cmath>

#include <QMetaObject>

#include "media_item.h"
#include "media_probe.h"
#include "proxy_generator.h"
#include "video_decoder.h"

struct DecodeWorker::Impl {
    fc::VideoDecoder decoder;
    QString openPath;
    double lastPts = -1.0;
    double fps = 0.0;

    // Latest-wins request state (see the class contract in the header).
    // pendingTarget: the newest requested position, -1 when consumed.
    // drainQueued: a drain job is queued/running. Both are touched from
    // the GUI thread (requestFrame) and the worker thread (drain), so
    // both are atomics; the flag protocol is the standard single-slot
    // coalescing pattern - a request that lands while drainQueued is
    // true is guaranteed to be seen by the running/queued drain's loop,
    // and a request that lands after the drain consumed everything
    // finds drainQueued false and queues a fresh drain.
    std::atomic<double> pendingTarget{-1.0};
    std::atomic<bool> drainQueued{false};
};

DecodeWorker::DecodeWorker(QObject *parent) : QObject(parent), d(new Impl) {}

DecodeWorker::~DecodeWorker() {
    delete d;
}

void DecodeWorker::open(const QString &path, qint64 token) {
    std::string error;
    if (!d->decoder.open(path.toStdString(), error)) {
        d->openPath.clear();
        d->pendingTarget = -1.0; // stale display requests must not outlive the open
        emit failed(QString::fromStdString(error));
        return;
    }
    d->openPath = path;
    d->lastPts = -1.0;
    d->fps = d->decoder.info().video.frameRate.toDouble();
    if (d->fps <= 0.0) {
        d->fps = 24.0;
    }

    const fc::MediaInfo &info = d->decoder.info();
    emit mediaInfo(fc::mediaSummary(info), info.durationSeconds(), d->fps, info.video.frameCount,
                   !info.audioStreams.empty(), token);
    requestFrame(0.0);
}

void DecodeWorker::openQuiet(const QString &path, qint64 token) {
    std::string error;
    if (!d->decoder.open(path.toStdString(), error)) {
        d->openPath.clear();
        d->pendingTarget = -1.0;
        emit failed(QString::fromStdString(error));
        return;
    }
    d->openPath = path;
    d->lastPts = -1.0;
    d->fps = d->decoder.info().video.frameRate.toDouble();
    if (d->fps <= 0.0) {
        d->fps = 24.0;
    }

    const fc::MediaInfo &info = d->decoder.info();
    emit mediaInfo(fc::mediaSummary(info), info.durationSeconds(), d->fps, info.video.frameCount,
                   !info.audioStreams.empty(), token);
}

void DecodeWorker::requestFrame(double seconds) {
    if (d->openPath.isEmpty()) {
        return;
    }
    // Store the newest target, then make sure exactly one drain job is
    // queued. requestFrame runs on the GUI thread (via invokeMethod)
    // and on the worker thread (open()'s t=0 display request); the
    // atomic protocol above keeps both correct.
    d->pendingTarget.store(seconds);
    if (!d->drainQueued.exchange(true)) {
        QMetaObject::invokeMethod(this, "drainQueuedSlot", Qt::QueuedConnection);
    }
}

void DecodeWorker::setOutputScale(double factor) {
    // Clamped hard: only the documented preview scales are meaningful,
    // and an off-list value would just waste scale re-configurations.
    if (factor >= 0.9) {
        factor = 1.0;
    } else if (factor >= 0.4) {
        factor = 0.5;
    } else {
        factor = 0.25;
    }
    d->decoder.setOutputScale(factor);
}

void DecodeWorker::drainQueuedSlot() {
    // Worker thread. Clear the flag FIRST: a request that arrives after
    // this point sees drainQueued == false and queues a fresh drain, so
    // nothing can be stranded; a request that arrived before it is
    // picked up by the loop below (the flag is what makes N queued
    // requests collapse into at most one decode per fresh arrival).
    d->drainQueued.store(false);
    while (true) {
        const double target = d->pendingTarget.exchange(-1.0);
        if (target < 0.0) {
            break;
        }
        decodeAt(target);
    }
}

void DecodeWorker::decodeAt(double seconds) {
    if (d->openPath.isEmpty()) {
        return;
    }
    std::string error;
    const bool contiguous = seconds >= d->lastPts - 0.001 && seconds <= d->lastPts + 1.0;
    if (!contiguous && !d->decoder.seekToSeconds(seconds, error)) {
        emit failed(QString::fromStdString(error));
        return;
    }

    const double tolerance = 0.5 / d->fps;
    fc::DecodedFrame frame;
    while (d->decoder.readFrame(frame, error)) {
        // Contiguous reads scan forward past frames that end before the
        // requested position; after a seek the decoder already positioned
        // us at the first frame at/after the target, so take it directly.
        if (contiguous && frame.ptsSeconds + tolerance < seconds) {
            continue;
        }
        const QImage image(frame.rgba.data(), frame.width, frame.height, frame.width * 4,
                           QImage::Format_RGBA8888);
        d->lastPts = frame.ptsSeconds;
        emit frameReady(image.copy(), frame.ptsSeconds);
        return;
    }
    if (!error.empty()) {
        emit failed(QString::fromStdString(error));
    }
    // Clean end of file: no frame emitted.
}

void DecodeWorker::runProxyJob(const QString &src, const QString &dst) {
    fc::ProxyConfig config;
    std::string error;
    const bool ok = fc::ProxyGenerator::generate(
        src.toStdString(), dst.toStdString(), config,
        [&error, this](double fraction) -> bool {
            emit proxyProgress(static_cast<int>(fraction * 100.0));
            return true;
        },
        error);
    // An empty error means the caller cancelled (the ProxyGenerator
    // contract mirrors the exporter's) - name that instead of showing a
    // bare "Proxy failed:" line.
    const QString result = ok ? dst
                              : (error.empty() ? QStringLiteral("cancelled by caller")
                                               : QString::fromStdString(error));
    emit proxyDone(ok, result);
}

void DecodeWorker::shutdown() {
    d->decoder.close();
    d->openPath.clear();
    d->lastPts = -1.0;
    d->pendingTarget = -1.0;
}

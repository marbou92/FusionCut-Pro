#include "decode_worker.h"

#include <cmath>

#include "media_probe.h"
#include "proxy_generator.h"
#include "video_decoder.h"

namespace {

// Human-readable one-liner for a probed file (project panel + status).
QString summarize(const fc::MediaInfo &info) {
    if (!info.hasVideo) {
        return QString::fromStdString("audio only");
    }
    return QString("%1x%2 %3, %4 fps, %5s")
        .arg(info.video.width)
        .arg(info.video.height)
        .arg(QString::fromStdString(info.video.codecName))
        .arg(QString::number(info.video.frameRate.toDouble(), 'f', 3))
        .arg(QString::number(info.durationSeconds(), 'f', 1));
}

} // namespace

struct DecodeWorker::Impl {
    fc::VideoDecoder decoder;
    QString openPath;
    double lastPts = -1.0;
    double fps = 0.0;
};

DecodeWorker::DecodeWorker(QObject *parent) : QObject(parent), d(new Impl) {}

DecodeWorker::~DecodeWorker() {
    delete d;
}

void DecodeWorker::open(const QString &path) {
    std::string error;
    if (!d->decoder.open(path.toStdString(), error)) {
        d->openPath.clear();
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
    emit mediaInfo(summarize(info), info.durationSeconds(), d->fps, info.video.frameCount);
    requestFrame(0.0);
}

void DecodeWorker::requestFrame(double seconds) {
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
    emit proxyDone(ok, ok ? dst : QString::fromStdString(error));
}

void DecodeWorker::shutdown() {
    d->decoder.close();
    d->openPath.clear();
    d->lastPts = -1.0;
}

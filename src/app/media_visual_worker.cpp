#include "media_visual_worker.h"

#include <QImage>
#include <QPainter>

#include <algorithm>
#include <cmath>
#include <vector>

#include "audio_decoder.h"
#include "video_decoder.h"

namespace fc {

namespace {

// Strip geometry: kStripFrames tiles at 2x devicePixelRatio. 96 px tall
// tiles paint into a 48 px clip body (the round-5 timeline lane is 56 px
// with a 4 px inset top and bottom); 180 px logical width covers a
// 16:9 tile, and odd aspect ratios are center-cropped to fill.
constexpr int kStripFrames = 8;
constexpr int kTilePx = 96;        // device pixels (painted at 48 logical)
constexpr int kTileLogicalW = 180; // logical tile width
constexpr double kWaveformRate = 8000.0;
constexpr double kPeakBucketsPerSecond = 100.0;
constexpr double kWaveformCapSeconds = 2.0 * 3600.0; // decode guard

QImage rgbaToImage(const DecodedFrame &frame) {
    if (frame.width <= 0 || frame.height <= 0 || frame.rgba.empty()) {
        return QImage();
    }
    // One copy takes ownership of the decode buffer (the QImage ctor
    // with external data does not).
    QImage img(frame.rgba.data(), frame.width, frame.height, static_cast<int>(frame.width) * 4,
               QImage::Format_RGBA8888);
    return img.copy();
}

// Aspect-FILL one source frame into a kTilePx x kTilePx tile, center
// crop: filmstrip tiles never letterbox (CapCut-style full-bleed).
QImage tileFromFrame(const QImage &frame) {
    if (frame.isNull()) {
        return QImage();
    }
    QImage scaled = frame.scaled(kTileLogicalW * 2, kTilePx, Qt::KeepAspectRatioByExpanding,
                                 Qt::SmoothTransformation);
    const int x0 = std::max(0, (scaled.width() - kTileLogicalW * 2) / 2);
    return scaled.copy(x0, 0, kTileLogicalW * 2, kTilePx);
}

} // namespace

MediaVisualWorker::MediaVisualWorker(QObject *parent) : QObject(parent) {
    // Queued connections cross the thread boundary: the waveform's peak
    // array is a QVector<float>, which is NOT in Qt's built-in metatype
    // set on every 5.12/5.15 build - register it explicitly so the
    // signal can never silently drop.
    qRegisterMetaType<QVector<float>>("QVector<float>");
}

void MediaVisualWorker::makeStrip(const QString &keyPath, const QString &decodePath,
                                  double durationSeconds) {
    VideoDecoder decoder;
    std::string error;
    if (!decoder.open(decodePath.toStdString(), error)) {
        return; // audio-only source, undecodable file: flat fallback stays
    }
    double duration = durationSeconds;
    if (duration <= 0.0) {
        duration = decoder.info().durationSeconds();
    }
    if (duration <= 0.0) {
        return;
    }

    // Sample at (i + 0.5) / N * duration: frame 0 is frequently black
    // slate, so the first tile lands mid-first-segment instead.
    QImage strip(kTileLogicalW * 2 * kStripFrames, kTilePx, QImage::Format_RGBA8888);
    strip.fill(0x20); // dark base shows through if a tile fails
    int filled = 0;
    for (int i = 0; i < kStripFrames; ++i) {
        const double at = (double(i) + 0.5) * duration / double(kStripFrames);
        const QImage tile = tileAt(decoder, at);
        if (tile.isNull()) {
            continue;
        }
        QPainter p(&strip);
        p.drawImage(i * kTileLogicalW * 2, 0, tile);
        ++filled;
    }
    if (filled == 0) {
        return;
    }
    strip.setDevicePixelRatio(2.0); // painted at half size, stays sharp
    emit stripReady(keyPath, strip, duration);
}

QImage MediaVisualWorker::tileAt(VideoDecoder &decoder, double seconds) const {
    std::string error;
    if (!decoder.seekToSeconds(seconds, error)) {
        return QImage();
    }
    DecodedFrame frame;
    if (!decoder.readFrame(frame, error)) {
        return QImage();
    }
    return tileFromFrame(rgbaToImage(frame));
}

void MediaVisualWorker::makeWaveform(const QString &keyPath, const QString &decodePath) {
    AudioDecoder decoder;
    std::string error;
    if (!decoder.open(decodePath.toStdString(), 8000, 1, error)) {
        return; // no audio stream (or undecodable): silence is the truth
    }

    // 100 buckets / second at 8 kHz = 80 samples per bucket. Running
    // per-bucket |max|, normalized by the file's own peak at the end -
    // a quiet phone video still shows a full-height, honest waveform.
    const int bucketSamples = 80;
    std::vector<float> buckets;
    double totalFrames = 0.0;
    float filePeak = 0.0f;
    DecodedAudio chunk;
    while (decoder.readSamples(chunk, error)) {
        const double chunkStart = chunk.ptsSeconds * kWaveformRate;
        for (int i = 0; i < chunk.frames; ++i) {
            const double samplePos = chunkStart + double(i);
            const size_t bucket = size_t(std::max(0.0, samplePos / double(bucketSamples)));
            if (bucket >= buckets.size()) {
                buckets.resize(bucket + 1, 0.0f);
            }
            const float v = std::fabs(chunk.samples[size_t(i)]);
            buckets[bucket] = std::max(buckets[bucket], v);
            filePeak = std::max(filePeak, v);
        }
        totalFrames = std::max(totalFrames, chunkStart + double(chunk.frames));
        if (totalFrames / kWaveformRate > kWaveformCapSeconds) {
            break; // absurdly long source: render what we have
        }
    }
    if (buckets.empty() || totalFrames <= 0.0) {
        return;
    }
    const double durationSec = totalFrames / kWaveformRate;
    const float norm = filePeak > 1e-6f ? 1.0f / filePeak : 0.0f;
    QVector<float> peaks(buckets.size());
    for (size_t i = 0; i < buckets.size(); ++i) {
        peaks[int(i)] = std::min(1.0f, buckets[i] * norm);
    }
    emit waveformReady(keyPath, peaks, durationSec);
}

} // namespace fc

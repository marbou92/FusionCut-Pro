// FusionCut Pro - media layer integration tests.
//
// Generates synthetic media at runtime (no binary assets in the repo),
// then exercises probe -> decode -> proxy -> re-probe against it.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include "test_harness.h"

#include "exporter.h"
#include "media_probe.h"
#include "proxy_generator.h"
#include "video_decoder.h"

#include "media_test_util.h"

namespace fs = std::filesystem;

namespace {

fs::path testDir() {
    std::error_code ec;
    fs::path dir = fs::temp_directory_path() / "fc_media_tests";
    if (fs::exists(dir) && !fs::is_directory(dir)) {
        fs::remove(dir, ec); // a stale file squatting on our directory name
    }
    fs::create_directories(dir, ec);
    if (ec || !fs::is_directory(dir)) {
        dir = fs::temp_directory_path(); // fall back to the temp root
    }
    return dir;
}

std::string pathOf(const char *name) {
    return (testDir() / name).string();
}

// Red-minus-blue channel difference at a pixel (positive = red dominant).
int redMinusBlueAt(const fc::DecodedFrame &frame, int x, int y) {
    const size_t idx = (static_cast<size_t>(y) * frame.width + x) * 4;
    return static_cast<int>(frame.rgba[idx]) - static_cast<int>(frame.rgba[idx + 2]);
}

void testProbe() {
    const std::string file = pathOf("probe_src.mp4");
    std::string error;
    fc::TestMediaSpec spec;
    spec.width = 640;
    spec.height = 360;
    spec.fps = 25;
    spec.seconds = 4.0;
    CHECK(fc::generateTestVideo(file, spec, error));

    fc::MediaInfo info;
    CHECK(fc::MediaProbe::probe(file, info, error));
    CHECK(info.hasVideo);
    CHECK(info.video.width == 640);
    CHECK(info.video.height == 360);
    CHECK(info.video.codecName == "h264" || info.video.codecName == "mpeg4");
    CHECK(info.video.frameRate.num == 25);
    CHECK(info.video.frameRate.den == 1);
    CHECK(info.video.frameCount >= 99 && info.video.frameCount <= 101);
    CHECK(std::fabs(info.durationSeconds() - 4.0) < 0.2);
    if (info.audioStreams.empty()) {
        CHECK(!"expected at least one audio stream");
    } else {
        CHECK(info.audioStreams[0].channels == 2);
        CHECK(info.audioStreams[0].sampleRate == 48000);
    }

    // Errors on garbage path.
    fc::MediaInfo bad;
    CHECK(!fc::MediaProbe::probe("/nonexistent/fc/does-not-exist.mp4", bad, error));
    CHECK(!error.empty());
}

void testSequentialDecode() {
    const std::string file = pathOf("decode_src.mp4");
    std::string error;
    fc::TestMediaSpec spec;
    spec.width = 320;
    spec.height = 240;
    spec.fps = 10;
    spec.seconds = 3.0; // 30 frames: red second, blue second, red second
    spec.colorA[0] = 200;
    spec.colorA[1] = 30;
    spec.colorA[2] = 30;
    spec.colorB[0] = 30;
    spec.colorB[1] = 30;
    spec.colorB[2] = 200;
    spec.withAudio = false;
    CHECK(fc::generateTestVideo(file, spec, error));

    fc::VideoDecoder decoder;
    CHECK(decoder.open(file, error));

    fc::DecodedFrame frame;
    int count = 0;
    double lastPts = -1.0;
    int firstFrameRedMinusBlue = 0;
    int midFrameRedMinusBlue = 0;
    while (decoder.readFrame(frame, error)) {
        CHECK(frame.width == 320);
        CHECK(frame.height == 240);
        CHECK(frame.rgba.size() == static_cast<size_t>(320) * 240 * 4);
        CHECK(frame.ptsSeconds >= lastPts - 1e-9); // monotonic
        lastPts = frame.ptsSeconds;
        if (count == 0) {
            firstFrameRedMinusBlue = redMinusBlueAt(frame, 160, 120);
        } else if (count == 15) {
            midFrameRedMinusBlue = redMinusBlueAt(frame, 160, 120);
        }
        ++count;
    }
    CHECK(error.empty()); // clean EOF leaves error untouched
    CHECK(count == 30);
    // Second 1 is red-dominant (R >> B), second 2 is blue-dominant.
    CHECK(firstFrameRedMinusBlue > 60);
    CHECK(midFrameRedMinusBlue < -60);
}

void testSeek() {
    const std::string file = pathOf("seek_src.mp4");
    std::string error;
    fc::TestMediaSpec spec;
    spec.width = 320;
    spec.height = 240;
    spec.fps = 10;
    spec.seconds = 3.0;
    spec.withAudio = false;
    CHECK(fc::generateTestVideo(file, spec, error));

    fc::VideoDecoder decoder;
    CHECK(decoder.open(file, error));

    fc::DecodedFrame frame;
    int count = 0;
    while (decoder.readFrame(frame, error)) {
        ++count;
    }
    CHECK(count == 30);

    // Seek back to the middle, then decode to the end again.
    CHECK(decoder.seekToSeconds(1.5, error));
    count = 0;
    while (decoder.readFrame(frame, error)) {
        ++count;
    }
    // Expect roughly the back half (15 frames) - allow keyframe slack.
    CHECK(count >= 13 && count <= 17);
    CHECK(count < 30);
}

void testProxyGeneration() {
    const std::string src = pathOf("proxy_src.mp4");
    const std::string dst = pathOf("proxy_dst.mp4");
    std::string error;
    fc::TestMediaSpec spec;
    spec.width = 1280;
    spec.height = 720;
    spec.fps = 25;
    spec.seconds = 3.0;
    spec.withAudio = true;
    CHECK(fc::generateTestVideo(src, spec, error));

    fc::ProxyConfig config; // defaults: 360p, crf 28, with audio
    double lastProgress = -1.0;
    int progressCalls = 0;
    const bool ok = fc::ProxyGenerator::generate(
        src, dst, config,
        [&](double fraction) {
            ++progressCalls;
            CHECK(fraction >= lastProgress - 1e-9); // monotonic progress
            CHECK(fraction >= 0.0 && fraction <= 1.0);
            lastProgress = fraction;
            return true;
        },
        error);
    CHECK(ok);
    CHECK(progressCalls >= 70); // ~75 frames should each report progress

    fc::MediaInfo proxyInfo;
    CHECK(fc::MediaProbe::probe(dst, proxyInfo, error));
    CHECK(proxyInfo.hasVideo);
    CHECK(proxyInfo.video.height == 360); // 1280x720 -> 640x360
    CHECK(proxyInfo.video.width == 640);
    CHECK(proxyInfo.video.codecName == "h264" || proxyInfo.video.codecName == "mpeg4");
    CHECK(std::fabs(proxyInfo.durationSeconds() - 3.0) < 0.3);
    CHECK(!proxyInfo.audioStreams.empty()); // audio survived the proxy

    // The proxy decodes end to end.
    fc::VideoDecoder decoder;
    CHECK(decoder.open(dst, error));
    fc::DecodedFrame frame;
    int frames = 0;
    while (decoder.readFrame(frame, error)) {
        ++frames;
    }
    CHECK(frames >= 73 && frames <= 77); // ~3s at 25fps
}

void testProxyNeverUpscales() {
    const std::string src = pathOf("tiny_src.mp4");
    const std::string dst = pathOf("tiny_dst.mp4");
    std::string error;
    fc::TestMediaSpec spec;
    spec.width = 320;
    spec.height = 180;
    spec.fps = 10;
    spec.seconds = 1.0;
    spec.withAudio = false;
    CHECK(fc::generateTestVideo(src, spec, error));

    fc::ProxyConfig config; // target 360 > source 180 -> keep 180
    CHECK(fc::ProxyGenerator::generate(src, dst, config, nullptr, error));

    fc::MediaInfo info;
    CHECK(fc::MediaProbe::probe(dst, info, error));
    CHECK(info.video.height == 180);
    CHECK(info.video.width == 320);
}

// ---- M5 Phase 3: the export pipeline ------------------------------------

// Deterministic 64x36 pattern: per-frame blue ramp + moving white column.
void fillExportFrame(int64_t frame, uint8_t *rgba, int w, int h) {
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            uint8_t *p = rgba + (static_cast<size_t>(y) * w + x) * 4;
            p[0] = static_cast<uint8_t>((x * 4) & 0xFF);
            p[1] = static_cast<uint8_t>((y * 7) & 0xFF);
            p[2] = static_cast<uint8_t>((frame * 8) & 0xFF);
            p[3] = 255;
        }
    }
    const int marker = static_cast<int>(frame % 64);
    for (int y = 0; y < h; ++y) {
        uint8_t *p = rgba + (static_cast<size_t>(y) * 64 + marker) * 4;
        p[0] = p[1] = p[2] = 255;
    }
}

void testExport() {
    const std::string dst = pathOf("export_out.mp4");
    std::string error;

    fc::ExportConfig config;
    config.width = 64;
    config.height = 36;
    config.fps = 12.0;
    config.totalFrames = 30;
    config.crf = 18; // high quality: the pixel assertions stay tight

    double lastProgress = -1.0;
    int progressCalls = 0;
    const bool ok = fc::Exporter::run(
        dst, config,
        [](int64_t frame, uint8_t *rgba) {
            fillExportFrame(frame, rgba, 64, 36);
            return true;
        },
        [&](double fraction) {
            ++progressCalls;
            CHECK(fraction >= lastProgress - 1e-9);
            CHECK(fraction > 0.0 && fraction <= 1.0);
            lastProgress = fraction;
            return true;
        },
        error);
    CHECK(ok);
    if (!ok) {
        std::printf("export failed: %s\n", error.c_str());
        return;
    }
    CHECK(progressCalls == 30);

    // Geometry + duration via the probe.
    fc::MediaInfo info;
    CHECK(fc::MediaProbe::probe(dst, info, error));
    CHECK(info.hasVideo);
    CHECK(info.video.width == 64);
    CHECK(info.video.height == 36);
    CHECK(std::fabs(info.durationSeconds() - 2.5) < 0.3);

    // Frame count + content round-trip through the decoder.
    fc::VideoDecoder decoder;
    CHECK(decoder.open(dst, error));
    fc::DecodedFrame frame;
    int frames = 0;
    bool sawMarker = false;
    while (decoder.readFrame(frame, error)) {
        if (frames == 0) {
            // First frame: red channel follows x*4 (luma-dominant, tight
            // tolerance after the yuv420p round-trip).
            const size_t idx = (static_cast<size_t>(18) * frame.width + 16) * 4;
            CHECK(std::abs(static_cast<int>(frame.rgba[idx]) - 64) <= 25);
        }
        // The moving marker column is white in frame 0 at x = 0.
        if (frames == 0) {
            const size_t idx = (static_cast<size_t>(18) * frame.width) * 4;
            const int r = frame.rgba[idx];
            const int g = frame.rgba[idx + 1];
            const int b = frame.rgba[idx + 2];
            if (r > 200 && g > 200 && b > 200) {
                sawMarker = true;
            }
        }
        ++frames;
    }
    CHECK(frames == 30);
    CHECK(sawMarker);
}

void testExportOddDimsAndCancellation() {
    std::string error;

    // Odd dimensions are evened down (yuv420p needs even geometry); the
    // provider renders at the evened size.
    {
        const std::string dst = pathOf("export_odd.mp4");
        fc::ExportConfig config;
        config.width = 65;
        config.height = 37;
        config.fps = 10.0;
        config.totalFrames = 4;
        CHECK(fc::Exporter::run(
            dst, config,
            [](int64_t, uint8_t *rgba) {
                for (int y = 0; y < 36; ++y) {
                    for (int x = 0; x < 64; ++x) {
                        uint8_t *p = rgba + (static_cast<size_t>(y) * 64 + x) * 4;
                        p[0] = p[1] = p[2] = 90;
                        p[3] = 255;
                    }
                }
                return true;
            },
            nullptr, error));
        fc::MediaInfo info;
        CHECK(fc::MediaProbe::probe(dst, info, error));
        CHECK(info.video.width == 64);
        CHECK(info.video.height == 36);
    }

    // Provider cancellation: no error text, partial file removed.
    {
        const std::string dst = pathOf("export_cancel.mp4");
        fc::ExportConfig config;
        config.width = 64;
        config.height = 36;
        config.fps = 12.0;
        config.totalFrames = 30;
        int provided = 0;
        const bool ok = fc::Exporter::run(
            dst, config,
            [&](int64_t, uint8_t *rgba) {
                ++provided;
                fillExportFrame(0, rgba, 64, 36);
                return provided < 6;
            },
            nullptr, error);
        CHECK(!ok);
        CHECK(error.empty());
        CHECK(provided == 6);
        std::error_code ec;
        CHECK(!fs::exists(dst, ec)); // partial output cleaned up
    }

    // Progress-cancellation path.
    {
        const std::string dst = pathOf("export_cancel2.mp4");
        fc::ExportConfig config;
        config.width = 64;
        config.height = 36;
        config.fps = 12.0;
        config.totalFrames = 30;
        int calls = 0;
        const bool ok = fc::Exporter::run(
            dst, config,
            [](int64_t frame, uint8_t *rgba) {
                fillExportFrame(frame, rgba, 64, 36);
                return true;
            },
            [&](double) { return ++calls < 4; }, error);
        CHECK(!ok);
        CHECK(error.empty());
        CHECK(calls == 4);
        std::error_code ec;
        CHECK(!fs::exists(dst, ec));
    }
}

void testProxyCancellation() {
    const std::string src = pathOf("cancel_src.mp4");
    const std::string dst = pathOf("cancel_dst.mp4");
    std::string error;
    fc::TestMediaSpec spec;
    spec.width = 640;
    spec.height = 480;
    spec.fps = 25;
    spec.seconds = 4.0;
    spec.withAudio = false;
    CHECK(fc::generateTestVideo(src, spec, error));

    fc::ProxyConfig config;
    int calls = 0;
    const bool ok = fc::ProxyGenerator::generate(
        src, dst, config,
        [&](double) {
            ++calls;
            return calls < 10; // cancel after 10 frames
        },
        error);
    CHECK(!ok);
    CHECK(!error.empty());
    CHECK(calls == 10);
}

} // namespace

int main() {
    std::printf("FFmpeg: %s\n", fc::ffmpegVersionInfo().c_str());

    testProbe();
    testSequentialDecode();
    testSeek();
    testProxyGeneration();
    testProxyNeverUpscales();
    testProxyCancellation();
    testExport();
    testExportOddDimsAndCancellation();

    return testExitCode("media");
}

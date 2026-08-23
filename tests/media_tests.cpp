// FusionCut Pro - media layer integration tests.
//
// Generates synthetic media at runtime (no binary assets in the repo),
// then exercises probe -> decode -> proxy -> re-probe against it.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include "test_harness.h"

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

    return testExitCode("media");
}

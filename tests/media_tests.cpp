// FusionCut Pro - media layer integration tests.
//
// Generates synthetic media at runtime (no binary assets in the repo),
// then exercises probe -> decode -> proxy -> re-probe against it.

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <string>

#include "test_harness.h"

#include "audio_decoder.h"
#include "audio_math.h"
#include "audio_mixer.h"
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

// ---- the export pipeline ------------------------------------

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

// ---------------------------------------------------------------------------
// Audio: decoder, window mixer, and the export's AAC track. The
// synthetic generator writes a 440 Hz tone at 0.4 amplitude, 48 kHz
// stereo - every assertion below leans on that.
// ---------------------------------------------------------------------------

// Goertzel magnitude at `freqHz` over the first n samples (per channel
// 0 of an interleaved stereo buffer).
static double goertzel(const std::vector<float> &stereo, double freqHz, int rate) {
    const size_t n = stereo.size() / 2;
    const double k = std::round(n * freqHz / rate);
    const double w = 2.0 * 3.141592653589793 * k / n;
    const double coeff = 2.0 * std::cos(w);
    double q1 = 0.0, q2 = 0.0;
    for (size_t i = 0; i < n; ++i) {
        const double v = stereo[2 * i];
        const double q0 = coeff * q1 - q2 + v;
        q2 = q1;
        q1 = q0;
    }
    return std::sqrt(q1 * q1 + q2 * q2 - coeff * q1 * q2) / (n * 0.2);
}

// RMS amplitude of an interleaved stereo window.
static double rms(const std::vector<float> &stereo) {
    if (stereo.empty()) {
        return 0.0;
    }
    double acc = 0.0;
    for (float v : stereo) {
        acc += double(v) * double(v);
    }
    return std::sqrt(acc / stereo.size());
}

void testAudioDecoder() {
    const std::string file = pathOf("audio_src.mp4");
    std::string error;
    fc::TestMediaSpec spec;
    spec.width = 320;
    spec.height = 180;
    spec.fps = 24;
    spec.seconds = 3.0;
    spec.audioHz = 440;
    CHECK(fc::generateTestVideo(file, spec, error));

    // Geometry: converted to the requested rate/channels.
    fc::AudioDecoder decoder;
    CHECK(decoder.open(file, 48000, 2, error));
    CHECK(decoder.sampleRate() == 48000);
    CHECK(decoder.channels() == 2);

    // Read everything forward: ~3 s of samples, tone-shaped.
    std::vector<float> all;
    double firstPts = -1.0;
    double lastPts = 0.0;
    fc::DecodedAudio chunk;
    int chunks = 0;
    while (decoder.readSamples(chunk, error)) {
        CHECK(chunk.frames > 0);
        CHECK(chunk.samples.size() == size_t(chunk.frames) * 2);
        CHECK(chunk.samples.size() % 2 == 0);
        if (firstPts < 0.0) {
            firstPts = chunk.ptsSeconds;
        }
        lastPts = chunk.ptsSeconds + double(chunk.frames) / 48000.0;
        all.insert(all.end(), chunk.samples.begin(), chunk.samples.end());
        ++chunks;
    }
    CHECK(error.empty());               // clean end of stream
    CHECK(chunks >= 100);               // AAC frames of ~1024 samples over 3 s
    CHECK(all.size() >= 2 * 48000 * 2); // at least 2 seconds of samples
    CHECK(all.size() <= 2 * 48000 * 4);
    CHECK(std::fabs(firstPts) < 0.2);
    CHECK(std::fabs(lastPts - 3.0) < 0.3);
    CHECK(rms(all) > 0.15); // the 0.4-amplitude tone survives
    CHECK(rms(all) < 0.6);
    CHECK(goertzel(all, 440.0, 48000) > 0.5); // the tone dominates

    // A different target rate resamples: 44.1 kHz also finds the tone.
    fc::AudioDecoder resampled;
    CHECK(resampled.open(file, 44100, 2, error));
    std::vector<float> mixed441;
    while (resampled.readSamples(chunk, error)) {
        mixed441.insert(mixed441.end(), chunk.samples.begin(), chunk.samples.end());
    }
    CHECK(mixed441.size() >= 2 * 44100 * 2);
    CHECK(goertzel(mixed441, 440.0, 44100) > 0.5);

    // Mono folds both channels.
    fc::AudioDecoder mono;
    CHECK(mono.open(file, 48000, 1, error));
    CHECK(mono.readSamples(chunk, error));
    CHECK(chunk.samples.size() == size_t(chunk.frames) * 1);

    // Seek mid-file: the stream resumes near the target.
    fc::AudioDecoder seeking;
    CHECK(seeking.open(file, 48000, 2, error));
    CHECK(seeking.seekToSeconds(2.0, error));
    CHECK(seeking.readSamples(chunk, error));
    CHECK(chunk.ptsSeconds >= 1.9);
    CHECK(chunk.ptsSeconds < 2.1);

    // Files without an audio stream refuse to open.
    fc::TestMediaSpec noAudio = spec;
    noAudio.withAudio = false;
    const std::string silent = pathOf("audio_none.mp4");
    CHECK(fc::generateTestVideo(silent, noAudio, error));
    fc::AudioDecoder none;
    CHECK(!none.open(silent, 48000, 2, error));
    CHECK(!error.empty());
}

void testAudioWindowMixer() {
    const std::string file = pathOf("mix_src.mp4");
    std::string error;
    fc::TestMediaSpec spec;
    spec.width = 320;
    spec.height = 180;
    spec.fps = 24;
    spec.seconds = 2.0;
    spec.audioHz = 440;
    CHECK(fc::generateTestVideo(file, spec, error));

    std::vector<fc::AudioSpan> spans;
    fc::AudioSpan span;
    span.path = file;
    span.srcStartSec = 0.0;
    span.startSec = 0.0;
    span.endSec = 1.0;
    span.gain = 1.0;
    spans.push_back(span);

    // Silence outside the span.
    fc::AudioWindowMixer mixer(48000, 2);
    std::vector<float> window(2 * 4800, 1.0f);                   // 100 ms, poisoned
    CHECK(mixer.pull(spans, 48000, 4800, window.data(), error)); // [1.0, 1.1) s
    for (float v : window) {
        CHECK(v == 0.0f);
    }

    // Inside the span: the tone is there.
    std::vector<float> in(2 * 4800);
    CHECK(mixer.pull(spans, 24000, 4800, in.data(), error)); // [0.5, 0.6) s
    CHECK(rms(in) > 0.1);
    CHECK(goertzel(in, 440.0, 48000) > 0.4);

    // Sequential monotonic windows keep working (rolling decode):
    // [0.6, 1.0) s in 4 steps, all inside the span.
    for (int w = 0; w < 4; ++w) {
        CHECK(mixer.pull(spans, 28800 + w * 4800, 4800, in.data(), error));
        CHECK(rms(in) > 0.1);
    }

    // A backwards jump re-seeks (the same span again).
    CHECK(mixer.pull(spans, 4800, 4800, in.data(), error));
    CHECK(rms(in) > 0.1);

    // Gain 0.5 halves the amplitude.
    fc::AudioWindowMixer half(48000, 2);
    spans[0].gain = 0.5;
    CHECK(half.pull(spans, 0, 4800, in.data(), error));
    const double full = [&]() {
        fc::AudioWindowMixer one(48000, 2);
        spans[0].gain = 1.0;
        std::vector<float> ref(2 * 4800);
        CHECK(one.pull(spans, 0, 4800, ref.data(), error));
        return rms(ref);
    }();
    spans[0].gain = 0.5;
    CHECK(half.pull(spans, 0, 4800, in.data(), error));
    CHECK(std::fabs(rms(in) - full * 0.5) < 0.06);

    // Hard-left pan silences the right channel.
    spans[0].gain = 1.0;
    spans[0].pan = -1.0;
    fc::AudioWindowMixer panned(48000, 2);
    CHECK(panned.pull(spans, 0, 4800, in.data(), error));
    double leftRms = 0.0, rightRms = 0.0;
    for (size_t i = 0; i < in.size() / 2; ++i) {
        leftRms += double(in[2 * i]) * double(in[2 * i]);
        rightRms += double(in[2 * i + 1]) * double(in[2 * i + 1]);
    }
    leftRms = std::sqrt(leftRms / (in.size() / 2));
    rightRms = std::sqrt(rightRms / (in.size() / 2));
    CHECK(leftRms > 0.1);
    CHECK(rightRms < 0.02);

    // Fade-in ramps from silence.
    spans[0].pan = 0.0;
    spans[0].fadeInSec = 1.0;
    fc::AudioWindowMixer fading(48000, 2);
    CHECK(fading.pull(spans, 0, 2400, in.data(), error)); // first 50 ms
    const double early = rms(std::vector<float>(in.begin(), in.begin() + 2 * 2400));
    CHECK(fading.pull(spans, 45600, 2400, in.data(), error)); // [0.95, 1.0) s
    const double late = rms(std::vector<float>(in.begin(), in.begin() + 2 * 2400));
    CHECK(early < late * 0.2);
    spans[0].fadeInSec = 0.0;

    // Master gain + the hard clip.
    spans[0].gain = 8.0; // 0.4 * 8 = 3.2 -> clipped
    fc::AudioWindowMixer loud(48000, 2);
    loud.setMasterGain(1.0);
    CHECK(loud.pull(spans, 0, 4800, in.data(), error));
    for (float v : in) {
        CHECK(v <= 1.0f && v >= -1.0f);
    }
    double clippedRms = 0.0;
    for (float v : in) {
        clippedRms += double(v) * double(v);
    }
    clippedRms = std::sqrt(clippedRms / in.size());
    CHECK(clippedRms > 0.5); // heavy limiting, not silence

    // Missing file: silence + an error note, never a failed window.
    fc::AudioSpan missing;
    missing.path = pathOf("does_not_exist.mp4");
    missing.startSec = 0.0;
    missing.endSec = 1.0;
    std::vector<fc::AudioSpan> bad = {missing};
    fc::AudioWindowMixer tolerant(48000, 2);
    std::string mixedError;
    CHECK(tolerant.pull(bad, 0, 4800, in.data(), mixedError));
    for (size_t i = 0; i < in.size(); ++i) {
        CHECK(in[i] == 0.0f);
    }
    CHECK(!mixedError.empty());

    // reset() drops the decoders (a fresh pull works after).
    tolerant.reset();
    CHECK(tolerant.pull(bad, 0, 4800, in.data(), mixedError)); // still silent
}

void testExportWithAudio() {
    const std::string dst = pathOf("export_audio.mp4");
    std::string error;

    fc::ExportConfig config;
    config.width = 64;
    config.height = 36;
    config.fps = 12.0;
    config.totalFrames = 24; // 2 seconds
    config.crf = 26;

    // A 440 Hz provider in the audio windows (the same formula the
    // synthetic generator uses, so the round-trip compares like with
    // like).
    const int rate = config.sampleRate; // 48000
    double phase = 0.0;
    const double step = 2.0 * 3.141592653589793 * 440.0 / rate;
    fc::ExportAudioProvider audio = [&](int64_t startSample, int sampleCount, float *dstOut) {
        CHECK(startSample >= 0);
        CHECK(sampleCount > 0);
        for (int i = 0; i < sampleCount; ++i) {
            const float v = 0.4f * float(std::sin(phase));
            phase += step;
            dstOut[2 * i] = v;
            dstOut[2 * i + 1] = v;
        }
        return true;
    };

    double lastProgress = -1.0;
    const bool ok = fc::Exporter::run(
        dst, config,
        [](int64_t frame, uint8_t *rgba) {
            fillExportFrame(frame, rgba, 64, 36);
            return true;
        },
        [&](double fraction) {
            CHECK(fraction >= lastProgress - 1e-9);
            lastProgress = fraction;
            return true;
        },
        error, audio);
    CHECK(ok);
    if (!ok) {
        std::printf("audio export failed: %s\n", error.c_str());
        return;
    }

    // The output carries an AAC stereo track at the config rate.
    fc::MediaInfo info;
    CHECK(fc::MediaProbe::probe(dst, info, error));
    CHECK(info.hasVideo);
    CHECK(!info.audioStreams.empty());
    if (!info.audioStreams.empty()) {
        CHECK(info.audioStreams[0].codecName == "aac");
        CHECK(info.audioStreams[0].channels == 2);
        CHECK(info.audioStreams[0].sampleRate == 48000);
    }
    // The audio duration tracks the video (within one AAC frame + the
    // muxer's start padding).
    CHECK(std::fabs(info.durationSeconds() - 2.0) < 0.4);

    // Decode the audio back: sample count ~ 2 s, the tone dominates.
    fc::AudioDecoder decoder;
    CHECK(decoder.open(dst, 48000, 2, error));
    std::vector<float> all;
    fc::DecodedAudio chunk;
    while (decoder.readSamples(chunk, error)) {
        all.insert(all.end(), chunk.samples.begin(), chunk.samples.end());
    }
    CHECK(all.size() >= 2 * 48000 * 1); // at least 1 s survived the round-trip
    CHECK(all.size() <= 2 * 48000 * 3);
    CHECK(rms(all) > 0.1);
    CHECK(goertzel(all, 440.0, 48000) > 0.4);

    // Cancellation through the AUDIO provider still removes the file.
    const std::string cancelDst = pathOf("export_audio_cancel.mp4");
    bool pulled = false;
    const bool cancelled = fc::Exporter::run(
        cancelDst, config,
        [](int64_t, uint8_t *rgba) {
            fillExportFrame(0, rgba, 64, 36);
            return true;
        },
        [](double) { return true; }, error,
        [&](int64_t, int, float *) {
            pulled = true;
            return false; // cancel on the first audio window
        });
    CHECK(!cancelled);
    CHECK(error.empty());
    CHECK(pulled);
    CHECK(!fs::exists(cancelDst));
}

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
    testAudioDecoder();
    testAudioWindowMixer();
    testExportWithAudio();

    return testExitCode("media");
}

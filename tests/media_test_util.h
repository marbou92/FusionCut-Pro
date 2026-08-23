#pragma once

// Test-only synthetic media generator. Produces small MP4 files (H.264 or
// MPEG-4 fallback + optional AAC tone) so integration tests never need
// binary assets in the repository.

#include <cstdint>
#include <string>

namespace fc {

struct TestMediaSpec {
    int width = 640;
    int height = 360;
    int fps = 25;
    double seconds = 3.0;
    uint8_t colorA[3] = {200, 40, 40}; // RGB of even seconds
    uint8_t colorB[3] = {40, 40, 200}; // RGB of odd seconds
    bool withAudio = true;
    int audioHz = 440;
};

// Writes the test clip to `path` (use .mp4). Returns false + error on any
// FFmpeg failure.
bool generateTestVideo(const std::string &path, const TestMediaSpec &spec, std::string &error);

} // namespace fc

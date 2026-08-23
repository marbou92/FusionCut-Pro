#pragma once

#include <string>

#include "media_info.h"

namespace fc {

// Inspects a media file and fills a MediaInfo structure (a ffprobe-lite).
// Pure function: opens the file, reads headers, closes it.
class MediaProbe {
public:
    // Returns false and fills `error` when the file cannot be opened or
    // parsed. A file with no decodable streams still succeeds with
    // hasVideo=false and an empty audioStreams list.
    static bool probe(const std::string &path, MediaInfo &out, std::string &error);
};

} // namespace fc

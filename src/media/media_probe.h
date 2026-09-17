#pragma once

#include <string>

#include "media_info.h"

struct AVFormatContext;

namespace fc {

// Inspects a media file and fills a MediaInfo structure (a ffprobe-lite).
// Pure function: opens the file, reads headers, closes it.
class MediaProbe {
public:
    // Returns false and fills `error` when the file cannot be opened or
    // parsed. A file with no decodable streams still succeeds with
    // hasVideo=false and an empty audioStreams list.
    static bool probe(const std::string &path, MediaInfo &out, std::string &error);

    // Variant for an ALREADY-OPENED, stream-info'd context (owned by the
    // caller): fills `out` from it without opening or closing anything.
    // Callers that keep using the context afterwards (the video decoder)
    // avoid paying the expensive find_stream_info pass twice.
    static bool probe(AVFormatContext *ctx, const std::string &path, MediaInfo &out,
                      std::string &error);
};

} // namespace fc

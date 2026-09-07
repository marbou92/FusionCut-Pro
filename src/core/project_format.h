#pragma once

#include <cstdint>
#include <string>

#include "timeline_model.h"

namespace fc {

// ---------------------------------------------------------------------------
// project persistence (save/load, format 1).
//
// Pure text codec, no Qt, no FFmpeg: a hand-rolled strict JSON subset
// (objects, arrays, strings with escapes incl. \uXXXX surrogate pairs,
// numbers, true/false/null) is both the writer and the reader, so one
// small file owns the whole format and it is fully unit tested in
// fc_project_tests.
//
// serializeProject() is DETERMINISTIC: the same model always serializes
// to the same bytes (fixed field order, shortest-exact number format) -
// same tree, same file, across platforms.
//
// The schema (format 1; adds the bracketed fields - old files load
// unchanged, "text" is simply absent):
// {
//   "format": 1,
//   "fps": 24,
//   "tracks":  [ {"name","audio",[text],"locked","muted","solo"}, ... ],
//   "clips":   [ {"id","track","source","label","in","out","start","rate",
//                 "effects": [ {"id","enabled","params":{k:v},
//                               "keyframes":{k:[[frame,value],...]}} ]},
//                {"id","track","label","start","duration","text":TEXT,
//                 "effects":[...]} , ... ],
//   "transitions": [ {"id","track","left","right","kind","duration"}, ... ]
// }
//
// TEXT = {"align":"left|center|right","anchorX","anchorY","wrap",
//         "background":bool,"bgColor":"RRGGBBAA",
//         "runs":[{"text","family","size","bold","italic","underline",
//                   "color":"RRGGBBAA"}]}
//
// Text tracks carry "text":true (mutually exclusive with "audio"); text
// clips reference one, carry a TEXT document instead of a source, and
// serialize their duration directly (sourceIn is always 0, rate 1).
//
// Forward compatibility mirrors the runtime contract: effect / transition
// ids unknown to THIS catalog round-trip untouched (they never process,
// they never crash); a clip referencing a missing track is rejected.
// Known effects store params BY KEY (catalog order changes never corrupt
// a file); unknown effects store positional "values".
// ---------------------------------------------------------------------------

// Serializes the model. Media files are referenced by path only - the
// project file never embeds media (1 GB RAM budget).
std::string serializeProject(const TimelineModel &model);

// Parses `text` and REPLACES the entire contents of `model` on success
// (ids, stacks, keyframes, transitions preserved exactly). On failure the
// model is left untouched and `error` says why. Structural JSON errors
// and schema violations (missing/wrongly-typed fields, dangling track or
// clip references, invalid clip geometry) are hard failures; the parse
// only succeeds completely or not at all.
bool parseProject(const std::string &text, TimelineModel &model, std::string &error);

// The format version this build writes (and the minimum it reads).
constexpr int kProjectFormatVersion = 1;

} // namespace fc

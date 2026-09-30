// SPDX-License-Identifier: MIT
//
// A Matroska file read back through libwebm's mkvparser: the first video and audio tracks,
// their CodecPrivate and every block's frames, for `av1mkv check` and `dump-frame`.
#pragma once

#include <mkvparser/mkvparser.h>
#include <mkvparser/mkvreader.h>

#include <cstdint>
#include <string>
#include <vector>

struct MkvFrame {
    long long pos = 0;
    long len = 0;
    bool key = false;
};

struct MkvTracks {
    mkvparser::MkvReader reader;
    mkvparser::Segment* segment = nullptr;
    std::string video_codec;
    std::vector<uint8_t> video_private;
    std::vector<MkvFrame> video;
    std::string audio_codec;
    std::vector<uint8_t> audio_private;
    std::vector<MkvFrame> audio;
    long long duration_ns = 0;

    std::string open(const char* path);
    std::string read(const MkvFrame& f, std::vector<uint8_t>& out);
    ~MkvTracks();
};

// "" when the segment's declared size fits in the file; mkvparser itself reads a segment that
// runs past the end of the file as one of unknown size.
std::string mkv_truncation(mkvparser::IMkvReader* reader, const mkvparser::Segment* segment);

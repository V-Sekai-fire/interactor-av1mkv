// The audio track as FLAC, through libFLAC (thirdparty/flac, the org's fork of xiph/flac): 16-bit
// PCM in, the A_FLAC CodecPrivate ("fLaC" and the metadata blocks) and one block per FLAC frame out.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct FlacTrack {
    std::vector<uint8_t> codec_private;
    std::vector<std::vector<uint8_t>> frames;
    std::vector<uint64_t> first_sample; // per frame
    uint64_t samples = 0;
};

// Encodes interleaved 16-bit PCM at libFLAC's level 8 with 4096-sample blocks. Returns "" or why not.
std::string flac_encode(const int16_t* pcm, uint64_t samples, uint32_t channels, uint32_t rate, FlacTrack& out);

// Decodes the track back with libFLAC's decoder; "" when every sample equals the PCM.
std::string flac_verify(const FlacTrack& t, const int16_t* pcm, uint32_t channels);

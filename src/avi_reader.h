// SPDX-License-Identifier: MIT
//
// Reader for the AVI/RIFF file entities-godot-cineform's MovieWriterCineForm writes
// (movie_writer_cineform.cpp, begin_avi/write_sample/write_audio): one 'vids' stream with
// handler CFHD, one 'auds' stream of 16-bit PCM (WAVEFORMATEX, 18 bytes), interleaved
// 00dc/01wb chunks under LIST movi, word-aligned, and an idx1 that we do not need because
// the movi list is walked directly. Everything is mmap-free: the file is read once into
// memory (it is a few hundred MB at most for a recording of this kind).
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct AviChunk {
    uint64_t offset = 0; // of the payload, from the start of the file
    uint32_t size = 0;
};

struct AviMovie {
    std::vector<uint8_t> bytes;
    uint32_t width = 0, height = 0;
    uint32_t fps_num = 0, fps_den = 1;   // vids strh dwRate/dwScale
    uint32_t total_frames = 0;            // avih dwTotalFrames
    char handler[5] = {0, 0, 0, 0, 0};
    // audio
    bool has_audio = false;
    uint32_t mix_rate = 0, channels = 0, bits = 0;
    std::vector<AviChunk> video; // 00dc
    std::vector<AviChunk> audio; // 01wb

    const uint8_t* data(const AviChunk& c) const { return bytes.data() + c.offset; }
};

namespace avi_detail {
inline uint32_t u32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline uint16_t u16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
inline bool tag(const uint8_t* p, const char* t) { return std::memcmp(p, t, 4) == 0; }
} // namespace avi_detail

// Returns an empty string on success, else the reason.
inline std::string avi_read(const char* path, AviMovie& m)
{
    using namespace avi_detail;
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return std::string("cannot open ") + path;
    std::fseek(f, 0, SEEK_END);
    const long long len = _ftelli64(f);
    std::fseek(f, 0, SEEK_SET);
    m.bytes.resize(size_t(len));
    if (std::fread(m.bytes.data(), 1, size_t(len), f) != size_t(len)) { std::fclose(f); return "short read"; }
    std::fclose(f);

    const uint8_t* b = m.bytes.data();
    const uint64_t n = m.bytes.size();
    if (n < 12 || !tag(b, "RIFF") || !tag(b + 8, "AVI ")) return "not a RIFF AVI";

    // Walk the top-level chunks; descend into LIST hdrl (strl lists) and LIST movi.
    uint64_t pos = 12;
    const uint64_t riff_end = std::min<uint64_t>(n, 8 + u32(b + 4));
    int stream_index = 0;
    while (pos + 8 <= riff_end) {
        const uint8_t* c = b + pos;
        const uint32_t size = u32(c + 4);
        const uint64_t payload = pos + 8;
        if (tag(c, "LIST")) {
            if (payload + 4 > n) break;
            const uint8_t* lt = b + payload;
            if (tag(lt, "hdrl")) {
                // avih, then strl lists
                uint64_t p = payload + 4;
                const uint64_t end = payload + size;
                while (p + 8 <= end) {
                    const uint8_t* h = b + p;
                    const uint32_t hs = u32(h + 4);
                    if (tag(h, "avih") && hs >= 56) {
                        m.total_frames = u32(h + 8 + 16);
                        m.width = u32(h + 8 + 32);
                        m.height = u32(h + 8 + 36);
                    } else if (tag(h, "LIST") && tag(h + 8, "strl")) {
                        uint64_t q = p + 12;
                        const uint64_t qe = p + 8 + hs;
                        char type[5] = {0};
                        while (q + 8 <= qe) {
                            const uint8_t* s = b + q;
                            const uint32_t ss = u32(s + 4);
                            if (tag(s, "strh") && ss >= 56) {
                                std::memcpy(type, s + 8, 4);
                                if (tag((const uint8_t*)type, "vids")) {
                                    std::memcpy(m.handler, s + 12, 4);
                                    const uint32_t scale = u32(s + 8 + 20);
                                    const uint32_t rate = u32(s + 8 + 24);
                                    m.fps_num = rate;
                                    m.fps_den = scale ? scale : 1;
                                }
                            } else if (tag(s, "strf")) {
                                if (tag((const uint8_t*)type, "auds") && ss >= 16) {
                                    m.has_audio = true;
                                    m.channels = u16(s + 8 + 2);
                                    m.mix_rate = u32(s + 8 + 4);
                                    m.bits = u16(s + 8 + 14);
                                } else if (tag((const uint8_t*)type, "vids") && ss >= 40) {
                                    if (!m.width) m.width = u32(s + 8 + 4);
                                    if (!m.height) m.height = u32(s + 8 + 8);
                                }
                            }
                            q += 8 + ss + (ss & 1);
                        }
                        (void)stream_index;
                        stream_index++;
                    }
                    p += 8 + hs + (hs & 1);
                }
            } else if (tag(lt, "movi")) {
                uint64_t p = payload + 4;
                // A file that was cut short (the writer patches sizes at the end) still has
                // its chunks: walk to the end of the file if the LIST size overshoots.
                const uint64_t end = std::min<uint64_t>(n, payload + size);
                while (p + 8 <= end) {
                    const uint8_t* h = b + p;
                    const uint32_t hs = u32(h + 4);
                    if (p + 8 + hs > n) break;
                    AviChunk ck;
                    ck.offset = p + 8;
                    ck.size = hs;
                    if (tag(h, "00dc")) m.video.push_back(ck);
                    else if (tag(h, "01wb")) m.audio.push_back(ck);
                    p += 8 + hs + (hs & 1);
                }
            }
        }
        pos = payload + size + (size & 1);
    }
    if (m.video.empty()) return "no 00dc video chunks";
    if (!m.width || !m.height) return "no dimensions in the headers";
    if (!m.fps_num) { m.fps_num = 30; m.fps_den = 1; }
    return std::string();
}

// SPDX-License-Identifier: MIT
//
// AV1 OBU walking and the av1C CodecPrivate box (AV1 Codec ISO Media File Format Binding,
// section 2.3), built from the sequence header OBU NVENC emits with its first key frame.
// The sequence header is parsed for real (seq_profile, seq_level_idx[0], seq_tier[0],
// color_config) rather than assumed, so the box says what the bitstream says.
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace av1 {

enum ObuType { OBU_SEQUENCE_HEADER = 1, OBU_TEMPORAL_DELIMITER = 2, OBU_FRAME_HEADER = 3, OBU_TILE_GROUP = 4, OBU_METADATA = 5, OBU_FRAME = 6, OBU_REDUNDANT_FRAME_HEADER = 7, OBU_TILE_LIST = 8, OBU_PADDING = 15 };

struct Obu {
    int type = 0;
    const uint8_t* start = nullptr; // whole OBU incl. header and size field
    size_t length = 0;              // whole OBU
    const uint8_t* payload = nullptr;
    size_t payload_length = 0;
};

// Low-overhead bitstream format (every OBU carries obu_size), which is what NVENC emits
// with outputAnnexBFormat = 0. Returns false on a malformed stream.
inline bool walk(const uint8_t* data, size_t size, std::vector<Obu>& out)
{
    size_t p = 0;
    while (p < size) {
        Obu o;
        o.start = data + p;
        const uint8_t h = data[p];
        if (h & 0x80) return false; // forbidden bit
        o.type = (h >> 3) & 0xf;
        const bool ext = (h >> 2) & 1;
        const bool has_size = (h >> 1) & 1;
        size_t q = p + 1 + (ext ? 1 : 0);
        if (!has_size) return false;
        uint64_t sz = 0;
        for (int i = 0; i < 8; i++) {
            if (q >= size) return false;
            const uint8_t b = data[q++];
            sz |= uint64_t(b & 0x7f) << (7 * i);
            if (!(b & 0x80)) break;
        }
        if (q + sz > size) return false;
        o.payload = data + q;
        o.payload_length = size_t(sz);
        o.length = q + size_t(sz) - p;
        out.push_back(o);
        p += o.length;
    }
    return true;
}

struct BitReader {
    const uint8_t* d; size_t n; size_t pos = 0; bool bad = false;
    BitReader(const uint8_t* data, size_t size) : d(data), n(size) {}
    uint32_t f(int bits) {
        uint32_t v = 0;
        for (int i = 0; i < bits; i++) {
            if (pos >= n * 8) { bad = true; return 0; }
            v = (v << 1) | ((d[pos >> 3] >> (7 - (pos & 7))) & 1);
            pos++;
        }
        return v;
    }
    uint32_t uvlc() {
        int lz = 0;
        while (!bad && f(1) == 0) { if (++lz >= 32) return 0xffffffffu; }
        if (lz >= 32) return 0xffffffffu;
        return f(lz) + (1u << lz) - 1;
    }
};

struct SeqInfo {
    uint32_t profile = 0, level = 0, tier = 0;
    uint32_t high_bitdepth = 0, twelve_bit = 0, mono = 0, ss_x = 1, ss_y = 1, csp = 0;
    uint32_t max_width = 0, max_height = 0;
    uint32_t color_primaries = 2, transfer = 2, matrix = 2, color_range = 0;
};

// Parses the sequence_header_obu() syntax (AV1 spec 5.5) far enough for av1C.
inline bool parse_sequence_header(const uint8_t* p, size_t n, SeqInfo& s)
{
    BitReader r(p, n);
    s.profile = r.f(3);
    r.f(1); // still_picture
    const uint32_t reduced = r.f(1);
    uint32_t decoder_model_info_present = 0, buffer_delay_length_minus_1 = 0;
    if (reduced) {
        s.level = r.f(5);
        s.tier = 0;
    } else {
        const uint32_t timing_info_present = r.f(1);
        if (timing_info_present) {
            r.f(32); r.f(32);
            const uint32_t equal_picture_interval = r.f(1);
            if (equal_picture_interval) r.uvlc();
            decoder_model_info_present = r.f(1);
            if (decoder_model_info_present) {
                buffer_delay_length_minus_1 = r.f(5);
                r.f(32); r.f(5); r.f(5);
            }
        }
        const uint32_t initial_display_delay_present = r.f(1);
        const uint32_t op_cnt_minus_1 = r.f(5);
        for (uint32_t i = 0; i <= op_cnt_minus_1; i++) {
            r.f(12); // operating_point_idc
            const uint32_t level = r.f(5);
            uint32_t tier = 0;
            if (level > 7) tier = r.f(1);
            if (i == 0) { s.level = level; s.tier = tier; }
            if (decoder_model_info_present) {
                if (r.f(1)) { // decoder_model_present_for_this_op
                    const int nbits = int(buffer_delay_length_minus_1) + 1;
                    r.f(nbits); r.f(nbits); r.f(1);
                }
            }
            if (initial_display_delay_present) {
                if (r.f(1)) r.f(4);
            }
        }
    }
    const uint32_t wbits = r.f(4) + 1;
    const uint32_t hbits = r.f(4) + 1;
    s.max_width = r.f(int(wbits)) + 1;
    s.max_height = r.f(int(hbits)) + 1;
    if (!reduced) {
        if (r.f(1)) { r.f(4); r.f(3); } // frame_id_numbers_present_flag
    }
    r.f(1); r.f(1); r.f(1); // use_128x128_superblock, enable_filter_intra, enable_intra_edge_filter
    if (!reduced) {
        r.f(1); r.f(1); r.f(1); r.f(1); // interintra, masked, warped, dual_filter
        const uint32_t enable_order_hint = r.f(1);
        if (enable_order_hint) { r.f(1); r.f(1); }
        uint32_t seq_force_screen_content_tools = 2;
        if (!r.f(1)) seq_force_screen_content_tools = r.f(1);
        if (seq_force_screen_content_tools > 0) {
            if (!r.f(1)) r.f(1); // seq_choose_integer_mv / seq_force_integer_mv
        }
        if (enable_order_hint) r.f(3);
    }
    r.f(1); r.f(1); r.f(1); // superres, cdef, restoration
    // color_config()
    s.high_bitdepth = r.f(1);
    if (s.profile == 2 && s.high_bitdepth) s.twelve_bit = r.f(1);
    s.mono = (s.profile == 1) ? 0 : r.f(1);
    if (r.f(1)) { s.color_primaries = r.f(8); s.transfer = r.f(8); s.matrix = r.f(8); }
    if (s.mono) {
        s.color_range = r.f(1); s.ss_x = 1; s.ss_y = 1; s.csp = 0;
    } else if (s.color_primaries == 1 && s.transfer == 13 && s.matrix == 0) {
        s.color_range = 1; s.ss_x = 0; s.ss_y = 0;
    } else {
        s.color_range = r.f(1);
        if (s.profile == 0) { s.ss_x = 1; s.ss_y = 1; }
        else if (s.profile == 1) { s.ss_x = 0; s.ss_y = 0; }
        else {
            if (s.twelve_bit) { s.ss_x = r.f(1); s.ss_y = s.ss_x ? r.f(1) : 0; }
            else { s.ss_x = 1; s.ss_y = 0; }
        }
        if (s.ss_x && s.ss_y) s.csp = r.f(2);
    }
    return !r.bad;
}

// av1C: 4 fixed bytes then configOBUs (the sequence header OBU, size field and all).
inline std::vector<uint8_t> build_av1c(const SeqInfo& s, const Obu& seq_hdr)
{
    std::vector<uint8_t> v;
    v.push_back(0x81); // marker = 1, version = 1
    v.push_back(uint8_t((s.profile << 5) | (s.level & 0x1f)));
    v.push_back(uint8_t((s.tier << 7) | (s.high_bitdepth << 6) | (s.twelve_bit << 5) | (s.mono << 4) |
                        (s.ss_x << 3) | (s.ss_y << 2) | (s.csp & 3)));
    v.push_back(0x00); // initial_presentation_delay_present = 0
    v.insert(v.end(), seq_hdr.start, seq_hdr.start + seq_hdr.length);
    return v;
}

} // namespace av1

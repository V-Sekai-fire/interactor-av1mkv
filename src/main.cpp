// SPDX-License-Identifier: MIT
//
// av1mkv: a CineForm (CFHD in AVI, as entities-godot-cineform's MovieWriter writes it) to
// AV1-and-FLAC .webm transcoder, a CineForm-and-FLAC .mkv remuxer, and an mkvparser-based
// inspector for both.
//
//   av1mkv encode <in.cfhd> <out.webm> [--cq 22] [--gop 60] [--gpu "RTX 4090"] [--frames N] [--xmp packet.xml]
//   av1mkv mkv <in.cfhd> <out.mkv>
//   av1mkv check <in.cfhd> <in.mkv>
//   av1mkv info <file.webm|file.mkv>
//   av1mkv dump-frame <in.cfhd|in.mkv> <index> <out.ppm>
//
// Decode: V-Sekai-fire/cineform-sdk, CFHD_OpenDecoder / CFHD_PrepareToDecode /
// CFHD_DecodeSample to 8-bit BGRA (the 12-bit 4:4:4 source is rounded to 8 bits here; the
// encoder is 8-bit 4:2:0). Encode: NVENC AV1 through the NVIDIA driver (nvenc_av1.cpp).
// Mux: V-Sekai-fire/libwebm mkvmuxer, V_AV1 with an av1C CodecPrivate built from the
// sequence header OBU of the first key frame (av1c.h), plus the recording's PCM track as
// A_FLAC (libFLAC, flac_track.cpp). The .mkv carries the CFHD frames unchanged as
// V_MS/VFW/FOURCC, the recording's BITMAPINFOHEADER as its CodecPrivate. No FFmpeg anywhere.

#include "avi_reader.h"
#include "av1c.h"
#include "flac_track.h"
#include "mkv_tracks.h"
#include "nvenc_av1.h"

#include <CFHDDecoder.h>
#include <CFHDTypes.h>

#include <mkvmuxer/mkvmuxer.h>
#include <mkvmuxer/mkvwriter.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

int mkv_info(const char* path); // mkv_info.cpp

namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

int fail(const std::string& why)
{
    std::fprintf(stderr, "av1mkv: %s\n", why.c_str());
    return 1;
}

struct CfhdDecoder {
    CFHD_DecoderRef ref = nullptr;
    int width = 0, height = 0;
    CFHD_PixelFormat format = CFHD_PIXEL_FORMAT_BGRA;
    int32_t pitch = 0;
    std::vector<uint8_t> frame;

    std::string open(const AviMovie& m) { return open(int(m.width), int(m.height), m.data(m.video[0]), m.video[0].size); }
    std::string open(int w, int h, const uint8_t* first, uint32_t first_size)
    {
        CFHD_Error err = CFHD_OpenDecoder(&ref, nullptr);
        if (err != CFHD_ERROR_OKAY) return "CFHD_OpenDecoder failed, code " + std::to_string(int(err));
        int aw = 0, ah = 0;
        CFHD_PixelFormat af = format;
        err = CFHD_PrepareToDecode(ref, w, h, format, CFHD_DECODED_RESOLUTION_FULL, CFHD_DECODING_FLAGS_NONE,
                                   const_cast<uint8_t*>(first), first_size, &aw, &ah, &af);
        if (err != CFHD_ERROR_OKAY) return "CFHD_PrepareToDecode failed, code " + std::to_string(int(err));
        width = aw;
        height = ah;
        format = af;
        CFHD_GetImagePitch(uint32_t(width), format, &pitch);
        frame.resize(size_t(pitch) * size_t(height));
        return std::string();
    }
    std::string decode(const AviMovie& m, size_t i) { return decode(m.data(m.video[i]), m.video[i].size, i); }
    std::string decode(const uint8_t* sample, uint32_t size, size_t i)
    {
        const CFHD_Error err = CFHD_DecodeSample(ref, const_cast<uint8_t*>(sample), size, frame.data(), pitch);
        if (err != CFHD_ERROR_OKAY) return "CFHD_DecodeSample failed on frame " + std::to_string(i) + ", code " + std::to_string(int(err));
        return std::string();
    }
    ~CfhdDecoder() { if (ref) CFHD_CloseDecoder(ref); }
};

// The decoder's BGRA is the Windows DIB convention, bottom row first (the MovieWriter
// flipped Godot's top-down rows into it, see movie_writer_cineform.cpp). NVENC wants
// top-down, so the rows are handed over through a reversed pitch: pointer to the last row
// and a walk upward. nvenc_av1 copies row by row, so a negative stride is expressed as a
// separate top-down buffer here; the copy is one memcpy per row either way.
void flip_rows(const std::vector<uint8_t>& src, int32_t pitch, int height, std::vector<uint8_t>& dst)
{
    dst.resize(src.size());
    for (int y = 0; y < height; y++)
        std::memcpy(dst.data() + size_t(pitch) * y, src.data() + size_t(pitch) * (height - 1 - y), size_t(pitch));
}

// An XMP packet file for the segment's XMP SimpleTag: the whole <?xpacket begin ... end?> text.
std::string read_xmp(const char* path, std::string& xmp)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::string("cannot read ") + path;
    std::stringstream ss;
    ss << f.rdbuf();
    xmp = ss.str();
    if (xmp.find("<?xpacket begin") == std::string::npos || xmp.find("<?xpacket end") == std::string::npos)
        return std::string(path) + " is not an XMP packet (no <?xpacket begin ... end?>)";
    if (xmp.find('\0') != std::string::npos) return std::string(path) + " has a NUL byte";
    return std::string();
}

bool ends_with(const std::string& s, const char* suffix)
{
    const size_t n = std::strlen(suffix);
    return s.size() > n && s.compare(s.size() - n, n, suffix) == 0;
}

const char kCineFormCodecId[] = "V_MS/VFW/FOURCC";

std::string cineform_format(const std::vector<uint8_t>& bih)
{
    if (bih.size() < 40 || avi_detail::u32(bih.data()) < 40) return "the video format is not a BITMAPINFOHEADER";
    if (std::memcmp(bih.data() + 16, "CFHD", 4)) return "the BITMAPINFOHEADER's FOURCC is not CFHD";
    return std::string();
}

int write_ppm(const CfhdDecoder& dec, const char* out, size_t index, size_t frames);

int dump_mkv_frame(const char* in, size_t index, const char* out)
{
    MkvTracks t;
    std::string e = t.open(in);
    if (!e.empty()) return fail(e);
    if (t.video_codec != kCineFormCodecId) return fail("the video track is " + t.video_codec + ", not " + kCineFormCodecId);
    if (!(e = cineform_format(t.video_private)).empty()) return fail(e);
    if (index >= t.video.size()) return fail("frame index out of range");
    const int w = int(avi_detail::u32(t.video_private.data() + 4));
    const int h = std::abs(int(avi_detail::u32(t.video_private.data() + 8)));
    std::vector<uint8_t> first, sample;
    if (!(e = t.read(t.video[0], first)).empty() || !(e = t.read(t.video[index], sample)).empty()) return fail(e);
    CfhdDecoder dec;
    if (!(e = dec.open(w, h, first.data(), uint32_t(first.size()))).empty()) return fail(e);
    if (!(e = dec.decode(sample.data(), uint32_t(sample.size()), index)).empty()) return fail(e);
    return write_ppm(dec, out, index, t.video.size());
}

int dump_frame(const char* in, size_t index, const char* out)
{
    if (ends_with(in, ".mkv")) return dump_mkv_frame(in, index, out);
    AviMovie m;
    std::string e = avi_read(in, m);
    if (!e.empty()) return fail(e);
    if (index >= m.video.size()) return fail("frame index out of range");
    CfhdDecoder dec;
    if (!(e = dec.open(m)).empty()) return fail(e);
    if (!(e = dec.decode(m, index)).empty()) return fail(e);
    return write_ppm(dec, out, index, m.video.size());
}

int write_ppm(const CfhdDecoder& dec, const char* out, size_t index, size_t frames)
{
    std::vector<uint8_t> top;
    flip_rows(dec.frame, dec.pitch, dec.height, top);
    std::FILE* f = std::fopen(out, "wb");
    if (!f) return fail("cannot write ppm");
    std::fprintf(f, "P6\n%d %d\n255\n", dec.width, dec.height);
    for (int y = 0; y < dec.height; y++) {
        const uint8_t* row = top.data() + size_t(dec.pitch) * y;
        for (int x = 0; x < dec.width; x++) {
            const uint8_t rgb[3] = {row[x * 4 + 2], row[x * 4 + 1], row[x * 4 + 0]};
            std::fwrite(rgb, 1, 3, f);
        }
    }
    std::fclose(f);
    std::printf("wrote %s (%dx%d, frame %zu of %zu)\n", out, dec.width, dec.height, index, frames);
    return 0;
}

std::string open_segment(mkvmuxer::MkvWriter& writer, mkvmuxer::Segment& seg, const char* out, const AviMovie& m,
                         const char* codec_id, uint64_t& vtrack)
{
    if (!writer.Open(out)) return std::string("cannot open ") + out + " for writing";
    if (!seg.Init(&writer)) return "mkvmuxer Segment::Init failed";
    seg.set_mode(mkvmuxer::Segment::kFile);
    seg.OutputCues(true);
    seg.GetSegmentInfo()->set_writing_app("av1mkv (interactor-dress-on)");
    seg.GetSegmentInfo()->set_muxing_app("libwebm mkvmuxer (V-Sekai-fire/libwebm)");
    vtrack = seg.AddVideoTrack(int(m.width), int(m.height), 0);
    if (!vtrack) return "AddVideoTrack failed";
    mkvmuxer::VideoTrack* video = static_cast<mkvmuxer::VideoTrack*>(seg.GetTrackByNumber(vtrack));
    video->set_codec_id(codec_id);
    video->set_frame_rate(double(m.fps_num) / double(m.fps_den));
    video->set_default_duration(uint64_t(1000000000ull * m.fps_den / m.fps_num));
    seg.CuesTrack(vtrack);
    return std::string();
}

std::vector<int16_t> pcm_of(const AviMovie& m)
{
    std::vector<int16_t> pcm;
    for (const AviChunk& c : m.audio) {
        const int16_t* p = reinterpret_cast<const int16_t*>(m.data(c));
        pcm.insert(pcm.end(), p, p + c.size / 2);
    }
    return pcm;
}

// The PCM as FLAC, decoded back bit-exact before it is muxed. A .webm's FLAC makes libwebm write
// the matroska doctype: WebM's spec names only Opus and Vorbis.
std::string add_flac_track(mkvmuxer::Segment& seg, const AviMovie& m, FlacTrack& flac, uint64_t& atrack)
{
    atrack = 0;
    if (!m.has_audio || m.audio.empty()) return std::string();
    if (m.bits != 16) return "the recording's audio is not 16-bit PCM";
    const std::vector<int16_t> pcm = pcm_of(m);
    const uint64_t total = pcm.size() / m.channels;
    std::string e = flac_encode(pcm.data(), total, m.channels, m.mix_rate, flac);
    if (e.empty()) e = flac_verify(flac, pcm.data(), m.channels);
    if (!e.empty()) return e;
    size_t bytes = 0;
    for (const std::vector<uint8_t>& f : flac.frames) bytes += f.size();
    std::printf("flac: %llu samples x%u, %zu frames, %zu bytes (PCM %zu); libFLAC decodes it back bit-exact, MD5 checked\n",
                (unsigned long long)total, m.channels, flac.frames.size(), bytes, pcm.size() * 2);
    atrack = seg.AddAudioTrack(int(m.mix_rate), int(m.channels), 0);
    if (!atrack) return "AddAudioTrack failed";
    mkvmuxer::AudioTrack* audio = static_cast<mkvmuxer::AudioTrack*>(seg.GetTrackByNumber(atrack));
    audio->set_codec_id("A_FLAC");
    audio->set_bit_depth(16);
    if (!audio->SetCodecPrivate(flac.codec_private.data(), flac.codec_private.size())) return "SetCodecPrivate (FLAC) failed";
    return std::string();
}

// FLAC frames up to a video frame's time go first: mkvmuxer wants timestamps monotonic across tracks.
struct AudioFeed {
    mkvmuxer::Segment* seg = nullptr;
    const FlacTrack* flac = nullptr;
    uint64_t track = 0;
    uint32_t rate = 0;
    size_t next = 0;

    std::string flush_to(uint64_t ts_ns)
    {
        while (track && next < flac->frames.size()) {
            const uint64_t ts = flac->first_sample[next] * 1000000000ull / rate;
            if (ts > ts_ns) break;
            const std::vector<uint8_t>& f = flac->frames[next];
            if (!seg->AddFrame(f.data(), f.size(), track, ts, true)) return "AddFrame (flac) failed";
            next++;
        }
        return std::string();
    }
};

long long file_size(const char* path)
{
    std::FILE* f = std::fopen(path, "rb");
    long long n = 0;
    if (f) { _fseeki64(f, 0, SEEK_END); n = _ftelli64(f); std::fclose(f); }
    return n;
}

int check_cineform(const AviMovie& m, const char* mkv)
{
    MkvTracks t;
    std::string e = t.open(mkv);
    if (!e.empty()) return fail(e);
    if (t.video_codec != kCineFormCodecId) return fail("the video track is " + t.video_codec + ", not " + kCineFormCodecId);
    if (t.video_private != m.video_format) return fail("the CodecPrivate differs from the recording's BITMAPINFOHEADER");
    if (t.video.size() != m.video.size())
        return fail("the .mkv has " + std::to_string(t.video.size()) + " video frames, the .cfhd " + std::to_string(m.video.size()));
    size_t same = 0, keys = 0;
    std::vector<uint8_t> block;
    for (size_t i = 0; i < t.video.size(); i++) {
        if (!(e = t.read(t.video[i], block)).empty()) return fail(e);
        const AviChunk& c = m.video[i];
        if (block.size() == c.size && std::memcmp(block.data(), m.data(c), c.size) == 0) same++;
        if (t.video[i].key) keys++;
    }
    std::printf("check: video %zu/%zu CFHD frames byte-identical to the .cfhd's, %zu key; CodecPrivate is the recording's %zu-byte "
                "BITMAPINFOHEADER\n", same, m.video.size(), keys, m.video_format.size());
    if (same != m.video.size()) return fail("a video frame differs from the .cfhd's");
    if (keys != m.video.size()) return fail("a CineForm block is not marked key");

    if (m.has_audio && !m.audio.empty()) {
        if (t.audio_codec != "A_FLAC") return fail("the audio track is " + t.audio_codec + ", not A_FLAC");
        const std::vector<int16_t> pcm = pcm_of(m);
        FlacTrack flac;
        flac.codec_private = t.audio_private;
        flac.samples = pcm.size() / m.channels;
        for (const MkvFrame& f : t.audio) {
            flac.frames.emplace_back();
            if (!(e = t.read(f, flac.frames.back())).empty()) return fail(e);
        }
        if (!(e = flac_verify(flac, pcm.data(), m.channels)).empty()) return fail("audio: " + e);
        std::printf("check: audio %zu FLAC blocks decode to the .cfhd's %llu PCM samples x%u bit-exact, MD5 checked\n", t.audio.size(),
                    (unsigned long long)flac.samples, m.channels);
    }
    const double want_s = double(m.video.size()) * double(m.fps_den) / double(m.fps_num);
    std::printf("check: duration %.3f s (%zu frames at %u/%u fps is %.3f s)\n", double(t.duration_ns) / 1e9, m.video.size(), m.fps_num,
                m.fps_den, want_s);
    if (std::abs(double(t.duration_ns) / 1e9 - want_s) > 0.001) return fail("the duration is not the frame count's");
    return 0;
}

std::string read_cineform(const char* in, AviMovie& m)
{
    std::string e = avi_read(in, m);
    if (e.empty()) e = avi_complete(m);
    if (e.empty() && std::strcmp(m.handler, "CFHD")) e = "the video handler is not CFHD";
    if (e.empty()) e = cineform_format(m.video_format);
    return e;
}

int mux_cineform(const char* in, const char* out)
{
    if (!ends_with(out, ".mkv")) return fail("the output must be a .mkv (the CineForm frames unchanged, the audio as FLAC)");
    const Clock::time_point t_all = Clock::now();
    AviMovie m;
    std::string e = read_cineform(in, m);
    if (!e.empty()) return fail(e);
    std::printf("input: %s, %zu bytes, %ux%u, %u/%u fps, %zu CFHD frames, %zu audio chunks\n", in, m.bytes.size(), m.width, m.height,
                m.fps_num, m.fps_den, m.video.size(), m.audio.size());

    mkvmuxer::MkvWriter writer;
    mkvmuxer::Segment seg;
    uint64_t vtrack = 0;
    if (!(e = open_segment(writer, seg, out, m, kCineFormCodecId, vtrack)).empty()) return fail(e);
    mkvmuxer::Track* video = seg.GetTrackByNumber(vtrack);
    if (!video->SetCodecPrivate(m.video_format.data(), m.video_format.size())) return fail("SetCodecPrivate (BITMAPINFOHEADER) failed");
    FlacTrack flac;
    uint64_t atrack = 0;
    if (!(e = add_flac_track(seg, m, flac, atrack)).empty()) return fail(e);
    AudioFeed feed;
    feed.seg = &seg;
    feed.flac = &flac;
    feed.track = atrack;
    feed.rate = m.mix_rate;

    const uint64_t frame_ns = 1000000000ull * m.fps_den / m.fps_num;
    for (size_t i = 0; i < m.video.size(); i++) {
        const uint64_t ts = uint64_t(i) * frame_ns;
        if (!(e = feed.flush_to(ts)).empty()) return fail(e);
        const AviChunk& c = m.video[i];
        if (!seg.AddFrame(m.data(c), c.size, vtrack, ts, true)) return fail("AddFrame (video) failed on frame " + std::to_string(i));
    }
    if (!(e = feed.flush_to(~0ull)).empty()) return fail(e);
    seg.set_duration(double(m.video.size() * frame_ns) / double(seg.GetSegmentInfo()->timecode_scale()));
    if (!seg.Finalize()) return fail("mkvmuxer Finalize failed");
    writer.Close();
    const long long out_size = file_size(out);
    std::printf("wrote %s: %lld bytes (%.2f%% of the input), %zu frames, %zu FLAC blocks, %.2f s wall\n", out, out_size,
                100.0 * double(out_size) / double(m.bytes.size()), m.video.size(), feed.next, ms_since(t_all) / 1000.0);
    return check_cineform(m, out);
}

int check(const char* in, const char* mkv)
{
    AviMovie m;
    const std::string e = read_cineform(in, m);
    if (!e.empty()) return fail(e);
    return check_cineform(m, mkv);
}

int encode(int argc, char** argv)
{
    if (argc < 4) return fail("usage: av1mkv encode <in.cfhd> <out.webm> [--cq N] [--gop N] [--gpu NAME] [--frames N] [--xmp FILE]");
    const char* in = argv[2];
    const char* out = argv[3];
    NvencAv1Settings s;
    size_t max_frames = 0;
    std::string xmp;
    for (int i = 4; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--cq")) s.cq = uint32_t(std::atoi(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--gop")) s.gop = uint32_t(std::atoi(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--gpu")) s.adapter_name_contains = argv[i + 1];
        else if (!std::strcmp(argv[i], "--frames")) max_frames = size_t(std::atoll(argv[i + 1]));
        else if (!std::strcmp(argv[i], "--xmp")) {
            const std::string e = read_xmp(argv[i + 1], xmp);
            if (!e.empty()) return fail(e);
        }
        else return fail(std::string("unknown option ") + argv[i]);
    }

    if (!ends_with(out, ".webm"))
        return fail("the output must be a .webm (AV1 video, the audio as FLAC)");

    const auto t_all = Clock::now();
    AviMovie m;
    std::string e = avi_read(in, m);
    if (!e.empty()) return fail(e);
    std::printf("input: %s, %zu bytes, %ux%u, %u/%u fps, handler %s, %zu video chunks, %zu audio chunks",
                in, m.bytes.size(), m.width, m.height, m.fps_num, m.fps_den, m.handler, m.video.size(), m.audio.size());
    if (m.has_audio) std::printf(", audio %u Hz x%u %u-bit PCM", m.mix_rate, m.channels, m.bits);
    std::printf("\n");
    if (std::strcmp(m.handler, "CFHD")) return fail("the video handler is not CFHD");

    CfhdDecoder dec;
    if (!(e = dec.open(m)).empty()) return fail(e);
    if (dec.format != CFHD_PIXEL_FORMAT_BGRA) return fail("the decoder did not give BGRA");
    if (uint32_t(dec.width) != m.width || uint32_t(dec.height) != m.height) return fail("decoded size differs from the header");

    s.width = m.width;
    s.height = m.height;
    s.fps_num = m.fps_num;
    s.fps_den = m.fps_den;
    NvencAv1 enc;
    if (!(e = enc.open(s)).empty()) return fail(e);
    std::printf("encoder: NVENC AV1 on %s, preset P6, tuning high quality, VBR constant quality cq=%u, two-pass full "
                "resolution, spatial AQ, no B-frames, no lookahead, 8-bit 4:2:0, key frame every %u frames\n",
                enc.adapter_description().c_str(), s.cq, s.gop);

    mkvmuxer::MkvWriter writer;
    mkvmuxer::Segment seg;
    uint64_t vtrack = 0;
    if (!(e = open_segment(writer, seg, out, m, mkvmuxer::Tracks::kAv1CodecId, vtrack)).empty()) return fail(e);
    mkvmuxer::Track* video = seg.GetTrackByNumber(vtrack);
    // Tags go into the segment header, which the first AddFrame writes.
    if (!xmp.empty()) {
        mkvmuxer::Tag* tag = seg.AddTag();
        if (!tag || !tag->add_simple_tag("XMP", xmp.c_str())) return fail("cannot add the XMP tag");
        std::printf("xmp: %zu bytes as the segment's XMP SimpleTag\n", xmp.size());
    }
    FlacTrack flac;
    uint64_t atrack = 0;
    if (!(e = add_flac_track(seg, m, flac, atrack)).empty()) return fail(e);
    AudioFeed feed;
    feed.seg = &seg;
    feed.flac = &flac;
    feed.track = atrack;
    feed.rate = m.mix_rate;

    const uint64_t frame_ns = 1000000000ull * m.fps_den / m.fps_num;
    const size_t frames = max_frames ? std::min(max_frames, m.video.size()) : m.video.size();
    bool have_private = false;
    size_t packets = 0, keyframes = 0;
    uint64_t out_bytes = 0;
    std::string mux_error;


    auto sink = [&](const NvencPacket& pkt) {
        if (!mux_error.empty()) return;
        std::vector<av1::Obu> obus;
        if (!av1::walk(pkt.data.data(), pkt.data.size(), obus)) { mux_error = "malformed OBU stream from NVENC"; return; }
        // The Matroska AV1 mapping: Blocks hold a Temporal Unit without its temporal
        // delimiter; the sequence header stays (it is on every key frame here).
        std::vector<uint8_t> block;
        block.reserve(pkt.data.size());
        for (const auto& o : obus) {
            if (o.type == av1::OBU_TEMPORAL_DELIMITER) continue;
            if (o.type == av1::OBU_SEQUENCE_HEADER && !have_private) {
                av1::SeqInfo info;
                if (!av1::parse_sequence_header(o.payload, o.payload_length, info)) { mux_error = "cannot parse the sequence header"; return; }
                const std::vector<uint8_t> av1c = av1::build_av1c(info, o);
                if (!video->SetCodecPrivate(av1c.data(), av1c.size())) { mux_error = "SetCodecPrivate failed"; return; }
                have_private = true;
                std::printf("av1C: profile %u level %u tier %u, %s-bit, %s, subsampling %ux%u, %ux%u max, %zu bytes\n",
                            info.profile, info.level, info.tier, info.high_bitdepth ? (info.twelve_bit ? "12" : "10") : "8",
                            info.mono ? "mono" : "colour", info.ss_x, info.ss_y, info.max_width, info.max_height, av1c.size());
            }
            block.insert(block.end(), o.start, o.start + o.length);
        }
        if (!have_private) { mux_error = "the first packet has no sequence header"; return; }
        const uint64_t ts = pkt.pts * frame_ns;
        mux_error = feed.flush_to(ts);
        if (!mux_error.empty()) return;
        if (!seg.AddFrame(block.data(), block.size(), vtrack, ts, pkt.keyframe)) { mux_error = "AddFrame (video) failed"; return; }
        packets++;
        out_bytes += block.size();
        if (pkt.keyframe) keyframes++;
    };

    double decode_ms = 0, flip_ms = 0;
    std::vector<uint8_t> top;
    const auto t_loop = Clock::now();
    for (size_t i = 0; i < frames; i++) {
        auto t = Clock::now();
        if (!(e = dec.decode(m, i)).empty()) return fail(e);
        decode_ms += ms_since(t);
        t = Clock::now();
        flip_rows(dec.frame, dec.pitch, dec.height, top);
        flip_ms += ms_since(t);
        if (!(e = enc.encode(top.data(), uint32_t(dec.pitch), i, sink)).empty()) return fail(e);
        if (!mux_error.empty()) return fail(mux_error);
        if ((i + 1) % 100 == 0) { std::printf("  %zu/%zu frames, %.1f s\r", i + 1, frames, ms_since(t_loop) / 1000.0); std::fflush(stdout); }
    }
    if (!(e = enc.finish(sink)).empty()) return fail(e);
    if (!mux_error.empty()) return fail(mux_error);
    // Audio that lands after the last video frame's time (there is one chunk per frame, so
    // normally none is left).
    if (!(e = feed.flush_to(~0ull)).empty()) return fail(e);
    const double loop_ms = ms_since(t_loop);
    const double dur_s = double(frames) * double(m.fps_den) / double(m.fps_num);
    if (!seg.Finalize()) return fail("mkvmuxer Finalize failed");
    writer.Close();
    const double all_ms = ms_since(t_all);

    const long long out_size = file_size(out);
    std::printf("\nwrote %s: %lld bytes (%.1f%% of the input), %zu frames (%zu key), %zu audio chunks, %.2f s of video\n", out,
                out_size, 100.0 * double(out_size) / double(m.bytes.size()), packets, keyframes, feed.next, dur_s);
    std::printf("time: %.2f s total wall, %.2f s decode+encode loop = %.1f fps (%.1fx realtime); per frame: CFHD decode %.2f ms, "
                "row flip %.2f ms, NVENC copy-in %.2f ms, submit %.2f ms, lock/readback %.2f ms\n",
                all_ms / 1000.0, loop_ms / 1000.0, double(frames) * 1000.0 / loop_ms, dur_s * 1000.0 / loop_ms, decode_ms / double(frames),
                flip_ms / double(frames), enc.copy_ms() / double(frames), enc.submit_ms() / double(frames), enc.lock_ms() / double(frames));
    std::printf("video bitrate: %.2f Mbit/s\n", double(out_bytes) * 8.0 / dur_s / 1e6);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc >= 2 && !std::strcmp(argv[1], "encode")) return encode(argc, argv);
    if (argc >= 4 && !std::strcmp(argv[1], "mkv")) return mux_cineform(argv[2], argv[3]);
    if (argc >= 4 && !std::strcmp(argv[1], "check")) return check(argv[2], argv[3]);
    if (argc >= 3 && !std::strcmp(argv[1], "info")) return mkv_info(argv[2]);
    if (argc >= 5 && !std::strcmp(argv[1], "dump-frame")) return dump_frame(argv[2], size_t(std::atoll(argv[3])), argv[4]);
    std::fprintf(stderr,
                 "usage:\n  av1mkv encode <in.cfhd> <out.webm> [--cq N] [--gop N] [--gpu NAME] [--frames N] [--xmp FILE]\n"
                 "  av1mkv mkv <in.cfhd> <out.mkv>\n  av1mkv check <in.cfhd> <in.mkv>\n  av1mkv info <file.webm|file.mkv>\n"
                 "  av1mkv dump-frame <in.cfhd|in.mkv> <index> <out.ppm>\n");
    return 2;
}

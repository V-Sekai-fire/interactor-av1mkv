#include "flac_track.h"

#include <FLAC/stream_decoder.h>
#include <FLAC/stream_encoder.h>

#include <cstring>

namespace {

struct EncCtx {
    FlacTrack* t;
    uint32_t block;
};

FLAC__StreamEncoderWriteStatus on_write(const FLAC__StreamEncoder*, const FLAC__byte buffer[], size_t bytes, uint32_t samples,
                                        uint32_t current_frame, void* data)
{
    auto* c = static_cast<EncCtx*>(data);
    if (samples == 0) {
        c->t->codec_private.insert(c->t->codec_private.end(), buffer, buffer + bytes);
    } else {
        c->t->frames.emplace_back(buffer, buffer + bytes);
        c->t->first_sample.push_back(uint64_t(current_frame) * c->block);
    }
    return FLAC__STREAM_ENCODER_WRITE_STATUS_OK;
}

// The final STREAMINFO (frame sizes, sample count, MD5) replaces the one written at the start.
void on_metadata(const FLAC__StreamEncoder*, const FLAC__StreamMetadata* m, void* data)
{
    auto* c = static_cast<EncCtx*>(data);
    if (m->type != FLAC__METADATA_TYPE_STREAMINFO || c->t->codec_private.size() < 42) return;
    const FLAC__StreamMetadata_StreamInfo& s = m->data.stream_info;
    uint8_t* p = c->t->codec_private.data() + 8;
    p[0] = uint8_t(s.min_blocksize >> 8), p[1] = uint8_t(s.min_blocksize);
    p[2] = uint8_t(s.max_blocksize >> 8), p[3] = uint8_t(s.max_blocksize);
    for (int i = 0; i < 3; i++) p[4 + i] = uint8_t(s.min_framesize >> (16 - 8 * i));
    for (int i = 0; i < 3; i++) p[7 + i] = uint8_t(s.max_framesize >> (16 - 8 * i));
    const uint64_t v = (uint64_t(s.sample_rate) << 44) | (uint64_t(s.channels - 1) << 41) |
                       (uint64_t(s.bits_per_sample - 1) << 36) | (s.total_samples & 0xFFFFFFFFFull);
    for (int i = 0; i < 8; i++) p[10 + i] = uint8_t(v >> (56 - 8 * i));
    std::memcpy(p + 18, s.md5sum, 16);
}

struct DecCtx {
    const FlacTrack* t;
    size_t part = 0, off = 0; // codec_private first, then the frames
    std::vector<int16_t> out;
    uint32_t channels;
    bool error = false;
};

FLAC__StreamDecoderReadStatus on_read(const FLAC__StreamDecoder*, FLAC__byte buffer[], size_t* bytes, void* data)
{
    auto* c = static_cast<DecCtx*>(data);
    size_t n = 0;
    while (n < *bytes) {
        const std::vector<uint8_t>* src = c->part == 0 ? &c->t->codec_private
                                          : c->part <= c->t->frames.size() ? &c->t->frames[c->part - 1] : nullptr;
        if (!src) break;
        const size_t k = std::min(*bytes - n, src->size() - c->off);
        std::memcpy(buffer + n, src->data() + c->off, k);
        n += k, c->off += k;
        if (c->off == src->size()) c->part++, c->off = 0;
    }
    *bytes = n;
    return n ? FLAC__STREAM_DECODER_READ_STATUS_CONTINUE : FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
}

FLAC__StreamDecoderWriteStatus on_frame(const FLAC__StreamDecoder*, const FLAC__Frame* f, const FLAC__int32* const b[], void* data)
{
    auto* c = static_cast<DecCtx*>(data);
    for (uint32_t i = 0; i < f->header.blocksize; i++)
        for (uint32_t ch = 0; ch < c->channels; ch++) c->out.push_back(int16_t(b[ch][i]));
    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void on_error(const FLAC__StreamDecoder*, FLAC__StreamDecoderErrorStatus, void* data)
{
    static_cast<DecCtx*>(data)->error = true;
}

} // namespace

std::string flac_encode(const int16_t* pcm, uint64_t samples, uint32_t channels, uint32_t rate, FlacTrack& out)
{
    const uint32_t block = 4096;
    FLAC__StreamEncoder* e = FLAC__stream_encoder_new();
    if (!e) return "FLAC__stream_encoder_new failed";
    FLAC__stream_encoder_set_channels(e, channels);
    FLAC__stream_encoder_set_bits_per_sample(e, 16);
    FLAC__stream_encoder_set_sample_rate(e, rate);
    FLAC__stream_encoder_set_compression_level(e, 8);
    FLAC__stream_encoder_set_blocksize(e, block);
    FLAC__stream_encoder_set_total_samples_estimate(e, samples);
    EncCtx ctx{ &out, block };
    if (FLAC__stream_encoder_init_stream(e, on_write, nullptr, nullptr, on_metadata, &ctx) != FLAC__STREAM_ENCODER_INIT_STATUS_OK) {
        FLAC__stream_encoder_delete(e);
        return "FLAC__stream_encoder_init_stream failed";
    }
    std::vector<FLAC__int32> buf;
    const uint64_t step = 65536;
    for (uint64_t at = 0; at < samples; at += step) {
        const uint64_t n = std::min(step, samples - at);
        buf.resize(size_t(n) * channels);
        for (size_t i = 0; i < buf.size(); i++) buf[i] = pcm[at * channels + i];
        if (!FLAC__stream_encoder_process_interleaved(e, buf.data(), uint32_t(n))) {
            const std::string why = FLAC__stream_encoder_get_resolved_state_string(e);
            FLAC__stream_encoder_delete(e);
            return "FLAC__stream_encoder_process_interleaved: " + why;
        }
    }
    const bool ok = FLAC__stream_encoder_finish(e);
    FLAC__stream_encoder_delete(e);
    if (!ok) return "FLAC__stream_encoder_finish failed";
    out.samples = samples;
    return "";
}

std::string flac_verify(const FlacTrack& t, const int16_t* pcm, uint32_t channels)
{
    FLAC__StreamDecoder* d = FLAC__stream_decoder_new();
    if (!d) return "FLAC__stream_decoder_new failed";
    FLAC__stream_decoder_set_md5_checking(d, true);
    DecCtx ctx{ &t, 0, 0, {}, channels, false };
    if (FLAC__stream_decoder_init_stream(d, on_read, nullptr, nullptr, nullptr, nullptr, on_frame, nullptr, on_error, &ctx) !=
        FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        FLAC__stream_decoder_delete(d);
        return "FLAC__stream_decoder_init_stream failed";
    }
    const bool ok = FLAC__stream_decoder_process_until_end_of_stream(d);
    const bool md5 = FLAC__stream_decoder_finish(d);
    FLAC__stream_decoder_delete(d);
    if (!ok || ctx.error) return "libFLAC could not decode the track";
    if (!md5) return "the decoded track's MD5 does not match STREAMINFO";
    if (ctx.out.size() != t.samples * channels) return "the decoded track has " + std::to_string(ctx.out.size()) + " values";
    if (std::memcmp(ctx.out.data(), pcm, ctx.out.size() * 2) != 0) return "a decoded sample differs from the PCM";
    return "";
}

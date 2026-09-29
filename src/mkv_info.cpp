// SPDX-License-Identifier: MIT
//
// `av1mkv info`: reads a Matroska file back with libwebm's mkvparser and prints what a
// player would see: doc type, duration, every track's codec id, dimensions and
// CodecPrivate, and per track the block count and key frame count. It is the FFmpeg-free
// verification of the transcoder's output.

#include <mkvparser/mkvparser.h>
#include <mkvparser/mkvreader.h>

#include <cstdio>
#include <cstring>
#include <map>

int mkv_info(const char* path)
{
    mkvparser::MkvReader reader;
    if (reader.Open(path)) { std::fprintf(stderr, "cannot open %s\n", path); return 1; }
    long long pos = 0;
    mkvparser::EBMLHeader ebml;
    if (ebml.Parse(&reader, pos) < 0) { std::fprintf(stderr, "not an EBML file\n"); return 1; }
    std::printf("doctype %s v%lld (read v%lld)\n", ebml.m_docType, ebml.m_docTypeVersion, ebml.m_docTypeReadVersion);

    mkvparser::Segment* segment = nullptr;
    if (mkvparser::Segment::CreateInstance(&reader, pos, segment) || !segment) { std::fprintf(stderr, "no segment\n"); return 1; }
    if (segment->Load() < 0) { std::fprintf(stderr, "segment load failed\n"); delete segment; return 1; }

    const mkvparser::SegmentInfo* info = segment->GetInfo();
    std::printf("timecode scale %lld ns, duration %.3f s, muxing app \"%s\", writing app \"%s\"\n", info->GetTimeCodeScale(),
                double(info->GetDuration()) / 1e9, info->GetMuxingAppAsUTF8() ? info->GetMuxingAppAsUTF8() : "",
                info->GetWritingAppAsUTF8() ? info->GetWritingAppAsUTF8() : "");

    const mkvparser::Tracks* tracks = segment->GetTracks();
    std::map<long long, const mkvparser::Track*> by_number;
    for (unsigned long i = 0; i < tracks->GetTracksCount(); i++) {
        const mkvparser::Track* t = tracks->GetTrackByIndex(i);
        if (!t) continue;
        by_number[t->GetNumber()] = t;
        size_t priv_len = 0;
        const unsigned char* priv = t->GetCodecPrivate(priv_len);
        std::printf("track %lld: type %ld, codec %s", static_cast<long long>(t->GetNumber()), static_cast<long>(t->GetType()), t->GetCodecId() ? t->GetCodecId() : "?");
        if (t->GetType() == mkvparser::Track::kVideo) {
            const auto* v = static_cast<const mkvparser::VideoTrack*>(t);
            std::printf(", %lldx%lld, frame rate %.3f, default duration %lld ns", v->GetWidth(), v->GetHeight(), v->GetFrameRate(),
                        v->GetDefaultDuration());
        } else if (t->GetType() == mkvparser::Track::kAudio) {
            const auto* a = static_cast<const mkvparser::AudioTrack*>(t);
            std::printf(", %.0f Hz, %lld ch, %lld bit", a->GetSamplingRate(), a->GetChannels(), a->GetBitDepth());
        }
        std::printf(", codec private %zu bytes", priv_len);
        if (priv && priv_len >= 4) {
            std::printf(" [");
            for (size_t k = 0; k < priv_len && k < 4; k++) std::printf("%02x", priv[k]);
            std::printf("...]");
        }
        std::printf("\n");
    }

    std::map<long long, long long> blocks, keys, bytes, last_ns, first_ns;
    long clusters = 0;
    for (const mkvparser::Cluster* c = segment->GetFirst(); c && !c->EOS(); c = segment->GetNext(c)) {
        clusters++;
        const mkvparser::BlockEntry* entry = nullptr;
        long status = c->GetFirst(entry);
        while (status >= 0 && entry && !entry->EOS()) {
            const mkvparser::Block* b = entry->GetBlock();
            const long long tn = b->GetTrackNumber();
            blocks[tn]++;
            if (b->IsKey()) keys[tn]++;
            for (int k = 0; k < b->GetFrameCount(); k++) bytes[tn] += b->GetFrame(k).len;
            const long long ns = b->GetTime(c);
            if (!first_ns.count(tn)) first_ns[tn] = ns;
            last_ns[tn] = ns;
            status = c->GetNext(entry, entry);
        }
    }
    std::printf("%ld clusters\n", clusters);
    for (const auto& kv : blocks) {
        std::printf("track %lld: %lld blocks, %lld key, %lld bytes, first %.3f s, last %.3f s\n", kv.first, kv.second, keys[kv.first],
                    bytes[kv.first], double(first_ns[kv.first]) / 1e9, double(last_ns[kv.first]) / 1e9);
    }
    // Every SimpleTag; an XMP one is checked for a whole packet, and "no tags" is said, not skipped.
    const mkvparser::Tags* tags = segment->GetTags();
    int simple = 0;
    for (int i = 0; tags && i < tags->GetTagCount(); i++) {
        const mkvparser::Tags::Tag* t = tags->GetTag(i);
        for (int k = 0; t && k < t->GetSimpleTagCount(); k++) {
            const mkvparser::Tags::SimpleTag* st = t->GetSimpleTag(k);
            const char* name = st->GetTagName() ? st->GetTagName() : "";
            const char* value = st->GetTagString() ? st->GetTagString() : "";
            simple++;
            std::printf("tag %s: %zu bytes", name, std::strlen(value));
            if (!std::strcmp(name, "XMP"))
                std::printf(", %s", std::strstr(value, "<?xpacket begin") && std::strstr(value, "<?xpacket end") ? "a whole xpacket" : "NOT an xpacket");
            std::printf("\n");
        }
    }
    if (simple == 0) std::printf("tags: none\n");
    const mkvparser::Cues* cues = segment->GetCues();
    if (cues) {
        while (cues->LoadCuePoint()) {}
        std::printf("cues: %ld cue points\n", cues->GetCount());
    }
    delete segment;
    return 0;
}

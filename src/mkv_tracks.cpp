// SPDX-License-Identifier: MIT

#include "mkv_tracks.h"

std::string mkv_truncation(mkvparser::IMkvReader* reader, const mkvparser::Segment* segment)
{
    long long total = 0;
    long long available = 0;
    if (reader->Length(&total, &available) < 0 || total < 0) return "cannot read the file length";
    long len = 0;
    const long long declared = mkvparser::ReadUInt(reader, segment->m_element_start + 4, len);
    if (declared < 0 || len < 1 || len > 8) return "the segment size does not parse";
    if (declared == (1LL << (7 * len)) - 1) return "the segment has no size (the muxer did not finish)";
    if (segment->m_start + declared > total)
        return "truncated: the segment ends at byte " + std::to_string(segment->m_start + declared) + ", the file has " +
               std::to_string(total);
    return std::string();
}

std::string MkvTracks::open(const char* path)
{
    if (reader.Open(path)) return std::string("cannot open ") + path;
    long long pos = 0;
    mkvparser::EBMLHeader ebml;
    if (ebml.Parse(&reader, pos) < 0) return "not an EBML file";
    if (mkvparser::Segment::CreateInstance(&reader, pos, segment) || !segment) return "no segment";
    const std::string cut = mkv_truncation(&reader, segment);
    if (!cut.empty()) return cut;
    if (segment->Load() < 0) return "the segment does not load";
    const mkvparser::SegmentInfo* info = segment->GetInfo();
    duration_ns = info ? info->GetDuration() : -1;

    const mkvparser::Tracks* tracks = segment->GetTracks();
    long long video_number = -1;
    long long audio_number = -1;
    for (unsigned long i = 0; tracks && i < tracks->GetTracksCount(); i++) {
        const mkvparser::Track* t = tracks->GetTrackByIndex(i);
        if (!t) continue;
        size_t len = 0;
        const unsigned char* priv = t->GetCodecPrivate(len);
        const std::string id = t->GetCodecId() ? t->GetCodecId() : "";
        if (t->GetType() == mkvparser::Track::kVideo && video_number < 0) {
            video_number = t->GetNumber();
            video_codec = id;
            if (priv) video_private.assign(priv, priv + len);
        } else if (t->GetType() == mkvparser::Track::kAudio && audio_number < 0) {
            audio_number = t->GetNumber();
            audio_codec = id;
            if (priv) audio_private.assign(priv, priv + len);
        }
    }
    if (video_number < 0) return "no video track";

    for (const mkvparser::Cluster* c = segment->GetFirst(); c && !c->EOS(); c = segment->GetNext(c)) {
        const mkvparser::BlockEntry* entry = nullptr;
        long status = c->GetFirst(entry);
        while (status >= 0 && entry && !entry->EOS()) {
            const mkvparser::Block* b = entry->GetBlock();
            std::vector<MkvFrame>* dst = b->GetTrackNumber() == video_number ? &video
                                         : b->GetTrackNumber() == audio_number ? &audio
                                                                               : nullptr;
            for (int k = 0; dst && k < b->GetFrameCount(); k++) {
                const mkvparser::Block::Frame& f = b->GetFrame(k);
                dst->push_back(MkvFrame{f.pos, f.len, b->IsKey()});
            }
            status = c->GetNext(entry, entry);
        }
        if (status < 0) return "a cluster does not parse";
    }
    return std::string();
}

std::string MkvTracks::read(const MkvFrame& f, std::vector<uint8_t>& out)
{
    out.resize(size_t(f.len));
    if (reader.Read(f.pos, f.len, out.data())) return "short read at byte " + std::to_string(f.pos);
    return std::string();
}

MkvTracks::~MkvTracks() { delete segment; }

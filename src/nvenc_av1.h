// SPDX-License-Identifier: MIT
//
// AV1 encoder over NVENC, reached through the driver's nvEncodeAPI64.dll at run time
// (nothing linked; struct layouts from V-Sekai-fire/nv-codec-headers). The session is
// opened on a D3D11 device created on the adapter whose name matches (the RTX 4090: AV1
// encode needs Ada). Frames come in as 8-bit BGRA rows in system memory; NVENC does the
// RGB to 4:2:0 YUV conversion itself.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct NvencAv1Settings {
    uint32_t width = 0, height = 0;
    uint32_t fps_num = 30, fps_den = 1;
    uint32_t gop = 60;        // key frame interval
    uint32_t cq = 22;         // targetQuality, 0..63 for AV1
    uint32_t queue_depth = 4; // frames in flight
    std::string adapter_name_contains = "RTX 4090";
};

// One encoded picture, in encode order (which is display order: no B-frames).
struct NvencPacket {
    std::vector<uint8_t> data; // OBUs, low-overhead format
    uint64_t pts = 0;           // the inputTimeStamp that went in (frame index)
    bool keyframe = false;
};

class NvencAv1 {
public:
    NvencAv1();
    ~NvencAv1();
    // Empty string on success, else the reason. Fills adapter_description on success.
    std::string open(const NvencAv1Settings& s);
    std::string adapter_description() const { return adapter_; }
    // Submits one BGRA frame (pitch in bytes). Completed packets are handed to `sink`
    // in order; each call may deliver zero or one packet (the queue lags by depth-1).
    std::string encode(const uint8_t* bgra, uint32_t pitch, uint64_t pts, const std::function<void(const NvencPacket&)>& sink);
    // Flushes the queue: delivers every outstanding packet.
    std::string finish(const std::function<void(const NvencPacket&)>& sink);
    void close();
    double submit_ms() const { return submit_ms_; }
    double lock_ms() const { return lock_ms_; }
    double copy_ms() const { return copy_ms_; }

    struct Impl;

private:
    Impl* impl_ = nullptr;
    std::string adapter_;
    double submit_ms_ = 0, lock_ms_ = 0, copy_ms_ = 0;
};

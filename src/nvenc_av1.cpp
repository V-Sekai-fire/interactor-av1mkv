// SPDX-License-Identifier: MIT
//
// See nvenc_av1.h. Modelled on tools/oxrsys/runtime/src/NvencVideoEncoder.cpp (the OXRSys
// Windows port): the same dynlink_loader.h load of nvEncodeAPI64.dll, the same session
// open on a D3D11 device, the same preset query. What differs: the input is a system
// memory buffer (nvEncCreateInputBuffer / nvEncLockInputBuffer) instead of a registered
// NV12 texture, the adapter is chosen by name, and the rate control is constant quality
// for a file rather than CBR for a stream.

#include "nvenc_av1.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <deque>

static void LogNvLoader(const char* format, ...)
{
    char message[512] = {};
    va_list args;
    va_start(args, format);
    std::vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    std::fprintf(stderr, "NVENC loader: %s\n", message);
}
#define FFNV_LOG_FUNC(logctx, msg, ...) LogNvLoader((msg), __VA_ARGS__)
#define FFNV_DEBUG_LOG_FUNC(logctx, msg, ...)
#include <ffnvcodec/dynlink_loader.h>

namespace {

using Clock = std::chrono::steady_clock;
double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

struct NvencApi {
    NvencFunctions* functions = nullptr;
    NV_ENCODE_API_FUNCTION_LIST api = {};
    bool ok = false;
    std::string error;
};

const NvencApi& GetNvencApi()
{
    static NvencApi instance = [] {
        NvencApi loaded;
        if (nvenc_load_functions(&loaded.functions, nullptr) != 0) {
            loaded.error = "nvEncodeAPI64.dll not available (no NVIDIA driver?)";
            return loaded;
        }
        uint32_t maxVersion = 0;
        loaded.functions->NvEncodeAPIGetMaxSupportedVersion(&maxVersion);
        const uint32_t headerVersion = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
        if (maxVersion < headerVersion) {
            char b[160];
            std::snprintf(b, sizeof b, "driver supports NVENC API %u.%u, headers need %u.%u", maxVersion >> 4, maxVersion & 0xf,
                          NVENCAPI_MAJOR_VERSION, NVENCAPI_MINOR_VERSION);
            loaded.error = b;
            return loaded;
        }
        loaded.api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
        if (loaded.functions->NvEncodeAPICreateInstance(&loaded.api) != NV_ENC_SUCCESS) {
            loaded.error = "NvEncodeAPICreateInstance failed";
            return loaded;
        }
        loaded.ok = true;
        return loaded;
    }();
    return instance;
}

bool SameGuid(const GUID& a, const GUID& b) { return std::memcmp(&a, &b, sizeof(GUID)) == 0; }

bool SessionHasAv1(const NvencApi& nv, void* encoder)
{
    uint32_t count = 0;
    if (nv.api.nvEncGetEncodeGUIDCount(encoder, &count) != NV_ENC_SUCCESS || count == 0) return false;
    std::vector<GUID> guids(count);
    uint32_t written = 0;
    if (nv.api.nvEncGetEncodeGUIDs(encoder, guids.data(), count, &written) != NV_ENC_SUCCESS) return false;
    for (uint32_t i = 0; i < written; i++)
        if (SameGuid(guids[i], NV_ENC_CODEC_AV1_GUID)) return true;
    return false;
}

std::string narrow(const wchar_t* w)
{
    std::string s;
    for (; *w; ++w) s.push_back(*w < 128 ? char(*w) : '?');
    return s;
}

bool contains_ci(const std::string& hay, const std::string& needle)
{
    if (needle.empty()) return true;
    std::string h = hay, n = needle;
    for (auto& c : h) c = char(std::tolower(unsigned(c)));
    for (auto& c : n) c = char(std::tolower(unsigned(c)));
    return h.find(n) != std::string::npos;
}

struct Slot {
    NV_ENC_INPUT_PTR input = nullptr;
    NV_ENC_OUTPUT_PTR output = nullptr;
    uint64_t pts = 0;
    bool pending = false;
};

} // namespace

struct NvencAv1::Impl {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    void* encoder = nullptr;
    NV_ENC_INITIALIZE_PARAMS init = {};
    NV_ENC_CONFIG config = {};
    std::vector<Slot> slots;
    std::deque<size_t> in_flight; // slot indices, submission order
    uint32_t width = 0, height = 0;

    ~Impl()
    {
        const NvencApi& nv = GetNvencApi();
        if (encoder && nv.ok) {
            for (auto& s : slots) {
                if (s.output) nv.api.nvEncDestroyBitstreamBuffer(encoder, s.output);
                if (s.input) nv.api.nvEncDestroyInputBuffer(encoder, s.input);
            }
            nv.api.nvEncDestroyEncoder(encoder);
        }
        if (context) context->Release();
        if (device) device->Release();
    }
};

NvencAv1::NvencAv1() = default;
NvencAv1::~NvencAv1() { close(); }
void NvencAv1::close() { delete impl_; impl_ = nullptr; }

std::string NvencAv1::open(const NvencAv1Settings& s)
{
    close();
    const NvencApi& nv = GetNvencApi();
    if (!nv.ok) return "NVENC: " + nv.error;

    auto impl = new Impl();
    impl->width = s.width;
    impl->height = s.height;

    // The adapter, by name. Index 0 is whichever card the OS lists first (on this desk it
    // was the 3090 on one boot and the 4090 on another), so the name decides.
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) { delete impl; return "CreateDXGIFactory1 failed"; }
    IDXGIAdapter1* chosen = nullptr;
    std::string seen;
    for (UINT i = 0;; i++) {
        IDXGIAdapter1* a = nullptr;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d = {};
        a->GetDesc1(&d);
        const std::string name = narrow(d.Description);
        seen += (seen.empty() ? "" : "; ") + name;
        if (!chosen && d.VendorId == 0x10de && contains_ci(name, s.adapter_name_contains)) {
            chosen = a;
            adapter_ = name;
        } else {
            a->Release();
        }
    }
    factory->Release();
    if (!chosen) { delete impl; return "no NVIDIA adapter whose name contains \"" + s.adapter_name_contains + "\" (adapters: " + seen + ")"; }

    const HRESULT hr = D3D11CreateDevice(chosen, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                         &impl->device, nullptr, &impl->context);
    chosen->Release();
    if (FAILED(hr)) { delete impl; return "D3D11CreateDevice failed on " + adapter_; }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params = {};
    params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    params.device = impl->device;
    params.apiVersion = NVENCAPI_VERSION;
    if (nv.api.nvEncOpenEncodeSessionEx(&params, &impl->encoder) != NV_ENC_SUCCESS || !impl->encoder) {
        impl->encoder = nullptr;
        delete impl;
        return "nvEncOpenEncodeSessionEx failed on " + adapter_;
    }
    if (!SessionHasAv1(nv, impl->encoder)) { delete impl; return adapter_ + " has no AV1 encoder (AV1 encode needs Ada or newer)"; }

    // Preset P6, tuned for quality: the file is written once and watched many times.
    NV_ENC_PRESET_CONFIG presetConfig = {};
    presetConfig.version = NV_ENC_PRESET_CONFIG_VER;
    presetConfig.presetCfg.version = NV_ENC_CONFIG_VER;
    if (nv.api.nvEncGetEncodePresetConfigEx(impl->encoder, NV_ENC_CODEC_AV1_GUID, NV_ENC_PRESET_P6_GUID,
                                            NV_ENC_TUNING_INFO_HIGH_QUALITY, &presetConfig) != NV_ENC_SUCCESS) {
        delete impl;
        return "AV1 P6 preset query failed";
    }
    NV_ENC_CONFIG& config = impl->config;
    config = presetConfig.presetCfg;
    config.version = NV_ENC_CONFIG_VER;
    config.gopLength = s.gop;
    // No B-frames: every packet is then in display order and its PTS is its DTS, so the
    // Matroska blocks go out in the order the encoder returns them.
    config.frameIntervalP = 1;
    // Constant quality: VBR with no bitrate bound and a target CQ. This is the NVENC form of
    // "quality N" (the one -cq maps to): the rate controller picks each frame's size.
    config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_VBR;
    config.rcParams.averageBitRate = 0;
    config.rcParams.maxBitRate = 0;
    config.rcParams.vbvBufferSize = 0;
    config.rcParams.vbvInitialDelay = 0;
    config.rcParams.targetQuality = uint8_t(s.cq);
    config.rcParams.targetQualityLSB = 0;
    config.rcParams.multiPass = NV_ENC_TWO_PASS_FULL_RESOLUTION;
    config.rcParams.enableAQ = 1; // spatial AQ
    config.rcParams.enableLookahead = 0;
    NV_ENC_CONFIG_AV1& av1 = config.encodeCodecConfig.av1Config;
    av1.idrPeriod = s.gop;
    av1.repeatSeqHdr = 1; // a sequence header on every key frame, so a seek lands on one
    av1.outputAnnexBFormat = 0; // low-overhead format, what Matroska carries
    av1.chromaFormatIDC = 1; // 4:2:0
    av1.inputBitDepth = NV_ENC_BIT_DEPTH_8;
    av1.outputBitDepth = NV_ENC_BIT_DEPTH_8;
    av1.level = NV_ENC_LEVEL_AV1_AUTOSELECT;
    av1.tier = NV_ENC_TIER_AV1_0;
    av1.colorPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
    av1.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_BT709;
    av1.matrixCoefficients = NV_ENC_VUI_MATRIX_COEFFS_BT709;
    av1.colorRange = 0;

    NV_ENC_INITIALIZE_PARAMS& init = impl->init;
    init.version = NV_ENC_INITIALIZE_PARAMS_VER;
    init.encodeGUID = NV_ENC_CODEC_AV1_GUID;
    init.presetGUID = NV_ENC_PRESET_P6_GUID;
    init.tuningInfo = NV_ENC_TUNING_INFO_HIGH_QUALITY;
    init.encodeWidth = s.width;
    init.encodeHeight = s.height;
    init.darWidth = s.width;
    init.darHeight = s.height;
    init.maxEncodeWidth = s.width;
    init.maxEncodeHeight = s.height;
    init.frameRateNum = s.fps_num;
    init.frameRateDen = s.fps_den;
    init.enablePTD = 1;
    init.enableEncodeAsync = 0;
    init.encodeConfig = &config;
    const NVENCSTATUS st = nv.api.nvEncInitializeEncoder(impl->encoder, &init);
    if (st != NV_ENC_SUCCESS) {
        char b[96];
        std::snprintf(b, sizeof b, "nvEncInitializeEncoder failed (%d)", int(st));
        delete impl;
        return b;
    }

    impl->slots.resize(s.queue_depth < 1 ? 1 : s.queue_depth);
    for (auto& slot : impl->slots) {
        NV_ENC_CREATE_INPUT_BUFFER in = {};
        in.version = NV_ENC_CREATE_INPUT_BUFFER_VER;
        in.width = s.width;
        in.height = s.height;
        in.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB; // A8R8G8B8 word order = B,G,R,A bytes in memory
        if (nv.api.nvEncCreateInputBuffer(impl->encoder, &in) != NV_ENC_SUCCESS) { delete impl; return "nvEncCreateInputBuffer failed"; }
        slot.input = in.inputBuffer;
        NV_ENC_CREATE_BITSTREAM_BUFFER out = {};
        out.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
        if (nv.api.nvEncCreateBitstreamBuffer(impl->encoder, &out) != NV_ENC_SUCCESS) { delete impl; return "nvEncCreateBitstreamBuffer failed"; }
        slot.output = out.bitstreamBuffer;
    }
    impl_ = impl;
    return std::string();
}

static std::string drain_one(NvencAv1::Impl* impl, const NvencApi& nv, double& lock_ms, const std::function<void(const NvencPacket&)>& sink)
{
    const size_t i = impl->in_flight.front();
    impl->in_flight.pop_front();
    Slot& slot = impl->slots[i];
    const auto t = Clock::now();
    NV_ENC_LOCK_BITSTREAM lock = {};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = slot.output;
    const NVENCSTATUS st = nv.api.nvEncLockBitstream(impl->encoder, &lock);
    if (st != NV_ENC_SUCCESS) {
        char b[96];
        std::snprintf(b, sizeof b, "nvEncLockBitstream failed (%d)", int(st));
        return b;
    }
    NvencPacket pkt;
    pkt.data.assign(static_cast<const uint8_t*>(lock.bitstreamBufferPtr),
                    static_cast<const uint8_t*>(lock.bitstreamBufferPtr) + lock.bitstreamSizeInBytes);
    pkt.pts = lock.outputTimeStamp;
    pkt.keyframe = lock.pictureType == NV_ENC_PIC_TYPE_IDR || lock.pictureType == NV_ENC_PIC_TYPE_I;
    nv.api.nvEncUnlockBitstream(impl->encoder, slot.output);
    slot.pending = false;
    lock_ms += ms_since(t);
    sink(pkt);
    return std::string();
}

std::string NvencAv1::encode(const uint8_t* bgra, uint32_t pitch, uint64_t pts, const std::function<void(const NvencPacket&)>& sink)
{
    if (!impl_) return "encoder not open";
    const NvencApi& nv = GetNvencApi();
    Impl* impl = impl_;

    // Free a slot first if every one is in flight.
    if (impl->in_flight.size() >= impl->slots.size()) {
        const std::string e = drain_one(impl, nv, lock_ms_, sink);
        if (!e.empty()) return e;
    }
    size_t idx = 0;
    for (; idx < impl->slots.size(); idx++)
        if (!impl->slots[idx].pending) break;
    Slot& slot = impl->slots[idx];

    {
        const auto t = Clock::now();
        NV_ENC_LOCK_INPUT_BUFFER lock = {};
        lock.version = NV_ENC_LOCK_INPUT_BUFFER_VER;
        lock.inputBuffer = slot.input;
        if (nv.api.nvEncLockInputBuffer(impl->encoder, &lock) != NV_ENC_SUCCESS) return "nvEncLockInputBuffer failed";
        uint8_t* dst = static_cast<uint8_t*>(lock.bufferDataPtr);
        const size_t row = size_t(impl->width) * 4;
        for (uint32_t y = 0; y < impl->height; y++)
            std::memcpy(dst + size_t(lock.pitch) * y, bgra + size_t(pitch) * y, row);
        nv.api.nvEncUnlockInputBuffer(impl->encoder, slot.input);
        copy_ms_ += ms_since(t);
    }

    NV_ENC_PIC_PARAMS pic = {};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputWidth = impl->width;
    pic.inputHeight = impl->height;
    pic.inputPitch = 0;
    pic.inputBuffer = slot.input;
    pic.bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB;
    pic.outputBitstream = slot.output;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputTimeStamp = pts;
    const auto t = Clock::now();
    const NVENCSTATUS st = nv.api.nvEncEncodePicture(impl->encoder, &pic);
    submit_ms_ += ms_since(t);
    if (st != NV_ENC_SUCCESS && st != NV_ENC_ERR_NEED_MORE_INPUT) {
        char b[96];
        std::snprintf(b, sizeof b, "nvEncEncodePicture failed (%d)", int(st));
        return b;
    }
    slot.pending = true;
    slot.pts = pts;
    impl->in_flight.push_back(idx);
    return std::string();
}

std::string NvencAv1::finish(const std::function<void(const NvencPacket&)>& sink)
{
    if (!impl_) return "encoder not open";
    const NvencApi& nv = GetNvencApi();
    NV_ENC_PIC_PARAMS eos = {};
    eos.version = NV_ENC_PIC_PARAMS_VER;
    eos.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
    nv.api.nvEncEncodePicture(impl_->encoder, &eos);
    while (!impl_->in_flight.empty()) {
        const std::string e = drain_one(impl_, nv, lock_ms_, sink);
        if (!e.empty()) return e;
    }
    return std::string();
}

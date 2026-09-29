# av1mkv: CineForm recording to AV1 and FLAC in a .webm, or to CineForm and FLAC in a .mkv

Turns the `.cfhd` files entities-godot-cineform's MovieWriter writes (CFHD 12-bit RGB
4:4:4 in an AVI/RIFF container, with a 16-bit PCM track) into a `.webm` of AV1 video and
FLAC audio, which chat clients and browsers play inline. No FFmpeg anywhere, no HEVC, no
H.264: the decoder is the org's CineForm SDK, the video encoder NVENC AV1 through the
NVIDIA driver, the audio encoder libFLAC, the muxer the org's libwebm. `av1mkv mkv` keeps
the picture lossless instead: the same CFHD frames, byte for byte, in a Matroska `.mkv`, with
the audio as FLAC.

- `src/avi_reader.h`: walks the RIFF the writer emits (`LIST hdrl` with `avih`, a `vids`
  CFHD `strl`, an `auds` PCM `strl`; `LIST movi` of `00dc`/`01wb` chunks; `idx1` unused).
- `src/main.cpp`: `CFHD_OpenDecoder` / `CFHD_PrepareToDecode` / `CFHD_DecodeSample` to
  8-bit BGRA (the 12-bit source is rounded to 8 bits: the encoder is 8-bit 4:2:0), rows
  flipped from the DIB bottom-up order the SDK's BGRA uses to top-down, then the encoder
  and the muxer.
- `src/flac_track.cpp`: the PCM track through libFLAC (level 8, 4096-sample blocks) as
  `A_FLAC`, the final STREAMINFO in the CodecPrivate. Before muxing, libFLAC's decoder reads
  the track back and every sample and the MD5 must match the PCM. WebM's spec names only
  Opus and Vorbis audio, so libwebm writes the matroska doctype.
- `src/nvenc_av1.cpp`: `nvEncodeAPI64.dll` loaded at run time with
  `ffnvcodec/dynlink_loader.h` (struct layouts from V-Sekai-fire/nv-codec-headers, the
  copy under `tools/oxrsys/third_party`), a D3D11 device on the adapter whose name
  matches `--gpu` (default `RTX 4090`; never index 0, which is the 3090 on some boots and
  has no AV1 encoder), system-memory input buffers in `NV_ENC_BUFFER_FORMAT_ARGB`, a
  4-deep queue of input/bitstream buffer pairs. Copied from
  `tools/oxrsys/runtime/src/NvencVideoEncoder.cpp`.
- `src/av1c.h`: OBU walker and a real parse of the sequence header (profile, level, tier,
  color_config) for the `av1C` CodecPrivate; temporal delimiters are dropped from the
  blocks, sequence headers stay on every key frame.
- `src/mkv_info.cpp`: `av1mkv info`, the verification: reads the file back with
  libwebm's mkvparser.
- `src/mkv_tracks.cpp`: reads a `.mkv` back for `av1mkv check` and `dump-frame`. It refuses a
  file whose segment runs past its end; mkvparser alone reads that as a segment of unknown size.

## Build

```
tools/av1mkv/build.sh        # llvm-mingw clang + CMake + Ninja -> build/av1mkv.exe
```

Dependencies, all from github.com/V-Sekai-fire as squashed subtrees under `thirdparty/`
(each with a `CITATION.cff`): `cineform-sdk` (9c2973f, Apache-2.0/MIT; built here from
source as `CFHDCodecStatic`, OpenMP off, the two `-Wno-incompatible-*` flags
entities-godot-cineform builds it with, `--allow-multiple-definition` for its duplicated
`GetProcessorCount`) and `libwebm` (6184f44, BSD-3; mkvmuxer and mkvparser compiled in).
`thirdparty/flac` is libFLAC from V-Sekai-fire/flac at tag 1.5.0, imported as the BSD-3 library
only: the flac and metaflac programs, the tests, libFLAC++ and the GPL and LGPL helpers are not in
the tree (its `CITATION.cff` records what was kept). One static exe, nothing else next to it.

## Usage

```
av1mkv encode <in.cfhd> <out.webm> [--cq 22] [--gop 60] [--gpu "RTX 4090"] [--frames N] [--xmp packet.xml]
av1mkv mkv <in.cfhd> <out.mkv>
av1mkv check <in.cfhd> <in.mkv>
av1mkv info <file.webm|file.mkv>
av1mkv dump-frame <in.cfhd|in.mkv> <index> <out.ppm>
```

`encode` writes only a `.webm` and `mkv` only a `.mkv`; any other output extension is refused.

## The .mkv (CineForm and FLAC)

The video track uses the Matroska codec specification's `V_MS/VFW/FOURCC` mapping: the
CodecPrivate is a BITMAPINFOHEADER, and a block is one frame as the AVI stored it. The
BITMAPINFOHEADER is the recording's own `vids` `strf`, copied unchanged (40 bytes, 24-bit,
FOURCC `CFHD`). Every CFHD frame is intra, so every block is a key frame; libwebm starts a
cluster and a cue point at each one. The Duration is the frame count over the frame rate.
The audio is the same libFLAC track `encode` writes. The doctype is `matroska`.

`mkv` refuses a recording the writer did not finish: the RIFF header must count no more bytes
than the file has, and the `movi` list must hold the frame count `avih` states. After it
writes the file, `mkv` runs `check`, which reads the `.mkv` back with mkvparser and requires:

- the CodecPrivate equals the recording's BITMAPINFOHEADER;
- every video block equals its `00dc` chunk byte for byte, and each is a key frame;
- libFLAC decodes the FLAC blocks to the recording's PCM, every sample and the MD5;
- the Duration is within 1 ms of the frame count over the frame rate.

`--xmp` puts an XMP packet (the file's whole `<?xpacket begin ... end?>` text) into the
segment as a `Tags/Tag/SimpleTag` named `XMP`, so the file carries its own description.
`info` lists every tag and says whether an `XMP` one is a whole packet; a file with none
prints `tags: none`.

## Encoder settings (recorded)

`NV_ENC_CODEC_AV1_GUID`, preset P6, `NV_ENC_TUNING_INFO_HIGH_QUALITY`, rate control
`NV_ENC_PARAMS_RC_VBR` with `averageBitRate = maxBitRate = 0` and `targetQuality = 22`
(NVENC's constant-quality form), two-pass full resolution, spatial AQ, no lookahead, no
B-frames (`frameIntervalP = 1`, so packets are in display order and PTS = DTS), 8-bit
4:2:0 (`chromaFormatIDC = 1`; the 4:4:4 of the source is not kept, NVENC AV1 has no 4:4:4),
`gopLength = idrPeriod = 60`, `repeatSeqHdr = 1`, low-overhead OBU format. Colour is
signalled BT.709 limited; the RGB to YUV conversion is NVENC's own for ARGB input.

## The run (2026-09-29, the Gate 10b sit chart clip)

Input `contact_kimodo_soma_sit.cfhd` (the `gate-10b-motion-contact` release): 33,644,474
bytes, 1152x648, 30 fps, 120 CFHD frames, 120 PCM chunks (48 kHz stereo 16-bit, silence).

Output `contact_kimodo_soma_sit.webm`: 394,347 bytes (1.2% of the input). The AV1 video is
392,028 bytes (0.78 Mbit/s at CQ 22), the FLAC audio 660 bytes against 768,000 of PCM, in
47 frames. Two key frames, two clusters, two cue points, 3.967 s.

Time: 0.78 s wall in total, 0.45 s for the decode+encode loop = 268 fps, 8.9x realtime.
Per frame: CFHD decode 2.49 ms, row flip 0.30 ms, copy into the NVENC input buffer
0.36 ms, `nvEncEncodePicture` 0.46 ms, bitstream lock 0.06 ms.

`av1C`: profile 0, level 23 (5.3, as NVENC autoselected), tier 1, 8-bit, 4:2:0, 20 bytes.

## Verification

- `av1mkv info` (libwebm mkvparser): doctype `matroska` v4; track 1 `V_AV1` 1152x648 at 30
  fps, CodecPrivate 20 bytes, 120 blocks, 2 key; track 2 `A_FLAC` 48000 Hz 2 ch 16 bit,
  CodecPrivate 86 bytes (`fLaC` and the STREAMINFO), 47 blocks.
- The FLAC track decodes back bit-exact through libFLAC, MD5 checked, before it is muxed.
- A chat client played the sit and walk clips inline, picture and all, when uploaded.
- A `.mkv` output is refused: "the output must be a .webm (AV1 video, the audio as FLAC)".

### The .mkv (2026-09-29, the first rung's pen and wear clip on Mire)

`wear_mire.cfhd`, 15,823,630 bytes, 1152x648, 30 fps, 97 frames, recorded by Gate 8
(`--gate=pen --pen=scripted`) on the stock single-precision engine. `av1mkv mkv` wrote
`wear_mire.mkv`, 15,203,118 bytes (96.08% of the input: 620,800 bytes of silent PCM became
534 bytes of FLAC),
in 0.04 s.

- `check`: 97 of 97 video blocks equal the `.cfhd`'s chunks, 97 key; the 38 FLAC blocks
  decode to the 155,200 PCM samples x2; Duration 3.233 s.
- `info`: track 1 `V_MS/VFW/FOURCC` 1152x648, BITMAPINFOHEADER FOURCC `CFHD` 24-bit; track 2
  `A_FLAC` 48000 Hz 2 ch; 97 clusters, 97 cue points.
- `dump-frame` of frames 48 and 96 from the `.mkv` and from the `.cfhd` gives the same PPM
  byte for byte. Control: frame 48 of the `.mkv` against frame 96 of the `.cfhd` differs
  (69,164 of 746,496 pixels).
- Controls, each refused: the `.cfhd` cut to 9,500,000 bytes ("truncated: the RIFF header
  counts 15823630 bytes, the file has 9500000"); the `.mkv` cut to 9,100,000 bytes, in
  `info`, `check` and `dump-frame` ("truncated: the segment ends at byte 15203118, the file
  has 9100000"); one byte of video block 50 changed ("a video frame differs", 96 of 97
  equal); one byte of FLAC block 10 changed ("libFLAC could not decode the track"); a
  `.webm` output name for `mkv`.

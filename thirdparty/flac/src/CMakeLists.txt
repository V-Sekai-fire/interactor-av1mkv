option(ENABLE_64_BIT_WORDS "Set FLAC__BYTES_PER_WORD to 8, for 64-bit machines. For 32-bit machines, turning this off might give a tiny speed improvement" ON)

# Stripped for tools/av1mkv: libFLAC only (BSD-3). The programs, tests, libFLAC++ and the
# GPL/LGPL share/ helpers of V-Sekai-fire/flac 1.5.0 are not in this tree.
add_subdirectory("libFLAC")

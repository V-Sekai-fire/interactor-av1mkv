# interactor-av1mkv

A Windows tool that rewraps `.cfhd` recordings into Matroska with FLAC audio, and a script that delivers each with its citation file.

## What it is for

The engine's movie writer records `.cfhd` video with PCM audio in a RIFF container. `av1mkv` copies those frames byte for byte into an `.mkv`, encodes the audio as FLAC, and reads the result back to check every frame and sample; it can also encode an AV1 `.webm` on the GPU. `deliver.exs` turns a recording into the `.mkv` and `.cff` that RFD 2294 says every video ships as.

## Build and run

    ./build.sh
    elixir deliver.exs --self-test

`av1mkv` run with no arguments lists its commands.

## Licence

MIT, as the SPDX headers in the source state. Vendored projects under `thirdparty/` carry their own licences.

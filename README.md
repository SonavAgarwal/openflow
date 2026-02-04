# OpenFlow (minimal)

Minimal macOS menu-bar app with a bubble overlay and history.

## Build & run (Xcode)

Xcode can open SwiftPM packages directly.

1. Open Xcode
2. File → Open… → select this folder (the one with `Package.swift`)
3. Select the `openflow` scheme and run

## Build & run (CLI)

```bash
swift build
swift run
```

## Whisper setup (C++ transcriber + VAD)

This repo includes a `transcriber/` folder with a CMake build for the C++ transcriber
and VAD. It mirrors the setup from your other repo.

```bash
cd transcriber
./scripts/setup_whisper.sh
```

This will:

- initialize/update the `transcriber/whisper.cpp` submodule
- apply the ARM NEON patch
- download `base.en` + `small.en` models
- build `transcriber` and `vad_transcriber`
- download `ggml-silero-v5.1.2.bin` for VAD

Binary paths:

```
transcriber/build/bin/transcriber
transcriber/build/bin/vad_transcriber
```

## Config

Config file is read from:

```
~/.config/openflow/config.json
```

Example:

```json
{
  "apiKey": "YOUR_KEY_HERE"
}
```

Optional dictionary file (used to bias decoding + prompt for VAD/Whisper):

```
~/.config/openflow/dictionary.txt
```

You can override the dictionary path in `config.json`:

```json
{
  "dictionaryPath": "/absolute/path/to/dictionary.txt"
}
```

## History

History file is stored at:

```
~/.config/openflow/history.jsonl
```

Each line is a JSON object. You can append items via:

```bash
./.build/debug/openflow --add "Hello world"
```

The app will also copy the added text to your clipboard and then exit.

## Menu

- Toggle Bubble: show/hide overlay
- Reload History: re-reads history file
- History: click an item to copy to clipboard
- Quit

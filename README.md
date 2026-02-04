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

## Build a .app bundle (for distribution)

```bash
./scripts/build_app.sh
./scripts/install_app.sh
```

This creates `dist/OpenFlow.app` and installs it to `~/Applications/OpenFlow.app`.

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
- build `transcriber` and `openflow_transcriber`
- download `ggml-silero-v5.1.2.bin` for VAD

Binary paths:

```
transcriber/build/bin/transcriber
transcriber/build/bin/openflow_transcriber
```

## Config

Config file is read from:

```
~/.openflow/config.json
```

Example:

```json
{
  "apiKey": "YOUR_KEY_HERE"
}
```

LLM refinement uses OpenRouter. Provide an API key in config or via env:

- `OPENROUTER_API_KEY` environment variable
- `apiKey` in `~/.openflow/config.json`

Optional dictionary file (used to bias decoding + prompt for VAD/Whisper):

```
~/.openflow/dictionary.txt
```

You can override the dictionary path in `config.json`:

```json
{
  "dictionaryPath": "/absolute/path/to/dictionary.txt"
}
```

You can select the whisper model in `config.json` (defaults to `small`):

```json
{
  "model": "small"
}
```

You can embed the dictionary in `config.json`:

```json
{
  "dictionaryText": ["openai", "openflow", "whisper"]
}
```

VAD thresholds are configurable:

```json
{
  "vadStart": 0.2,
  "vadStop": 0.1
}
```

Styles are configurable. If omitted, defaults are created on first run.

```json
{
  "styles": [
    {
      "id": "default",
      "name": "Default",
      "systemPrompt": "Determine the intended style to the best of your ability. Use proper punctuation and capitalization and respect the original style."
    },
    {
      "id": "casual",
      "name": "Casual",
      "systemPrompt": "Use a casual, friendly tone. Contractions are fine. Keep it natural."
    },
    {
      "id": "formal",
      "name": "Formal",
      "systemPrompt": "Use a formal, professional tone. Avoid contractions. Keep it polished."
    }
  ],
  "selectedStyleId": "casual"
}
```

## History

History file is stored at:

```
~/.openflow/history.jsonl
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

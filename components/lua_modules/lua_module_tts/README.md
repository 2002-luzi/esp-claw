# Lua TTS

This module exposes text-to-speech playback to Lua.

## How to call
- Import it with `local tts = require("tts")`
- Call `tts.init(opts)` to initialize the audio output from board manager. By default it uses `audio_dac`.
- Call `tts.play("text")` to request speech from the configured provider and play it through the speaker.
- Call `tts.close()` when TTS is no longer needed.
- `tts.tts_init` and `tts.tts_play` are aliases for scripts that prefer explicit names.

## Options
Provider settings are device configuration, not script-local options. Configure
the TTS provider, API key, base URL, model, voice, and timeout from the web
settings UI before running scripts.

`tts.init(opts)` and `tts.play(text, opts)` accept only runtime playback
controls:
- `audio_device` or `device`: board manager audio output device, defaults to `audio_dac`.
- `volume`: output volume, 0..100, defaults to 80.
- `timeout_ms`: HTTP timeout, defaults to the configured `tts_timeout_ms`.
- `style`: optional provider-specific style instruction for the current request.

Passing service configuration options such as `provider`, `api_key`,
`base_url`, `model`, or `voice` from Lua returns an error. These values must be
stored in the device settings.

The module reads persisted settings from:
`tts_provider`, `tts_api_key`, `tts_base_url`, `tts_model`, `tts_voice`, and
`tts_timeout_ms`.

## Example
```lua
local tts = require("tts")

assert(tts.init({
    volume = 70,
}))

local result = assert(tts.play("hello from ESP-Claw"))
print("played bytes:", result.audio_bytes)

tts.close()
```

The same example is available as `test/tts_play.lua`.

# Lua TTS

This module exposes text-to-speech playback to Lua.

## How to call
- Import it with `local tts = require("tts")`
- Call `tts.init(opts)` to initialize the audio output from board manager. By default it uses `audio_dac`.
- Call `tts.play("text")` to request speech from the configured provider and play it through the speaker.
- Call `tts.close()` when TTS is no longer needed.
- `tts.tts_init` and `tts.tts_play` are aliases for scripts that prefer explicit names.

## Options
`tts.init(opts)` and `tts.play(text, opts)` accept:
- `provider`: currently `xiao_mimo`
- `api_key`: Xiao MiMo API key
- `base_url`: defaults to `https://api.xiaomimimo.com/v1`
- `model`: defaults to `mimo-v2.5-tts`
- `voice`: provider voice name
- `style`: optional style prompt passed as a MiMo `user` message
- `audio_device` or `device`: board manager audio output device, defaults to `audio_dac`
- `volume`: output volume, 0..100
- `timeout_ms`: HTTP timeout

If an option is not passed, the module also looks for persisted settings:
`tts_provider`, `tts_device`, `tts_api_key`, `tts_base_url`, `tts_model`, `tts_voice`, and `tts_style`.

## Example
```lua
local tts = require("tts")

local API_KEY = "REPLACE_WITH_YOUR_XIAO_MIMO_KEY"

assert(tts.init({
    volume = 70,
}))

local result = assert(tts.play("hello from ESP-Claw", {
    api_key = API_KEY,
    voice = "mimo_default",
}))
print("played bytes:", result.audio_bytes)

tts.close()
```

The same example is available as `test/tts_play.lua`.

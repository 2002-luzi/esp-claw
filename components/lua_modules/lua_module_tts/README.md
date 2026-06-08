# Lua API: TTS

Use this module when a Lua script needs to synthesize speech and play it
through the board audio output. Service credentials and default provider
parameters are managed by the device, so scripts should only choose playback
behavior and per-request speech overrides.

## Import

```lua
local tts = require("tts")
```

## Functions

### `tts.init(opts)`

Initializes the audio output. This is optional because `tts.play()` initializes
on first use, but call it explicitly when the script needs to choose the audio
device or volume before playback.

- `audio_device` or `device`: board manager audio output device, defaults to `audio_dac`.
- `volume`: output volume, 0..100, defaults to 80.
- `timeout_ms`: HTTP timeout, defaults to the configured `tts_timeout_ms`.

Returns `true` on success, or `nil, err` on failure.

### `tts.play(text, opts)`

Synthesizes `text`, streams the returned PCM to the speaker, and returns a
result table:

```lua
{
    ok = true,
    audio_bytes = 1234,
    http_bytes = 5678,
}
```

`opts` accepts the same runtime playback controls as `tts.init()`. When audio
has already been initialized, changing `audio_device`, `device`, or `volume`
from `tts.play()` returns an error; use `tts.init()` first for those.

`tts.play()` also accepts per-request speech controls:

- `voice`: optional provider voice override for the current request.
- `style`: optional provider-specific style instruction for the current request, up to 512 characters.

Returns `result` on success, or `nil, err` on failure.

### `tts.close()`

Closes the audio output and releases module runtime state. Returns `true`.

### Aliases

`tts.tts_init` is an alias for `tts.init`.
`tts.tts_play` is an alias for `tts.play`.

## Script Boundaries

Passing service configuration options such as `provider`, `api_key`,
`base_url`, `model`, `appid`, `app_id`, `access_token`, `token`, `cluster`,
`speaker`, or `resource_id` from Lua returns an error. Agents should not ask
users for provider credentials in Lua scripts.

## Example

```lua
local tts = require("tts")

assert(tts.init({
    volume = 70,
}))

local result = assert(tts.play("hello from ESP-Claw"))
print("played bytes:", result.audio_bytes)

local lively = assert(tts.play("hello again", {
    voice = "default",
    style = "Speak warmly and naturally.",
}))
print("played bytes:", lively.audio_bytes)

tts.close()
```

The same example is available as `test/tts_play.lua`.

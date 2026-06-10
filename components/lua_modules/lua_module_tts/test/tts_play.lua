local tts = require("tts")

local TEXT = "hello from ESP-Claw"

assert(tts.init({
    volume = 70,
}))

local ok, err = xpcall(function()
    local result = assert(tts.play(TEXT))
    print(string.format("[tts_play] audio_bytes=%d http_bytes=%d", result.audio_bytes, result.http_bytes))
end, debug.traceback)

pcall(function() tts.close() end)
if not ok then
    error(err)
end

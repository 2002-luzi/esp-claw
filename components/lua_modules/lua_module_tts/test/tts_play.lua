local tts = require("tts")

local API_KEY = "REPLACE_WITH_YOUR_XIAO_MIMO_KEY"
local TEXT = "hello from ESP-Claw"
local VOICE = "mimo_default"

if API_KEY == "" or API_KEY == "REPLACE_WITH_YOUR_XIAO_MIMO_KEY" then
    error("edit API_KEY in this script before running tts_play.lua")
end

assert(tts.init({
    volume = 70,
}))

local ok, err = xpcall(function()
    local result = assert(tts.play(TEXT, {
        api_key = API_KEY,
        voice = VOICE,
    }))
    print(string.format("[tts_play] audio_bytes=%d http_bytes=%d", result.audio_bytes, result.http_bytes))
end, debug.traceback)

pcall(function() tts.close() end)
if not ok then
    error(err)
end

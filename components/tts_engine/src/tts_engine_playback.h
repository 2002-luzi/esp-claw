#pragma once

#include <stddef.h>
#include <stdint.h>

#include "tts_engine_provider.h"
#include "tts_engine_speaker.h"

esp_err_t tts_engine_playback_init(const tts_engine_speaker_t *speaker);
esp_err_t tts_engine_playback_enqueue_audio(const tts_engine_provider_stream_status_t *status,
                                            const uint8_t *audio,
                                            size_t audio_len);
esp_err_t tts_engine_playback_complete(uint32_t session_id);
esp_err_t tts_engine_playback_stop(void);

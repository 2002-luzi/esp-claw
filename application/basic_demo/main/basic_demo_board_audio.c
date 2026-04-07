#include "basic_demo_board_audio.h"

#include <inttypes.h>
#include <string.h>

#include "audio_codec_data_if.h"
#include "driver/gpio.h"
#include "driver/i2s_pdm.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "soc/gpio_sig_map.h"

static const char *TAG = "basic_demo_audio";

#define BASIC_DEMO_AUDIO_SAMPLE_RATE   16000
#define AUDIO_PDM_SPEAK_P_GPIO         GPIO_NUM_6
#define AUDIO_PDM_SPEAK_N_GPIO         GPIO_NUM_7
#define AUDIO_PA_CTL_GPIO              GPIO_NUM_3
#define BASIC_DEMO_AUDIO_OUTPUT_VOLUME 70

#define BASIC_DEMO_I2S_GPIO_CFG(_dout)                                       \
    {                                                                        \
        .clk = GPIO_NUM_NC,                                                  \
        .dout = (_dout),                                                     \
        .invert_flags = {                                                    \
            .clk_inv = false,                                                \
        },                                                                   \
    }

#define BASIC_DEMO_I2S_PDM_TX_CFG(_sample_rate, _dout)                       \
    {                                                                        \
        .clk_cfg = I2S_PDM_TX_CLK_DEFAULT_CONFIG(_sample_rate),              \
        .slot_cfg = I2S_PDM_TX_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, \
                                                   I2S_SLOT_MODE_MONO),      \
        .gpio_cfg = BASIC_DEMO_I2S_GPIO_CFG(_dout),                          \
    }

static i2s_chan_handle_t s_tx_handle;
static const audio_codec_data_if_t *s_i2s_data_if;
static esp_codec_dev_handle_t s_speaker_dev;

static esp_err_t basic_demo_board_audio_set_pa_enabled(bool enabled)
{
    esp_err_t ret = gpio_set_level(AUDIO_PA_CTL_GPIO, enabled ? 1 : 0);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "gpio_set_level PA %s failed: %s",
                 enabled ? "high" : "low",
                 esp_err_to_name(ret));
    }

    return ret;
}

static void basic_demo_board_audio_cleanup(void)
{
    if (s_speaker_dev) {
        esp_codec_dev_delete(s_speaker_dev);
        s_speaker_dev = NULL;
    }
    if (s_i2s_data_if) {
        audio_codec_delete_data_if(s_i2s_data_if);
        s_i2s_data_if = NULL;
    }
    if (s_tx_handle) {
        i2s_channel_disable(s_tx_handle);
        i2s_del_channel(s_tx_handle);
        s_tx_handle = NULL;
    }
    basic_demo_board_audio_set_pa_enabled(false);
}

static esp_err_t basic_demo_board_audio_init_pa_ctrl(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << AUDIO_PA_CTL_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret;

    ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config PA failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return basic_demo_board_audio_set_pa_enabled(false);
}

static esp_err_t basic_demo_board_audio_init_pdm_output(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_pdm_tx_config_t pdm_cfg =
        BASIC_DEMO_I2S_PDM_TX_CFG(BASIC_DEMO_AUDIO_SAMPLE_RATE,
                                  AUDIO_PDM_SPEAK_P_GPIO);
    esp_err_t ret;

    chan_cfg.auto_clear = true;
    ret = i2s_new_channel(&chan_cfg, &s_tx_handle, NULL);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    pdm_cfg.clk_cfg.up_sample_fs = BASIC_DEMO_AUDIO_SAMPLE_RATE / 100;
    pdm_cfg.slot_cfg.sd_scale = I2S_PDM_SIG_SCALING_MUL_4;
    pdm_cfg.slot_cfg.hp_scale = I2S_PDM_SIG_SCALING_MUL_4;
    pdm_cfg.slot_cfg.lp_scale = I2S_PDM_SIG_SCALING_MUL_4;
    pdm_cfg.slot_cfg.sinc_scale = I2S_PDM_SIG_SCALING_MUL_4;

    ret = i2s_channel_init_pdm_tx_mode(s_tx_handle, &pdm_cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_pdm_tx_mode failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2s_channel_enable(s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = gpio_set_drive_capability(AUDIO_PDM_SPEAK_P_GPIO, GPIO_DRIVE_CAP_0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_set_drive_capability P failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_rom_gpio_pad_select_gpio(AUDIO_PDM_SPEAK_N_GPIO);
    ret = gpio_set_direction(AUDIO_PDM_SPEAK_N_GPIO, GPIO_MODE_OUTPUT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_set_direction N failed: %s", esp_err_to_name(ret));
        return ret;
    }
    esp_rom_gpio_connect_out_signal(AUDIO_PDM_SPEAK_N_GPIO,
                                    I2S0O_SD_OUT_IDX,
                                    true,
                                    false);

    ret = gpio_set_drive_capability(AUDIO_PDM_SPEAK_N_GPIO, GPIO_DRIVE_CAP_0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_set_drive_capability N failed: %s", esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

static esp_err_t basic_demo_board_audio_create_codec_dev(void)
{
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = NULL,
        .tx_handle = s_tx_handle,
    };
    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = NULL,
        .data_if = NULL,
    };

    s_i2s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!s_i2s_data_if) {
        ESP_LOGE(TAG, "audio_codec_new_i2s_data failed");
        return ESP_FAIL;
    }

    codec_dev_cfg.data_if = s_i2s_data_if;
    s_speaker_dev = esp_codec_dev_new(&codec_dev_cfg);
    if (!s_speaker_dev) {
        ESP_LOGE(TAG, "esp_codec_dev_new failed");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t basic_demo_board_audio_prime_volume(void)
{
    esp_codec_dev_sample_info_t sample_info = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = BASIC_DEMO_AUDIO_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    int ret;

    ret = esp_codec_dev_open(s_speaker_dev, &sample_info);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open for volume prime failed: %d", ret);
        return ESP_FAIL;
    }

    ret = esp_codec_dev_set_out_vol(s_speaker_dev, BASIC_DEMO_AUDIO_OUTPUT_VOLUME);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_set_out_vol failed: %d", ret);
        esp_codec_dev_close(s_speaker_dev);
        return ESP_FAIL;
    }

    ret = esp_codec_dev_close(s_speaker_dev);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_close after volume prime failed: %d", ret);
        return ESP_FAIL;
    }

    ret = basic_demo_board_audio_set_pa_enabled(false);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = i2s_channel_enable(s_tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG,
                 "i2s_channel_enable after volume prime failed: %s",
                 esp_err_to_name(ret));
        return ret;
    }

    return ESP_OK;
}

static esp_err_t basic_demo_board_audio_speaker_open(void *user_ctx,
                                                     const tts_engine_speaker_format_t *format)
{
    esp_codec_dev_sample_info_t sample_info = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = BASIC_DEMO_AUDIO_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    int ret;

    (void)user_ctx;

    if (!s_speaker_dev || !format) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!format->codec_is_raw) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (format->bits_per_sample != 0) {
        sample_info.bits_per_sample = format->bits_per_sample;
    }
    if (format->channels != 0) {
        sample_info.channel = format->channels;
    }
    if (format->sample_rate_hz != 0) {
        sample_info.sample_rate = format->sample_rate_hz;
    }

    ret = esp_codec_dev_open(s_speaker_dev, &sample_info);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG,
                 "esp_codec_dev_open failed rate=%" PRIu32 " ch=%u bits=%u ret=%d",
                 sample_info.sample_rate,
                 sample_info.channel,
                 sample_info.bits_per_sample,
                 ret);
        return ESP_FAIL;
    }

    ret = basic_demo_board_audio_set_pa_enabled(true);
    if (ret != ESP_OK) {
        esp_codec_dev_close(s_speaker_dev);
        return ret;
    }

    return ESP_OK;
}

static esp_err_t basic_demo_board_audio_speaker_write(void *user_ctx,
                                                      const uint8_t *audio,
                                                      size_t audio_len)
{
    int ret;

    (void)user_ctx;

    if (!s_speaker_dev || !audio || audio_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    ret = esp_codec_dev_write(s_speaker_dev, (void *)audio, (int)audio_len);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_write failed len=%u ret=%d", (unsigned)audio_len, ret);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t basic_demo_board_audio_speaker_close(void *user_ctx)
{
    int ret;

    (void)user_ctx;

    if (!s_speaker_dev) {
        return ESP_OK;
    }

    ret = esp_codec_dev_close(s_speaker_dev);
    if (ret != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_close failed: %d", ret);
        return ESP_FAIL;
    }

    return basic_demo_board_audio_set_pa_enabled(false);
}

esp_err_t basic_demo_board_audio_init(tts_engine_speaker_t *speaker)
{
    esp_err_t ret;

    if (!speaker) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(speaker, 0, sizeof(*speaker));

    if (!s_speaker_dev) {
        ret = basic_demo_board_audio_init_pa_ctrl();
        if (ret != ESP_OK) {
            return ret;
        }

        ret = basic_demo_board_audio_init_pdm_output();
        if (ret != ESP_OK) {
            basic_demo_board_audio_cleanup();
            return ret;
        }

        ret = basic_demo_board_audio_create_codec_dev();
        if (ret != ESP_OK) {
            basic_demo_board_audio_cleanup();
            return ret;
        }

        ret = basic_demo_board_audio_prime_volume();
        if (ret != ESP_OK) {
            basic_demo_board_audio_cleanup();
            return ret;
        }

        ESP_LOGI(TAG,
                 "Board speaker initialized on GPIO%d/GPIO%d PA=%d",
                 AUDIO_PDM_SPEAK_P_GPIO,
                 AUDIO_PDM_SPEAK_N_GPIO,
                 AUDIO_PA_CTL_GPIO);
    }

    speaker->user_ctx = NULL;
    speaker->open = basic_demo_board_audio_speaker_open;
    speaker->write = basic_demo_board_audio_speaker_write;
    speaker->close = basic_demo_board_audio_speaker_close;
    return ESP_OK;
}

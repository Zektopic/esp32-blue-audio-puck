/*
 * SPDX-FileCopyrightText: 2026 Zektopic
 *
 * SPDX-License-Identifier: MIT
 *
 * BlueAudio Puck -- application entry point.
 *
 * Brings up the Bluetooth Classic stack as an A2DP sink and pipes decoded PCM
 * into the I2S output path. Bluedroid calls back on its own task, so the only
 * work done in a callback is either a ring buffer write (audio) or a dispatch
 * onto the application task (events).
 */

#include <inttypes.h>
#include <string.h>

#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "esp_bt_device.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_system.h"

#include "audio_dsp.h"
#include "audio_sink.h"
#include "bt_core.h"
#include "puck_avrcp.h"
#include "puck_ui.h"

static const char *TAG = "puck";

/* Work item ids for bt_core_dispatch(). */
enum {
    PUCK_WORK_STACK_UP = 0,
    PUCK_WORK_A2DP_EVT,
};

static const char *const s_conn_state[] = {"disconnected", "connecting", "connected", "disconnecting"};
static const char *const s_audio_state[] = {"suspended", "started"};

/*
 * Re-open the puck to new sources.
 *
 * Dropping the current link first is deliberate: a source that is already
 * connected will usually reconnect instantly, which would leave the user
 * staring at a pairing light that never finds their other phone.
 */
static void enter_pairing_mode(void)
{
    ESP_LOGI(TAG, "entering pairing mode");
    esp_a2d_sink_disconnect(NULL);
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    puck_ui_set_state(PUCK_UI_PAIRING);
}

/* Runs on the UI task. */
static void on_gesture(puck_ui_gesture_t gesture)
{
    switch (gesture) {
    case PUCK_UI_PRESS_SINGLE:
        /* One button, so play and pause share it: ask for whichever the
         * source is not currently doing. */
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            puck_avrcp_send_key(puck_avrcp_is_playing() ? ESP_AVRC_PT_CMD_PAUSE
                                                        : ESP_AVRC_PT_CMD_PLAY));
        break;
    case PUCK_UI_PRESS_DOUBLE:
        ESP_ERROR_CHECK_WITHOUT_ABORT(puck_avrcp_send_key(ESP_AVRC_PT_CMD_FORWARD));
        break;
    case PUCK_UI_PRESS_TRIPLE:
        ESP_ERROR_CHECK_WITHOUT_ABORT(puck_avrcp_send_key(ESP_AVRC_PT_CMD_BACKWARD));
        break;
    case PUCK_UI_PRESS_LONG:
        enter_pairing_mode();
        break;
    case PUCK_UI_VOLUME_UP:
        puck_avrcp_adjust_volume(CONFIG_PUCK_VOLUME_STEP);
        break;
    case PUCK_UI_VOLUME_DOWN:
        puck_avrcp_adjust_volume(-CONFIG_PUCK_VOLUME_STEP);
        break;
    default:
        break;
    }
}

/* Runs on the application task, via AVRCP. */
static void on_track_change(const puck_track_info_t *info)
{
    ESP_LOGI(TAG, "now playing: %s -- %s (%s)",
             info->artist[0] ? info->artist : "?",
             info->title[0] ? info->title : "?",
             info->album[0] ? info->album : "?");
}

/**
 * @brief Translate the negotiated SBC capability bits into a sample rate.
 *
 * The A2DP configuration carries the *agreed* codec settings, so exactly one
 * frequency bit is set. Anything else means a source we did not expect.
 */
static uint32_t sbc_sample_rate(const esp_a2d_mcc_t *mcc)
{
    const uint8_t freq = mcc->cie.sbc_info.samp_freq;

    if (freq & ESP_A2D_SBC_CIE_SF_48K) {
        return 48000;
    }
    if (freq & ESP_A2D_SBC_CIE_SF_44K) {
        return 44100;
    }
    if (freq & ESP_A2D_SBC_CIE_SF_32K) {
        return 32000;
    }
    if (freq & ESP_A2D_SBC_CIE_SF_16K) {
        return 16000;
    }
    ESP_LOGW(TAG, "unknown SBC sample rate bits 0x%02x, assuming 44.1 kHz", freq);
    return 44100;
}

static uint8_t sbc_channels(const esp_a2d_mcc_t *mcc)
{
    return (mcc->cie.sbc_info.ch_mode & ESP_A2D_SBC_CIE_CH_MODE_MONO) ? 1 : 2;
}

/* Runs on the application task. */
static void handle_a2dp_event(uint16_t event, void *param)
{
    esp_a2d_cb_param_t *a2d = (esp_a2d_cb_param_t *)param;
    char bda[18];

    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        const esp_a2d_connection_state_t state = a2d->conn_stat.state;
        ESP_LOGI(TAG, "link %s [%s]", s_conn_state[state],
                 bt_core_bda_str(a2d->conn_stat.remote_bda, bda, sizeof(bda)));

        if (state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            /* Stop advertising while occupied: a second source cannot be
             * served anyway, and staying discoverable wastes radio time. */
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
            ESP_ERROR_CHECK_WITHOUT_ABORT(audio_sink_start());
            puck_ui_set_state(PUCK_UI_CONNECTED);
        } else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(audio_sink_stop());
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            puck_ui_set_state(PUCK_UI_PAIRING);
        }
        break;
    }

    case ESP_A2D_AUDIO_STATE_EVT: {
        const esp_a2d_audio_state_t state = a2d->audio_stat.state;
        ESP_LOGI(TAG, "stream %s", s_audio_state[state]);
        if (state == ESP_A2D_AUDIO_STATE_STARTED) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(audio_sink_start());
            puck_ui_set_state(PUCK_UI_PLAYING);
        } else {
            puck_ui_set_state(PUCK_UI_CONNECTED);
        }
        break;
    }

    case ESP_A2D_AUDIO_CFG_EVT: {
        const esp_a2d_mcc_t *mcc = &a2d->audio_cfg.mcc;
        if (mcc->type != ESP_A2D_MCT_SBC) {
            ESP_LOGE(TAG, "unsupported codec type %d", mcc->type);
            break;
        }
        const uint32_t rate = sbc_sample_rate(mcc);
        const uint8_t channels = sbc_channels(mcc);
        ESP_LOGI(TAG, "SBC configured: %" PRIu32 " Hz, %u ch, bitpool %u-%u",
                 rate, channels, mcc->cie.sbc_info.min_bitpool, mcc->cie.sbc_info.max_bitpool);
        ESP_ERROR_CHECK_WITHOUT_ABORT(audio_sink_set_format(rate, channels));
        /* Biquad coefficients are a function of frequency over sample rate, so
         * a source that picks 48 kHz would otherwise shift the whole curve. */
        ESP_ERROR_CHECK_WITHOUT_ABORT(audio_dsp_set_sample_rate(rate));
        break;
    }

    default:
        ESP_LOGD(TAG, "unhandled A2DP event %u", event);
        break;
    }
}

/* Runs on the application task. */
static void handle_stack_up(uint16_t event, void *param)
{
    (void)event;
    (void)param;

    ESP_ERROR_CHECK(esp_bt_gap_set_device_name(CONFIG_PUCK_BT_DEVICE_NAME));

    /* AVRCP before A2DP: Bluedroid warns "A2DP Enable without AVRC" and skips
     * part of its SDP record if the remote control profiles are not up yet. */
    ESP_ERROR_CHECK(puck_avrcp_init());
    ESP_ERROR_CHECK(esp_a2d_sink_init());

    /* Announce ourselves as headphones so phones show the right icon and pick
     * sensible defaults for volume and routing. */
    esp_bt_gap_set_cod((esp_bt_cod_t){
                           .major   = ESP_BT_COD_MAJOR_DEV_AV,
                           .minor   = 0x06,  /* headphones */
                           .service = ESP_BT_COD_SRVC_AUDIO | ESP_BT_COD_SRVC_RENDERING,
                       },
                       ESP_BT_SET_COD_ALL);

    ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE));
    puck_ui_set_state(PUCK_UI_PAIRING);
    ESP_LOGI(TAG, "discoverable as \"%s\"", CONFIG_PUCK_BT_DEVICE_NAME);
}

/* Bluedroid context -- keep short. */
static void a2dp_event_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT:
    case ESP_A2D_AUDIO_STATE_EVT:
    case ESP_A2D_AUDIO_CFG_EVT:
        bt_core_dispatch(handle_a2dp_event, event, param, sizeof(*param), NULL);
        break;
    default:
        ESP_LOGD(TAG, "A2DP event %d ignored", event);
        break;
    }
}

/* Bluedroid context -- this is the hot path, so it does nothing but enqueue. */
static void a2dp_data_cb(const uint8_t *data, uint32_t len)
{
    audio_sink_write(data, len);
}

/* Bluedroid context. */
static void gap_event_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    char bda[18];

    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "paired with \"%s\"", param->auth_cmpl.device_name);
        } else {
            ESP_LOGW(TAG, "pairing failed, status %d", param->auth_cmpl.stat);
        }
        break;
    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(TAG, "link power mode %d, interval %.2f ms",
                 param->mode_chg.mode, param->mode_chg.interval * 0.625);
        break;
    case ESP_BT_GAP_ACL_DISCONN_CMPL_STAT_EVT:
        ESP_LOGI(TAG, "ACL closed with [%s], reason 0x%x",
                 bt_core_bda_str(param->acl_disconn_cmpl_stat.bda, bda, sizeof(bda)),
                 param->acl_disconn_cmpl_stat.reason);
        break;
    default:
        ESP_LOGD(TAG, "GAP event %d", event);
        break;
    }
}

static void log_boot_banner(void)
{
    esp_chip_info_t chip = {0};
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        flash_size = 0;
    }

    ESP_LOGI(TAG, "BlueAudio Puck booting (IDF %s)", esp_get_idf_version());
    ESP_LOGI(TAG, "%s rev v%u.%u, %d core(s), flash %" PRIu32 " MB%s",
             CONFIG_IDF_TARGET,
             chip.revision / 100, chip.revision % 100,
             chip.cores,
             flash_size / (1024U * 1024U),
             (chip.features & CHIP_FEATURE_EMB_FLASH) ? " (embedded)" : "");

    /* A2DP needs Bluetooth Classic (BR/EDR). The S3/C3 are BLE-only and cannot
     * run this firmware, so say so loudly rather than failing later. */
    if ((chip.features & CHIP_FEATURE_BT) == 0) {
        ESP_LOGE(TAG, "This chip has no Bluetooth Classic -- A2DP is not possible here");
    }

    ESP_LOGI(TAG, "Free heap: %" PRIu32 " bytes", esp_get_free_heap_size());
}

void app_main(void)
{
    log_boot_banner();

    /* Audio output first: the I2S channel must exist before the first A2DP
     * packet can arrive. */
    ESP_ERROR_CHECK(audio_sink_init());
    ESP_ERROR_CHECK(audio_dsp_init(44100));
    audio_sink_set_processor(audio_dsp_process);

    ESP_ERROR_CHECK(puck_ui_init());
    puck_ui_set_gesture_cb(on_gesture);

    ESP_ERROR_CHECK(bt_core_stack_init());
    ESP_ERROR_CHECK(bt_core_task_start());

    puck_avrcp_set_track_cb(on_track_change);

    ESP_ERROR_CHECK(esp_bt_gap_register_callback(gap_event_cb));
    ESP_ERROR_CHECK(esp_a2d_register_callback(a2dp_event_cb));
    ESP_ERROR_CHECK(esp_a2d_sink_register_data_callback(a2dp_data_cb));

    bt_core_dispatch(handle_stack_up, PUCK_WORK_STACK_UP, NULL, 0, NULL);
}

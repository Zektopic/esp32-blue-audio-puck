/*
 * SPDX-FileCopyrightText: 2026 Zektopic
 * SPDX-License-Identifier: MIT
 *
 * AVRCP in both roles.
 *
 * Target: the phone owns the volume slider and pushes absolute volume down to
 * us. Controller: we ask the phone for track metadata and can send transport
 * keys back up.
 *
 * All Bluedroid callbacks here do is dispatch onto the application task, so
 * nothing in this file runs in stack context except the two entry callbacks.
 */

#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#include "esp_avrc_api.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "audio_sink.h"
#include "bt_core.h"
#include "puck_avrcp.h"

static const char *TAG = "avrcp";

/* Metadata we ask the source for on every track change. */
#define METADATA_ATTRS (ESP_AVRC_MD_ATTR_TITLE | ESP_AVRC_MD_ATTR_ARTIST | ESP_AVRC_MD_ATTR_ALBUM)

enum {
    WORK_CT_EVT = 0,
    WORK_TG_EVT,
};

typedef struct {
    uint8_t                    volume;        /*!< 0..PUCK_VOLUME_MAX */
    bool                       volume_notify; /*!< source is waiting on a volume change response */
    bool                       connected;
    bool                       playing;
    esp_avrc_rn_evt_cap_mask_t peer_rn_cap;   /*!< which notifications the source supports */
    uint8_t                    tl;            /*!< rolling AVRCP transaction label */
    puck_track_info_t          track;
    puck_avrcp_track_cb_t      track_cb;
    SemaphoreHandle_t          lock;          /*!< guards volume and track */
} puck_avrcp_t;

static puck_avrcp_t s_rc;

/* AVRCP transaction labels are 4 bits and must differ between outstanding
 * commands; a rolling counter is enough for the handful we ever have in
 * flight. Only ever called from the application task. */
static uint8_t next_tl(void)
{
    s_rc.tl = (s_rc.tl + 1) & 0x0f;
    return s_rc.tl;
}

static bool peer_supports(esp_avrc_rn_event_ids_t event)
{
    return esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &s_rc.peer_rn_cap, event);
}

static void request_notification(esp_avrc_rn_event_ids_t event)
{
    if (!peer_supports(event)) {
        ESP_LOGD(TAG, "source does not support notification %d", event);
        return;
    }
    esp_err_t err = esp_avrc_ct_send_register_notification_cmd(next_tl(), event, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "registering notification %d failed: %s", event, esp_err_to_name(err));
    }
}

static void request_metadata(void)
{
    esp_err_t err = esp_avrc_ct_send_metadata_cmd(next_tl(), METADATA_ATTRS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "metadata request failed: %s", esp_err_to_name(err));
    }
}

static void store_metadata(uint8_t attr_id, const uint8_t *text, uint16_t len)
{
    char *field = NULL;
    size_t field_size = 0;

    switch (attr_id) {
    case ESP_AVRC_MD_ATTR_TITLE:
        field = s_rc.track.title;
        field_size = sizeof(s_rc.track.title);
        break;
    case ESP_AVRC_MD_ATTR_ARTIST:
        field = s_rc.track.artist;
        field_size = sizeof(s_rc.track.artist);
        break;
    case ESP_AVRC_MD_ATTR_ALBUM:
        field = s_rc.track.album;
        field_size = sizeof(s_rc.track.album);
        break;
    default:
        return;
    }

    /* attr_text is not NUL-terminated and its length comes from the remote
     * device, so clamp before copying. */
    const size_t copy = (len < field_size - 1) ? len : field_size - 1;
    xSemaphoreTake(s_rc.lock, portMAX_DELAY);
    memcpy(field, text, copy);
    field[copy] = '\0';
    xSemaphoreGive(s_rc.lock);
}

/* Runs on the application task. */
static void handle_ct_event(uint16_t event, void *param)
{
    esp_avrc_ct_cb_param_t *rc = (esp_avrc_ct_cb_param_t *)param;
    char bda[18];

    switch (event) {
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
        s_rc.connected = rc->conn_stat.connected;
        ESP_LOGI(TAG, "controller %s [%s]", s_rc.connected ? "connected" : "disconnected",
                 bt_core_bda_str(rc->conn_stat.remote_bda, bda, sizeof(bda)));
        if (s_rc.connected) {
            /* Ask what the source can notify us about before subscribing. */
            esp_avrc_ct_send_get_rn_capabilities_cmd(next_tl());
        } else {
            memset(&s_rc.track, 0, sizeof(s_rc.track));
            memset(&s_rc.peer_rn_cap, 0, sizeof(s_rc.peer_rn_cap));
            s_rc.playing = false;
        }
        break;

    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
        s_rc.peer_rn_cap.bits = rc->get_rn_caps_rsp.evt_set.bits;
        ESP_LOGI(TAG, "source notification capabilities 0x%x", s_rc.peer_rn_cap.bits);
        request_notification(ESP_AVRC_RN_TRACK_CHANGE);
        request_notification(ESP_AVRC_RN_PLAY_STATUS_CHANGE);
        request_metadata();
        break;

    case ESP_AVRC_CT_METADATA_RSP_EVT:
        if (rc->meta_rsp.attr_text != NULL) {
            store_metadata(rc->meta_rsp.attr_id, rc->meta_rsp.attr_text, rc->meta_rsp.attr_length);
            ESP_LOGI(TAG, "metadata 0x%x: %.*s", rc->meta_rsp.attr_id,
                     (int)rc->meta_rsp.attr_length, (const char *)rc->meta_rsp.attr_text);
            /* copy_metadata() allocated this; bt_core only frees the message. */
            free(rc->meta_rsp.attr_text);
            rc->meta_rsp.attr_text = NULL;
        }
        if (rc->meta_rsp.attr_id == ESP_AVRC_MD_ATTR_ALBUM && s_rc.track_cb) {
            /* Album is the last attribute of the batch, so the set is complete. */
            s_rc.track_cb(&s_rc.track);
        }
        break;

    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
        switch (rc->change_ntf.event_id) {
        case ESP_AVRC_RN_TRACK_CHANGE:
            request_metadata();
            break;
        case ESP_AVRC_RN_PLAY_STATUS_CHANGE:
            s_rc.playing = (rc->change_ntf.event_parameter.playback == ESP_AVRC_PLAYBACK_PLAYING);
            ESP_LOGI(TAG, "source is %s", s_rc.playing ? "playing" : "not playing");
            break;
        default:
            break;
        }
        /* Notifications are one-shot: re-register to keep hearing about them. */
        request_notification(rc->change_ntf.event_id);
        break;

    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
        ESP_LOGI(TAG, "source features 0x%" PRIx32, rc->rmt_feats.feat_mask);
        break;

    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
        ESP_LOGD(TAG, "key 0x%x state %d acknowledged with %d",
                 rc->psth_rsp.key_code, rc->psth_rsp.key_state, rc->psth_rsp.rsp_code);
        break;

    default:
        ESP_LOGD(TAG, "unhandled CT event %u", event);
        break;
    }
}

/* Runs on the application task. */
static void handle_tg_event(uint16_t event, void *param)
{
    esp_avrc_tg_cb_param_t *rc = (esp_avrc_tg_cb_param_t *)param;

    switch (event) {
    case ESP_AVRC_TG_CONNECTION_STATE_EVT:
        if (!rc->conn_stat.connected) {
            s_rc.volume_notify = false;
        }
        break;

    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT: {
        const uint8_t volume = rc->set_abs_vol.volume & PUCK_VOLUME_MAX;
        xSemaphoreTake(s_rc.lock, portMAX_DELAY);
        s_rc.volume = volume;
        xSemaphoreGive(s_rc.lock);
        audio_sink_set_volume(volume);
        ESP_LOGI(TAG, "source set volume to %u%%", (unsigned)volume * 100 / PUCK_VOLUME_MAX);
        break;
    }

    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
        if (rc->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
            /* The source is now waiting for us to report volume changes. Answer
             * with the current value straight away, then keep the request open. */
            s_rc.volume_notify = true;
            esp_avrc_rn_param_t rn = { .volume = s_rc.volume };
            esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_INTERIM, &rn);
        }
        break;

    case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
        ESP_LOGI(TAG, "source sent key 0x%x state %d",
                 rc->psth_cmd.key_code, rc->psth_cmd.key_state);
        break;

    default:
        ESP_LOGD(TAG, "unhandled TG event %u", event);
        break;
    }
}

/*
 * Deep copy for metadata responses: attr_text points at a buffer Bluedroid
 * owns and reuses, so the shallow struct copy alone would leave the
 * application task reading freed memory by the time it runs.
 */
static void copy_metadata(void *dst, void *src, int len)
{
    (void)len;
    esp_avrc_ct_cb_param_t *d = (esp_avrc_ct_cb_param_t *)dst;
    const esp_avrc_ct_cb_param_t *s = (const esp_avrc_ct_cb_param_t *)src;

    d->meta_rsp.attr_text = NULL;
    if (s->meta_rsp.attr_text == NULL || s->meta_rsp.attr_length == 0) {
        d->meta_rsp.attr_length = 0;
        return;
    }

    uint8_t *text = malloc(s->meta_rsp.attr_length);
    if (text == NULL) {
        d->meta_rsp.attr_length = 0;
        return;
    }
    memcpy(text, s->meta_rsp.attr_text, s->meta_rsp.attr_length);
    d->meta_rsp.attr_text = text;
}

/* Bluedroid context. */
static void ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_CT_METADATA_RSP_EVT:
        bt_core_dispatch(handle_ct_event, event, param, sizeof(*param), copy_metadata);
        break;
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
        bt_core_dispatch(handle_ct_event, event, param, sizeof(*param), NULL);
        break;
    default:
        break;
    }
}

/* Bluedroid context. */
static void tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_TG_CONNECTION_STATE_EVT:
    case ESP_AVRC_TG_REMOTE_FEATURES_EVT:
    case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
    case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT:
        bt_core_dispatch(handle_tg_event, event, param, sizeof(*param), NULL);
        break;
    default:
        break;
    }
}

esp_err_t puck_avrcp_init(void)
{
    if (s_rc.lock == NULL) {
        s_rc.lock = xSemaphoreCreateMutex();
        ESP_RETURN_ON_FALSE(s_rc.lock != NULL, ESP_ERR_NO_MEM, TAG, "mutex alloc failed");
    }

    s_rc.volume = CONFIG_PUCK_DEFAULT_VOLUME;
    audio_sink_set_volume(s_rc.volume);

    ESP_RETURN_ON_ERROR(esp_avrc_ct_init(), TAG, "AVRCP controller init failed");
    ESP_RETURN_ON_ERROR(esp_avrc_ct_register_callback(ct_cb), TAG, "AVRCP CT callback failed");

    ESP_RETURN_ON_ERROR(esp_avrc_tg_init(), TAG, "AVRCP target init failed");
    ESP_RETURN_ON_ERROR(esp_avrc_tg_register_callback(tg_cb), TAG, "AVRCP TG callback failed");

    /* Declare that we can report volume changes. Without this the phone will
     * not hand us absolute volume control and its slider does nothing. */
    esp_avrc_rn_evt_cap_mask_t cap = {0};
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cap, ESP_AVRC_RN_VOLUME_CHANGE);
    ESP_RETURN_ON_ERROR(esp_avrc_tg_set_rn_evt_cap(&cap), TAG, "declaring volume capability failed");

    ESP_LOGI(TAG, "AVRCP up, volume %u%%",
             (unsigned)s_rc.volume * 100 / PUCK_VOLUME_MAX);
    return ESP_OK;
}

uint8_t puck_avrcp_get_volume(void)
{
    return s_rc.volume;
}

void puck_avrcp_set_volume(uint8_t volume)
{
    if (volume > PUCK_VOLUME_MAX) {
        volume = PUCK_VOLUME_MAX;
    }

    xSemaphoreTake(s_rc.lock, portMAX_DELAY);
    const bool changed = (s_rc.volume != volume);
    s_rc.volume = volume;
    xSemaphoreGive(s_rc.lock);

    if (!changed) {
        return;
    }
    audio_sink_set_volume(volume);
    ESP_LOGI(TAG, "volume set locally to %u%%", (unsigned)volume * 100 / PUCK_VOLUME_MAX);

    /* Answer the source's outstanding notification request so its slider
     * follows the buttons. The request is consumed by answering it. */
    if (s_rc.volume_notify) {
        esp_avrc_rn_param_t rn = { .volume = volume };
        esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_CHANGED, &rn);
        s_rc.volume_notify = false;
    }
}

void puck_avrcp_adjust_volume(int16_t delta)
{
    int32_t next = (int32_t)s_rc.volume + delta;

    if (next < 0) {
        next = 0;
    } else if (next > PUCK_VOLUME_MAX) {
        next = PUCK_VOLUME_MAX;
    }
    puck_avrcp_set_volume((uint8_t)next);
}

esp_err_t puck_avrcp_send_key(uint8_t key_code)
{
    ESP_RETURN_ON_FALSE(s_rc.connected, ESP_ERR_INVALID_STATE, TAG, "no controller link");

    ESP_RETURN_ON_ERROR(esp_avrc_ct_send_passthrough_cmd(next_tl(), key_code,
                                                         ESP_AVRC_PT_CMD_STATE_PRESSED),
                        TAG, "key press failed");
    ESP_RETURN_ON_ERROR(esp_avrc_ct_send_passthrough_cmd(next_tl(), key_code,
                                                         ESP_AVRC_PT_CMD_STATE_RELEASED),
                        TAG, "key release failed");
    return ESP_OK;
}

void puck_avrcp_set_track_cb(puck_avrcp_track_cb_t cb)
{
    s_rc.track_cb = cb;
}

void puck_avrcp_get_track(puck_track_info_t *out)
{
    if (out == NULL) {
        return;
    }
    xSemaphoreTake(s_rc.lock, portMAX_DELAY);
    *out = s_rc.track;
    xSemaphoreGive(s_rc.lock);
}

bool puck_avrcp_is_playing(void)
{
    return s_rc.playing;
}

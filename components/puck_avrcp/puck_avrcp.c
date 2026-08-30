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

/* Longest metadata string kept, matching the fields in puck_track_info_t. */
#define METADATA_MAX_BYTES 63

enum {
    WORK_CT_EVT = 0,
    WORK_TG_EVT,
};

typedef struct {
    uint8_t                    volume;        /*!< 0..PUCK_VOLUME_MAX */
    esp_bd_addr_t              tg_peer;       /*!< who holds the AVRCP target link */
    esp_bd_addr_t              audio_peer;    /*!< who holds the A2DP stream */
    bool                       tg_peer_valid;
    bool                       audio_peer_valid;
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

/*
 * AVRCP transaction labels are 4 bits and must differ between outstanding
 * commands.
 *
 * The increment is atomic because this is not single-threaded: event handlers
 * call it on the application task, while puck_avrcp_send_key() is a public
 * entry point the UI task calls directly. A torn read-modify-write hands two
 * outstanding commands the same label, which sources answer wrongly or ignore.
 */
static uint8_t next_tl(void)
{
    return (uint8_t)(__atomic_add_fetch(&s_rc.tl, 1, __ATOMIC_RELAXED) & 0x0f);
}

/*
 * Print a stored metadata field with anything non-printable removed.
 *
 * These bytes are chosen by the remote device. Echoing them raw lets a source
 * inject ANSI escapes into the console of whoever is debugging the puck, and
 * spam long lines to bury everything else in the log.
 */
static void log_sanitised(const char *what, uint8_t attr_id)
{
    const char *src = NULL;

    switch (attr_id) {
    case ESP_AVRC_MD_ATTR_TITLE:  src = s_rc.track.title;  break;
    case ESP_AVRC_MD_ATTR_ARTIST: src = s_rc.track.artist; break;
    case ESP_AVRC_MD_ATTR_ALBUM:  src = s_rc.track.album;  break;
    default: return;
    }

    char safe[METADATA_MAX_BYTES + 1];
    size_t i = 0;
    for (; src[i] != '\0' && i < sizeof(safe) - 1; i++) {
        const unsigned char c = (unsigned char)src[i];
        safe[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
    }
    safe[i] = '\0';
    ESP_LOGI(TAG, "%s 0x%x: %s", what, attr_id, safe);
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
            log_sanitised("metadata", rc->meta_rsp.attr_id);
        }
        /* free_metadata() releases attr_text once this returns. */
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
        if (rc->conn_stat.connected) {
            memcpy(s_rc.tg_peer, rc->conn_stat.remote_bda, sizeof(s_rc.tg_peer));
            s_rc.tg_peer_valid = true;
        } else {
            s_rc.volume_notify = false;
            s_rc.tg_peer_valid = false;
        }
        break;

    case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT: {
        /* Only the device actually streaming gets to move the volume. The
         * command itself carries no address, so the target link's peer is
         * compared against the A2DP peer recorded on connection. */
        if (s_rc.audio_peer_valid && s_rc.tg_peer_valid &&
            memcmp(s_rc.audio_peer, s_rc.tg_peer, sizeof(s_rc.audio_peer)) != 0) {
            ESP_LOGW(TAG, "ignoring volume command from a device that is not the source");
            break;
        }
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

    /* Clamp to what store_metadata() can keep. The length is chosen by the
     * remote device, and copying all of it would let a source dictate the size
     * of a heap allocation on a device with ~100 kB of free heap. */
    const uint16_t wanted = s->meta_rsp.attr_length;
    const uint16_t kept = (wanted > METADATA_MAX_BYTES) ? METADATA_MAX_BYTES : wanted;

    uint8_t *text = malloc(kept);
    if (text == NULL) {
        d->meta_rsp.attr_length = 0;
        return;
    }
    memcpy(text, s->meta_rsp.attr_text, kept);
    d->meta_rsp.attr_text = text;
    d->meta_rsp.attr_length = kept;
}

/*
 * Release what copy_metadata() allocated.
 *
 * bt_core runs this on every disposal path, including the ones that never
 * reach a handler -- a full work queue, or shutdown. Freeing only in the
 * handler leaked on exactly the paths a flood of metadata responses drives.
 */
static void free_metadata(void *param)
{
    esp_avrc_ct_cb_param_t *p = (esp_avrc_ct_cb_param_t *)param;

    free(p->meta_rsp.attr_text);
    p->meta_rsp.attr_text = NULL;
}

/* Bluedroid context. */
static void ct_cb(esp_avrc_ct_cb_event_t event, esp_avrc_ct_cb_param_t *param)
{
    switch (event) {
    case ESP_AVRC_CT_METADATA_RSP_EVT:
        if (!bt_core_dispatch(handle_ct_event, event, param, sizeof(*param),
                              copy_metadata, free_metadata)) {
            /* Dropped. Metadata is cosmetic, so losing one is survivable --
             * the next track change requests the whole set again. */
            ESP_LOGD(TAG, "metadata event dropped");
        }
        break;
    case ESP_AVRC_CT_CONNECTION_STATE_EVT:
    case ESP_AVRC_CT_PASSTHROUGH_RSP_EVT:
    case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
    case ESP_AVRC_CT_REMOTE_FEATURES_EVT:
    case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
        bt_core_dispatch(handle_ct_event, event, param, sizeof(*param), NULL, NULL);
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
        bt_core_dispatch(handle_tg_event, event, param, sizeof(*param), NULL, NULL);
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
    if (s_rc.lock == NULL) {
        ESP_LOGW(TAG, "volume change before AVRCP is up, ignored");
        return;
    }
    if (volume > PUCK_VOLUME_MAX) {
        volume = PUCK_VOLUME_MAX;
    }

    /* Test-and-clear the notification flag inside the lock along with the
     * volume. Outside it, a concurrent REGISTER_NOTIFICATION could see the flag
     * set twice and answer one outstanding request twice, or lose it entirely
     * and leave the phone's slider stuck. */
    xSemaphoreTake(s_rc.lock, portMAX_DELAY);
    const bool changed = (s_rc.volume != volume);
    s_rc.volume = volume;
    const bool answer_now = changed && s_rc.volume_notify;
    if (answer_now) {
        s_rc.volume_notify = false;
    }
    xSemaphoreGive(s_rc.lock);

    if (!changed) {
        return;
    }
    audio_sink_set_volume(volume);
    ESP_LOGI(TAG, "volume set locally to %u%%", (unsigned)volume * 100 / PUCK_VOLUME_MAX);

    /* Sent outside the lock: it calls into Bluedroid, which may block. */
    if (answer_now) {
        esp_avrc_rn_param_t rn = { .volume = volume };
        esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE, ESP_AVRC_RN_RSP_CHANGED, &rn);
    }
}

void puck_avrcp_set_audio_peer(const uint8_t *bda)
{
    if (bda == NULL) {
        s_rc.audio_peer_valid = false;
        return;
    }
    memcpy(s_rc.audio_peer, bda, sizeof(s_rc.audio_peer));
    s_rc.audio_peer_valid = true;
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
    if (out == NULL || s_rc.lock == NULL) {
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

/**
 * @file ble_gatt.c
 * @brief Connectable BLE GATT service for AirCube (see ble_gatt.h)
 *
 * Protocol spec: docs/BLE_GATT_PROTOCOL.md
 *
 * Layout:
 *   Service  A17C0DE0-1D0F-4E7C-8E4B-2A3D5F6B7C80
 *     Device Info      A17C0DE1  (read)          14 bytes
 *     Live Data        A17C0DE2  (read/notify)   20 bytes
 *     History Request  A17C0DE3  (write)          4 bytes
 *     History Data     A17C0DE4  (notify)        4 + n*32 bytes
 *     Brightness       A17C0DE5  (read/write/notify)  1 byte (percent 0-100)
 *
 * Advertising keeps the BTHome v2 service-data payload from ble_bthome.c
 * (Home Assistant proxies keep decoding live values) but is *connectable*;
 * the scan response carries the device name and the 128-bit service UUID
 * so phone apps can filter scans.
 *
 * @author StuckAtPrototype, LLC
 */

#include "ble_gatt.h"
#include "history.h"
#include "device_model.h"
#include "button.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "ble_gatt";

#define DEVICE_NAME "AirCube"

/* ── UUIDs (little-endian byte order for BLE_UUID128_INIT) ───────────────
 * Full form: A17C0DEx-1D0F-4E7C-8E4B-2A3D5F6B7C80 */
#define AIRCUBE_UUID128(x) BLE_UUID128_INIT(                     \
    0x80, 0x7C, 0x6B, 0x5F, 0x3D, 0x2A, 0x4B, 0x8E,              \
    0x7C, 0x4E, 0x0F, 0x1D, (x), 0x0D, 0x7C, 0xA1)

static const ble_uuid128_t UUID_SVC       = AIRCUBE_UUID128(0xE0);
static const ble_uuid128_t UUID_DEV_INFO  = AIRCUBE_UUID128(0xE1);
static const ble_uuid128_t UUID_LIVE      = AIRCUBE_UUID128(0xE2);
static const ble_uuid128_t UUID_HIST_REQ  = AIRCUBE_UUID128(0xE3);
static const ble_uuid128_t UUID_HIST_DATA = AIRCUBE_UUID128(0xE4);
static const ble_uuid128_t UUID_BRIGHT    = AIRCUBE_UUID128(0xE5);

/* ── Protocol constants ──────────────────────────────────────────────── */
#define PROTOCOL_VERSION      1

#define HIST_OP_START         0x01
#define HIST_OP_ABORT         0x02

#define FRAME_DATA            0x01
#define FRAME_DONE            0x02
#define FRAME_ERROR           0x03

#define ERR_BUSY              1
#define ERR_BAD_REQUEST       2

#define FRAME_HEADER_LEN      4
#define SEQ_NONE              0xFFFF

/* Retry cadence when the NimBLE mbuf pool is exhausted (backpressure) */
#define NOTIFY_RETRY_DELAY_MS 10
#define NOTIFY_RETRY_MAX      500   /* * 10 ms = give up after 5 s stalled */

/* ── Packed wire structs ─────────────────────────────────────────────── */

typedef struct __attribute__((packed)) {
    uint8_t  protocol_version;
    uint8_t  model;             /* 0 = Base, 1 = Pro */
    uint8_t  fw_major;
    uint8_t  fw_minor;
    uint8_t  fw_patch;
    uint8_t  reserved;
    uint16_t history_capacity;
    uint16_t history_entry_count;
    uint16_t history_window_s;
    uint16_t newest_seq;
} dev_info_t;
_Static_assert(sizeof(dev_info_t) == 14, "dev_info_t must be 14 bytes");

typedef struct __attribute__((packed)) {
    int16_t  temp_x100;
    uint16_t hum_x100;
    uint16_t voc_level;
    uint16_t eco2_ppm;
    uint16_t etvoc_ppb;
    uint16_t co2_ppm;
    uint16_t lux_x10;
    uint8_t  aqi_uba;
    uint8_t  flags;             /* bit0 = Pro */
    uint32_t uptime_ms;
} live_data_t;
_Static_assert(sizeof(live_data_t) == 20, "live_data_t must be 20 bytes");

/* ── State ───────────────────────────────────────────────────────────── */

static bool              s_initialized     = false;
static volatile bool     s_ble_ready       = false;
static volatile uint16_t s_conn_handle     = BLE_HS_CONN_HANDLE_NONE;
static volatile bool     s_live_notify     = false;
static volatile bool     s_hist_notify     = false;
static volatile bool     s_stream_abort    = false;

static uint16_t s_live_val_handle;
static uint16_t s_hist_data_val_handle;
static uint16_t s_bright_val_handle;
static volatile bool s_bright_notify = false;

static live_data_t s_live;                 /* latest readings (little-endian native) */

/* BTHome advertising payload inputs */
static volatile int16_t  s_adv_temp_x100 = 2000;
static volatile uint16_t s_adv_hum_x100  = 5000;
static volatile uint16_t s_adv_co2      = 0;
static volatile uint16_t s_adv_tvoc     = 0;

/* History stream request queue (length 1: one stream at a time) */
static QueueHandle_t s_stream_queue = NULL;

/* One-shot timer: Service Changed indication after connect (see below) */
static esp_timer_handle_t s_svc_changed_timer = NULL;

/* ── Firmware version ────────────────────────────────────────────────── */

static void get_fw_version(uint8_t *major, uint8_t *minor, uint8_t *patch)
{
    unsigned a = 0, b = 0, c = 0;
    sscanf(esp_app_get_description()->version, "%u.%u.%u", &a, &b, &c);
    *major = (uint8_t)a;
    *minor = (uint8_t)b;
    *patch = (uint8_t)c;
}

/* ── Advertising ─────────────────────────────────────────────────────── */

/* BTHome v2 constants (same as ble_bthome.c) */
#define BTHOME_DEVICE_INFO      0x40
#define BTHOME_OBJ_TEMPERATURE  0x02
#define BTHOME_OBJ_HUMIDITY     0x03
#define BTHOME_OBJ_CO2          0x12
#define BTHOME_OBJ_TVOC         0x13

static int gap_event_cb(struct ble_gap_event *event, void *arg);

/* iOS caches a peripheral's GATT database across connections, so when a
 * firmware update adds characteristics an iPhone keeps using the stale
 * table (the client never "sees" new characteristics until Bluetooth is
 * toggled). We are not bonded, so we can't track per-client state; instead
 * indicate Service Changed shortly after every connect - the delay gives
 * the central time to subscribe to the Service Changed characteristic.
 * Cost: the phone re-discovers our handful of characteristics (~100 ms). */
static void svc_changed_timer_cb(void *arg)
{
    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        ble_svc_gatt_changed(0x0001, 0xFFFF);
    }
}

/** Set advertisement + scan-response data and start connectable advertising. */
static void do_advertise(void)
{
    /* ── Advertisement: flags + BTHome v2 service data ── */
    uint8_t adv[31];
    int pos = 0;

    adv[pos++] = 2;
    adv[pos++] = 0x01;
    adv[pos++] = 0x06;      /* LE General Discoverable | BR/EDR Not Supported */

    uint8_t svc[20];
    int sp = 0;
    svc[sp++] = 0xD2;       /* BTHome UUID 0xFCD2, little-endian */
    svc[sp++] = 0xFC;
    svc[sp++] = BTHOME_DEVICE_INFO;

    int16_t t = s_adv_temp_x100;
    svc[sp++] = BTHOME_OBJ_TEMPERATURE;
    svc[sp++] = (uint8_t)((uint16_t)t & 0xFF);
    svc[sp++] = (uint8_t)((uint16_t)t >> 8);

    svc[sp++] = BTHOME_OBJ_HUMIDITY;
    svc[sp++] = (uint8_t)(s_adv_hum_x100 & 0xFF);
    svc[sp++] = (uint8_t)(s_adv_hum_x100 >> 8);

    svc[sp++] = BTHOME_OBJ_CO2;
    svc[sp++] = (uint8_t)(s_adv_co2 & 0xFF);
    svc[sp++] = (uint8_t)(s_adv_co2 >> 8);

    svc[sp++] = BTHOME_OBJ_TVOC;
    svc[sp++] = (uint8_t)(s_adv_tvoc & 0xFF);
    svc[sp++] = (uint8_t)(s_adv_tvoc >> 8);

    adv[pos++] = sp + 1;
    adv[pos++] = 0x16;      /* Service Data - 16-bit UUID */
    memcpy(adv + pos, svc, sp);
    pos += sp;

    int rc = ble_gap_adv_set_data(adv, pos);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_data failed: %d", rc);
        return;
    }

    /* ── Scan response: name + 128-bit service UUID (9 + 18 = 27 bytes) ── */
    uint8_t rsp[31];
    int rp = 0;

    uint8_t nlen = (uint8_t)strlen(DEVICE_NAME);
    rsp[rp++] = nlen + 1;
    rsp[rp++] = 0x09;       /* Complete Local Name */
    memcpy(rsp + rp, DEVICE_NAME, nlen);
    rp += nlen;

    rsp[rp++] = 17;
    rsp[rp++] = 0x07;       /* Complete List of 128-bit Service UUIDs */
    memcpy(rsp + rp, UUID_SVC.value, 16);
    rp += 16;

    ble_gap_adv_rsp_set_data(rsp, rp);

    /* ── Connectable advertising, GAP events -> gap_event_cb ── */
    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min  = BLE_GAP_ADV_ITVL_MS(200);
    params.itvl_max  = BLE_GAP_ADV_ITVL_MS(400);

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &params, gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv_start failed: %d", rc);
    }
}

static void refresh_advertising(void)
{
    if (!s_ble_ready || s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        return;     /* not synced yet, or connected (adv stopped) */
    }
    if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
    }
    do_advertise();
}

/* ── GAP events ──────────────────────────────────────────────────────── */

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "Central connected (handle %u)", s_conn_handle);
            if (s_svc_changed_timer != NULL) {
                esp_timer_stop(s_svc_changed_timer);
                esp_timer_start_once(s_svc_changed_timer, 1500 * 1000);
            }
        } else {
            ESP_LOGW(TAG, "Connection failed (status %d)", event->connect.status);
            do_advertise();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Central disconnected (reason %d)", event->disconnect.reason);
        s_conn_handle   = BLE_HS_CONN_HANDLE_NONE;
        s_live_notify   = false;
        s_hist_notify   = false;
        s_bright_notify = false;
        s_stream_abort  = true;     /* stop any running history stream */
        do_advertise();
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_live_val_handle) {
            s_live_notify = event->subscribe.cur_notify;
        } else if (event->subscribe.attr_handle == s_hist_data_val_handle) {
            s_hist_notify = event->subscribe.cur_notify;
            if (!s_hist_notify) {
                s_stream_abort = true;
            }
        } else if (event->subscribe.attr_handle == s_bright_val_handle) {
            s_bright_notify = event->subscribe.cur_notify;
        }
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU negotiated: %u", event->mtu.value);
        break;

    default:
        break;
    }
    return 0;
}

/* ── History streaming ───────────────────────────────────────────────── */

/** Send one notification, retrying on mbuf exhaustion (backpressure). */
static bool notify_with_retry(const uint8_t *buf, uint16_t len)
{
    for (int attempt = 0; attempt < NOTIFY_RETRY_MAX; attempt++) {
        if (s_stream_abort || s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
            return false;
        }
        struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, len);
        if (om != NULL) {
            int rc = ble_gatts_notify_custom(s_conn_handle, s_hist_data_val_handle, om);
            if (rc == 0) {
                return true;
            }
            /* notify_custom consumed the mbuf even on failure */
            if (rc != BLE_HS_ENOMEM && rc != BLE_HS_EAGAIN) {
                ESP_LOGW(TAG, "notify failed: %d", rc);
                return false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(NOTIFY_RETRY_DELAY_MS));
    }
    ESP_LOGW(TAG, "notify stalled - aborting stream");
    return false;
}

static void send_control_frame(uint8_t type, uint16_t value)
{
    uint8_t frame[FRAME_HEADER_LEN] = {
        type, 0, (uint8_t)(value & 0xFF), (uint8_t)(value >> 8)
    };
    notify_with_retry(frame, sizeof(frame));
}

/** Map after_seq to the first logical index to stream. */
static uint16_t stream_start_index(uint16_t after_seq, uint16_t count)
{
    if (after_seq == SEQ_NONE || count == 0) {
        return 0;   /* full history */
    }

    history_slot_t newest;
    if (history_read_slot(count - 1, &newest) != ESP_OK) {
        return 0;
    }

    /* Monotonic u16 sequences; diff >= 0x8000 means after_seq is ahead of us
     * (client from a cleared/other timeline) -> full resync. The 0xFFFF-skip
     * on wraparound (every ~7.5 years of slots) can make this off by one
     * slot; harmless since clients dedup by sequence. */
    uint16_t diff = (uint16_t)(newest.sequence - after_seq);
    if (diff == 0) {
        return count;           /* nothing new */
    }
    if (diff >= 0x8000 || diff >= count) {
        return 0;               /* unknown reference -> full history */
    }
    return count - diff;
}

static void history_stream_task(void *arg)
{
    uint16_t after_seq;

    for (;;) {
        if (xQueueReceive(s_stream_queue, &after_seq, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (!history_stream_acquire()) {
            /* Serial transfer in flight - tell the client to retry */
            send_control_frame(FRAME_ERROR, ERR_BUSY);
            continue;
        }

        s_stream_abort = false;

        uint16_t count;
        history_get_info(NULL, &count);
        uint16_t idx = stream_start_index(after_seq, count);

        /* Batch size from the negotiated MTU: payload = MTU - 3 (ATT header) */
        uint16_t mtu = ble_att_mtu(s_conn_handle);
        if (mtu < 23 + FRAME_HEADER_LEN + HISTORY_SLOT_SIZE) {
            mtu = 23 + FRAME_HEADER_LEN + HISTORY_SLOT_SIZE;  /* always fit 1 slot */
        }
        uint16_t slots_per_frame = (mtu - 3 - FRAME_HEADER_LEN) / HISTORY_SLOT_SIZE;

        ESP_LOGI(TAG, "History stream: after_seq=%u count=%u start_idx=%u (%u slots/frame)",
                 after_seq, count, idx, slots_per_frame);

        uint8_t frame[FRAME_HEADER_LEN + 8 * HISTORY_SLOT_SIZE];
        if (slots_per_frame > 8) {
            slots_per_frame = 8;    /* frame buffer bound (8*32 = 256 > any MTU we allow) */
        }

        uint16_t total_sent = 0;
        bool ok = true;

        while (idx < count && ok && !s_stream_abort) {
            uint8_t n = 0;
            uint16_t first_idx = idx;

            while (n < slots_per_frame && idx < count) {
                history_slot_t slot;
                esp_err_t err = history_read_slot(idx, &slot);
                idx++;
                if (err == ESP_OK) {
                    memcpy(frame + FRAME_HEADER_LEN + (size_t)n * HISTORY_SLOT_SIZE,
                           &slot, HISTORY_SLOT_SIZE);
                    n++;
                } else if (err == ESP_ERR_INVALID_ARG) {
                    /* count shrank under us (clear is blocked, but be safe) */
                    idx = count;
                    break;
                }
                /* ESP_ERR_NOT_FOUND (freshly erased slot) -> just skip it */
            }

            if (n > 0) {
                frame[0] = FRAME_DATA;
                frame[1] = n;
                frame[2] = (uint8_t)(first_idx & 0xFF);
                frame[3] = (uint8_t)(first_idx >> 8);
                ok = notify_with_retry(frame, FRAME_HEADER_LEN + (uint16_t)n * HISTORY_SLOT_SIZE);
                if (ok) {
                    total_sent += n;
                }
            }
        }

        if (ok && !s_stream_abort) {
            send_control_frame(FRAME_DONE, total_sent);
            ESP_LOGI(TAG, "History stream complete: %u slots", total_sent);
        } else {
            ESP_LOGW(TAG, "History stream aborted after %u slots", total_sent);
        }

        history_stream_release();
    }
}

/* ── GATT access callbacks ───────────────────────────────────────────── */

static int dev_info_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    dev_info_t info = {0};
    info.protocol_version = PROTOCOL_VERSION;
    info.model = aircube_model_is_pro() ? 1 : 0;
    get_fw_version(&info.fw_major, &info.fw_minor, &info.fw_patch);
    info.history_capacity = HISTORY_MAX_VALID_ENTRIES;
    info.history_window_s = (uint16_t)(HISTORY_WINDOW_US / 1000000ULL);

    uint16_t count;
    history_get_info(NULL, &count);
    info.history_entry_count = count;

    info.newest_seq = SEQ_NONE;
    if (count > 0) {
        history_slot_t newest;
        if (history_read_slot(count - 1, &newest) == ESP_OK) {
            info.newest_seq = newest.sequence;
        }
    }

    return os_mbuf_append(ctxt->om, &info, sizeof(info)) == 0
               ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int live_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return os_mbuf_append(ctxt->om, &s_live, sizeof(s_live)) == 0
               ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int hist_req_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    uint8_t req[4];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, req, sizeof(req), &len) != 0 || len < 4) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint8_t  opcode    = req[0];
    uint16_t after_seq = (uint16_t)(req[2] | (req[3] << 8));

    if (opcode == HIST_OP_ABORT) {
        s_stream_abort = true;
        return 0;
    }
    if (opcode != HIST_OP_START) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (!s_hist_notify) {
        /* Client must subscribe to History Data first */
        send_control_frame(FRAME_ERROR, ERR_BAD_REQUEST);
        return 0;
    }

    if (xQueueSend(s_stream_queue, &after_seq, 0) != pdTRUE) {
        /* A stream is already queued/running */
        send_control_frame(FRAME_ERROR, ERR_BUSY);
    }
    return 0;
}

static int hist_data_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    /* Notify-only characteristic; reads return nothing useful */
    return 0;
}

static int brightness_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        uint8_t pct = (uint8_t)button_get_brightness_percent();
        return os_mbuf_append(ctxt->om, &pct, 1) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t pct;
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, &pct, 1, &len) != 0 || len < 1) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        if (pct > 100) {
            pct = 100;
        }
        button_set_brightness_percent(pct);
        ESP_LOGI(TAG, "Brightness set to %u%% via BLE", pct);
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

/* ── GATT service table ──────────────────────────────────────────────── */

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &UUID_SVC.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid      = &UUID_DEV_INFO.u,
                .access_cb = dev_info_access_cb,
                .flags     = BLE_GATT_CHR_F_READ,
            },
            {
                .uuid       = &UUID_LIVE.u,
                .access_cb  = live_access_cb,
                .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_live_val_handle,
            },
            {
                .uuid      = &UUID_HIST_REQ.u,
                .access_cb = hist_req_access_cb,
                .flags     = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid       = &UUID_HIST_DATA.u,
                .access_cb  = hist_data_access_cb,
                .flags      = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_hist_data_val_handle,
            },
            {
                .uuid       = &UUID_BRIGHT.u,
                .access_cb  = brightness_access_cb,
                .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                              BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_bright_val_handle,
            },
            { 0 }   /* terminator */
        },
    },
    { 0 }   /* terminator */
};

/* ── NimBLE host lifecycle ───────────────────────────────────────────── */

static void on_sync(void)
{
    s_ble_ready = true;
    ESP_LOGI(TAG, "BLE stack ready - starting connectable advertising");
    do_advertise();
}

static void on_reset(int reason)
{
    s_ble_ready   = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    ESP_LOGW(TAG, "BLE host reset (reason %d)", reason);
}

static void ble_host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* ── Public API ──────────────────────────────────────────────────────── */

void ble_gatt_init(void)
{
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(ret));
        return;
    }

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_svc_gap_device_name_set(DEVICE_NAME);

    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(gatt_svcs);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT service registration failed: %d", rc);
        return;
    }

    s_stream_queue = xQueueCreate(1, sizeof(uint16_t));
    xTaskCreate(history_stream_task, "ble_hist_stream", 4096, NULL, 4, NULL);

    const esp_timer_create_args_t timer_args = {
        .callback = svc_changed_timer_cb,
        .name = "ble_svc_chg",
    };
    esp_timer_create(&timer_args, &s_svc_changed_timer);

    nimble_port_freertos_init(ble_host_task);

    s_initialized = true;
    ESP_LOGI(TAG, "BLE GATT service initialized");
}

void ble_gatt_update_live(float temp_c, float humidity, int aqi, int eco2,
                          int etvoc, uint16_t co2_ppm, float lux, int aqi_uba)
{
    if (!s_initialized) {
        return;
    }

    s_live.temp_x100 = (int16_t)(temp_c * 100.0f);
    s_live.hum_x100  = (uint16_t)(humidity * 100.0f);
    s_live.voc_level = (uint16_t)(aqi < 0 ? 0 : (aqi > 65535 ? 65535 : aqi));
    s_live.eco2_ppm  = (uint16_t)(eco2 < 0 ? 0 : (eco2 > 65535 ? 65535 : eco2));
    s_live.etvoc_ppb = (uint16_t)(etvoc < 0 ? 0 : (etvoc > 65535 ? 65535 : etvoc));
    s_live.co2_ppm   = co2_ppm;
    s_live.lux_x10   = (uint16_t)(lux < 0 ? 0 : (lux * 10.0f > 65535.0f ? 65535 : lux * 10.0f));
    s_live.aqi_uba   = (uint8_t)(aqi_uba < 0 ? 0 : (aqi_uba > 255 ? 255 : aqi_uba));
    s_live.flags     = aircube_model_is_pro() ? 0x01 : 0x00;
    s_live.uptime_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    /* BTHome payload: prefer true CO2 on Pro, eCO2 estimate on Base */
    s_adv_temp_x100 = s_live.temp_x100;
    s_adv_hum_x100  = s_live.hum_x100;
    s_adv_co2       = (co2_ppm != 0) ? co2_ppm : s_live.eco2_ppm;
    s_adv_tvoc      = s_live.etvoc_ppb;

    if (!s_ble_ready) {
        return;
    }

    if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
        if (s_live_notify) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(&s_live, sizeof(s_live));
            if (om != NULL) {
                ble_gatts_notify_custom(s_conn_handle, s_live_val_handle, om);
            }
        }
    } else {
        /* Refresh the BTHome advertisement payload at most every 10 s
         * (sensor updates arrive ~1/s; restarting adv that often is noisy
         * and pointless for HA proxies). */
        static TickType_t last_adv_refresh = 0;
        TickType_t now = xTaskGetTickCount();
        if ((now - last_adv_refresh) >= pdMS_TO_TICKS(10000)) {
            last_adv_refresh = now;
            refresh_advertising();
        }
    }
}

bool ble_gatt_is_connected(void)
{
    return s_conn_handle != BLE_HS_CONN_HANDLE_NONE;
}

void ble_gatt_report_brightness(void)
{
    if (!s_initialized || !s_bright_notify ||
        s_conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    uint8_t pct = (uint8_t)button_get_brightness_percent();
    struct os_mbuf *om = ble_hs_mbuf_from_flat(&pct, 1);
    if (om != NULL) {
        ble_gatts_notify_custom(s_conn_handle, s_bright_val_handle, om);
    }
}

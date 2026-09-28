#include "main.h"
#include "collection.h"
#include "esp_partition.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdlib.h>

#define COL_MAGIC 0x314c4f43U
#define COL_NVS_MAGIC 0x31474643U
#define COL_SECTOR 4096U
#define COL_RECORD 32U
#define COL_SESSIONS 128U
#define COL_SAMPLE 0U
#define COL_START 1U
#define COL_STOP 2U
#define COL_DELETE 3U
#define COL_CONFIG_FLAG 0x20U
#define COL_RESUME_FLAG 0x40U

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t log_sequence;
    uint32_t session_id;
    uint32_t utc_s;
    uint32_t elapsed_ds;
    uint32_t value_raw;
    uint8_t kind_flags;
    uint8_t function;
    uint8_t unit;
    uint8_t range;
    uint32_t crc;
} col_record_t;
_Static_assert(sizeof(col_record_t) == COL_RECORD, "Collection record must be 32 bytes");

typedef struct {
    uint32_t magic;
    uint32_t session_id;
    uint32_t interval_ms;
    uint32_t target;
    uint32_t elapsed_ds;
    uint8_t limit_kind;
    uint8_t function;
    uint8_t display_unit;
    uint8_t active;
    uint8_t alerted_full;
    uint8_t alerted_done;
    uint8_t reserved[2];
    uint32_t pending_done_id;
    uint32_t pending_full_id;
    uint32_t crc;
} col_config_t;

typedef struct {
    uint32_t id;
    uint32_t count;
    uint32_t first_seq;
    uint32_t last_seq;
    uint32_t first_utc_s;
    uint32_t last_utc_s;
    uint32_t elapsed_ds;
    uint32_t interval_ms;
    uint32_t target;
    uint8_t function;
    uint8_t unit;
    uint8_t limit_kind;
    bool settings_known;
    bool stopped;
    uint32_t start_utc_s;
    char name[32];
} col_summary_t;

static const char *TAG = "COLLECTION";
static const esp_partition_t *col_part;
static SemaphoreHandle_t col_lock;
static col_config_t col_cfg;
static col_summary_t sessions[COL_SESSIONS];
static size_t session_count;
static uint32_t col_slots;
static uint32_t write_slot;
static uint32_t next_log_sequence = 1;
static uint32_t highest_session_id;
static uint32_t last_snapshot_sequence;
static uint64_t started_ms;
static uint64_t next_due_ms;
static volatile bool col_active;
static volatile bool time_synced;
static volatile bool export_active;
static QueueHandle_t send_queue;
static bool telegram_started;
static volatile uint8_t send_state;
static volatile uint32_t send_session_id;
static char ntp_server[64] = "time.nist.gov";
static char timezone_name[40] = "America/Chicago";
static char telegram_token[96];
static char telegram_chat[40];
static uint8_t scan_buffer[COL_SECTOR];
static bool storage_ready;
static void scan_storage(void);

static const char *function_name(uint8_t function)
{
    switch (function) {
        case 1: case 2: return "Voltage";
        case 3: case 4: case 5: case 6: return "Current";
        case 7: return "Resistance";
        default: return "Unknown";
    }
}

static const char *range_name(uint8_t range)
{
    switch (range) {
        case ASW_DCV1: return "1000 V";
        case ASW_DCV2: return "100 V";
        case ASW_DCV3: return "10 V";
        case ASW_ACV1: return "1000 V AC";
        case ASW_ACV2: return "100 V AC";
        case ASW_ACV3: return "10 V AC";
        case ASW_DCMA: return "250 mA";
        case ASW_DCA: return "2.5 A";
        case ASW_ACMA: return "250 mA AC";
        case ASW_ACA: return "2.5 A AC";
        case ASW_R2: return "1 MOhm";
        case ASW_R3: return "100 kOhm";
        case ASW_R4: return "10 kOhm";
        case ASW_R5: return "1 kOhm";
        default: return "Auto";
    }
}

static const char *standard_unit(uint8_t function)
{
    if (function == 1 || function == 2) return "V";
    if (function >= 3 && function <= 6) return "A";
    if (function == 7) return "Ohm";
    return "";
}

static double standard_value(const col_record_t *record)
{
    uint8_t unit = record->unit & 0x0f;
    double value = (double)record->value_raw;
    if (unit == 0x01) value *= 1e-6;
    else if (unit == 0x02) value *= 1e-3;
    else if (unit == 0x05) value *= 1e-6;
    else if (unit == 0x09) value *= 1e-3;
    if (record->kind_flags & 4U) value = -value;
    return value;
}

static void json_escape_local(const char *src, char *dst, size_t cap)
{
    size_t used = 0;
    while (*src && used + 1 < cap) {
        char c = *src++;
        if ((c == '"' || c == '\\') && used + 2 < cap) dst[used++] = '\\';
        if ((unsigned char)c < 0x20) c = ' ';
        dst[used++] = c;
    }
    dst[used] = 0;
}

static void csv_filename(uint32_t session_id, const char *name, char *out, size_t cap)
{
    size_t used = 0;
    if (cap == 0) return;
    for (size_t i = 0; name && name[i] && used + 5 < cap; ++i) {
        unsigned char c = (unsigned char)name[i];
        bool safe = c >= 0x21 && c <= 0x7e && c != '"' && c != '\\' &&
                    c != '/' && c != ':' && c != '*' && c != '?' &&
                    c != '<' && c != '>' && c != '|' && c != ';';
        if (safe || c == ' ') {
            out[used++] = (char)c;
        } else if (used > 0 && out[used - 1] != '_') {
            out[used++] = '_';
        }
    }
    while (used > 0 && (out[used - 1] == '_' || out[used - 1] == '.' || out[used - 1] == ' ')) used--;
    if (used == 0) {
        snprintf(out, cap, "collection-%" PRIu32 ".csv", session_id);
        return;
    }
    snprintf(out + used, cap - used, ".csv");
}

static void session_name_key(uint32_t id, char *key, size_t cap)
{
    snprintf(key, cap, "name_%08" PRIx32, id);
}

static void load_session_name(col_summary_t *summary)
{
    if (!summary || summary->name[0]) return;
    snprintf(summary->name, sizeof(summary->name), "Collection #%" PRIu32, summary->id);
    nvs_handle_t nvs;
    char key[16], value[32];
    session_name_key(summary->id, key, sizeof(key));
    if (nvs_open("collection", NVS_READONLY, &nvs) == ESP_OK) {
        size_t size = sizeof(value);
        if (nvs_get_str(nvs, key, value, &size) == ESP_OK && value[0]) strlcpy(summary->name, value, sizeof(summary->name));
        nvs_close(nvs);
    }
}

static uint32_t crc32_bytes(const void *data, size_t length)
{
    const uint8_t *p = data;
    uint32_t crc = ~0U;
    for (size_t i = 0; i < length; ++i) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xedb88320U & -(crc & 1U));
    }
    return ~crc;
}

static void cfg_seal(void)
{
    col_cfg.magic = COL_NVS_MAGIC;
    col_cfg.crc = crc32_bytes(&col_cfg, offsetof(col_config_t, crc));
}

static esp_err_t cfg_save(void)
{
    cfg_seal();
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("collection", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(nvs, "state", &col_cfg, sizeof(col_cfg));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    return err;
}

static bool record_valid(const col_record_t *r)
{
    return r->magic == COL_MAGIC && r->log_sequence != 0 &&
           r->crc == crc32_bytes(r, offsetof(col_record_t, crc));
}

static bool record_blank(const col_record_t *r)
{
    const uint32_t *p = (const uint32_t *)r;
    for (unsigned i = 0; i < COL_RECORD / 4; ++i) if (p[i] != UINT32_MAX) return false;
    return true;
}

static bool read_slot(uint32_t slot, col_record_t *r)
{
    return col_part && esp_partition_read(col_part, slot * COL_RECORD, r, sizeof(*r)) == ESP_OK;
}

static col_summary_t *summary_for(uint32_t id, bool create)
{
    for (size_t i = 0; i < session_count; ++i) if (sessions[i].id == id) return &sessions[i];
    if (!create) return NULL;
    if (session_count == COL_SESSIONS) {
        memmove(sessions, sessions + 1, sizeof(sessions[0]) * (COL_SESSIONS - 1));
        session_count--;
    }
    col_summary_t *s = &sessions[session_count++];
    memset(s, 0, sizeof(*s));
    s->id = id;
    load_session_name(s);
    return s;
}

static void summary_remove(uint32_t id)
{
    for (size_t i = 0; i < session_count; ++i) {
        if (sessions[i].id != id) continue;
        memmove(&sessions[i], &sessions[i + 1], (session_count - i - 1) * sizeof(sessions[0]));
        session_count--;
        return;
    }
}

static void summary_apply(const col_record_t *r)
{
    if (r->session_id > highest_session_id) highest_session_id = r->session_id;
    if ((r->kind_flags & 3U) == COL_DELETE) {
        summary_remove(r->session_id);
        return;
    }
    col_summary_t *s = summary_for(r->session_id, true);
    if (!s) return;
    switch (r->kind_flags & 3U) {
        case COL_START:
            s->interval_ms = r->value_raw;
            s->function = r->function;
            s->unit = r->unit & 0x0f;
            if (r->kind_flags & COL_CONFIG_FLAG) {
                s->limit_kind = r->range;
                s->target = r->elapsed_ds;
                s->settings_known = true;
            }
            if (!s->start_utc_s) s->start_utc_s = r->utc_s;
            if (r->kind_flags & COL_RESUME_FLAG) s->stopped = false;
            break;
        case COL_STOP:
            s->stopped = true;
            s->elapsed_ds = r->elapsed_ds;
            break;
        case COL_SAMPLE:
            if (s->count == 0) { s->first_seq = r->log_sequence; s->first_utc_s = r->utc_s; }
            s->count++;
            s->last_seq = r->log_sequence;
            s->last_utc_s = r->utc_s;
            s->elapsed_ds = r->elapsed_ds;
            s->function = r->function;
            if (!s->unit) s->unit = r->unit & 0x0f;
            break;
    }
}

static bool slot_is_erased(uint32_t slot)
{
    col_record_t r;
    return read_slot(slot, &r) && record_blank(&r);
}

static esp_err_t write_record(col_record_t *r)
{
    if (!col_part || !col_slots) return ESP_ERR_INVALID_STATE;
    for (uint32_t tried = 0; tried < col_slots; ++tried) {
        if (write_slot % (COL_SECTOR / COL_RECORD) == 0) {
            if (export_active) return ESP_ERR_INVALID_STATE;
            esp_err_t err = esp_partition_erase_range(col_part, write_slot * COL_RECORD, COL_SECTOR);
            if (err != ESP_OK) return err;
        }
        if (slot_is_erased(write_slot)) break;
        write_slot = (write_slot + 1) % col_slots;
    }
    if (!slot_is_erased(write_slot)) return ESP_ERR_NO_MEM;
    r->magic = COL_MAGIC;
    r->log_sequence = next_log_sequence++;
    r->crc = crc32_bytes(r, offsetof(col_record_t, crc));
    esp_err_t err = esp_partition_write(col_part, write_slot * COL_RECORD, r, sizeof(*r));
    if (err == ESP_OK) {
        summary_apply(r);
        write_slot = (write_slot + 1) % col_slots;
    }
    return err;
}

static uint32_t now_utc_s(void)
{
    time_t now = time(NULL);
    if (!time_synced || now < 1700000000 || (uint64_t)now > UINT32_MAX) return 0;
    return (uint32_t)now;
}

static uint8_t now_tenth(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return time_synced ? (uint8_t)(tv.tv_usec / 100000) : 0;
}

static esp_err_t collection_stop_locked(bool completed)
{
    if (completed) col_cfg.pending_done_id = col_cfg.session_id;
    col_cfg.active = 0;
    col_active = false;
    col_cfg.elapsed_ds += (uint32_t)((esp_timer_get_time() / 1000 - started_ms) / 100);
    esp_err_t err = cfg_save();
    col_record_t end = { .session_id = col_cfg.session_id, .utc_s = now_utc_s(),
                         .elapsed_ds = col_cfg.elapsed_ds, .kind_flags = COL_STOP,
                         .function = col_cfg.function, .unit = col_cfg.display_unit | (now_tenth() << 4) };
    esp_err_t write_err = write_record(&end);
    return err == ESP_OK ? write_err : err;
}

static void collection_task(void *arg)
{
    (void)arg;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(50));
        if (!storage_ready && col_cfg.active) {
            if (xSemaphoreTake(col_lock, pdMS_TO_TICKS(100)) == pdTRUE) {
                scan_storage();
                xSemaphoreGive(col_lock);
            }
        }
        if (!col_active || !col_part || export_active) continue;
        uint64_t now_ms = esp_timer_get_time() / 1000;
        if (now_ms < next_due_ms) continue;
        if (xSemaphoreTake(col_lock, pdMS_TO_TICKS(100)) != pdTRUE) continue;
        if (!col_active) { xSemaphoreGive(col_lock); continue; }
        now_ms = esp_timer_get_time() / 1000;
        uint32_t elapsed_ds = col_cfg.elapsed_ds + (uint32_t)((now_ms - started_ms) / 100);
        col_summary_t *summary = summary_for(col_cfg.session_id, false);
        uint32_t count = summary ? summary->count : 0;
        if ((col_cfg.limit_kind == 1 && count >= col_cfg.target) ||
            (col_cfg.limit_kind == 2 && elapsed_ds >= col_cfg.target * 600U)) {
            collection_stop_locked(true);
            xSemaphoreGive(col_lock);
            continue;
        }
        measurement_snapshot_t m = {0};
        if (measurement_get_snapshot(&m) && m.timestamp_ms && m.sequence != last_snapshot_sequence &&
            m.function == col_cfg.function && now_ms - m.timestamp_ms <= 3000) {
            col_record_t r = { .session_id = col_cfg.session_id, .utc_s = now_utc_s(),
                .elapsed_ds = elapsed_ds, .value_raw = m.value_raw,
                .kind_flags = (m.sign ? 4 : 0) | (m.valid ? 8 : 0) | (m.overrange ? 16 : 0),
                .function = m.function, .unit = (m.unit & 0x0f) | (now_tenth() << 4), .range = m.range };
            esp_err_t err = write_record(&r);
            if (err == ESP_OK) last_snapshot_sequence = m.sequence;
            else ESP_LOGE(TAG, "Flash write failed: %s", esp_err_to_name(err));
        }
        if (!col_cfg.alerted_full && next_log_sequence >= col_slots * 95U / 100U) {
            col_cfg.alerted_full = 1;
            col_cfg.pending_full_id = col_cfg.session_id;
            cfg_save();
        }
        next_due_ms = now_ms + col_cfg.interval_ms;
        xSemaphoreGive(col_lock);
    }
}

bool collection_is_active(void) { return col_active; }
bool collection_alert_pending(void)
{
    if (!col_lock || !telegram_token[0] || !telegram_chat[0]) return false;
    xSemaphoreTake(col_lock, portMAX_DELAY);
    bool pending = col_cfg.pending_full_id || col_cfg.pending_done_id;
    xSemaphoreGive(col_lock);
    return pending;
}
bool collection_network_busy(void)
{
    return send_state == 1 || export_active ||
           (send_queue && uxQueueMessagesWaiting(send_queue));
}
uint8_t collection_active_function(void) { return col_active ? col_cfg.function : 0; }
const char *collection_ntp_server(void) { return ntp_server; }
void collection_time_synced(void) { time_synced = true; }
bool collection_time_is_synced(void) { return time_synced; }

static void apply_timezone(void)
{
    const char *posix = "CST6CDT,M3.2.0/2,M11.1.0/2";
    if (strcmp(timezone_name, "UTC") == 0) posix = "UTC0";
    else if (strcmp(timezone_name, "America/New_York") == 0) posix = "EST5EDT,M3.2.0/2,M11.1.0/2";
    else if (strcmp(timezone_name, "America/Denver") == 0) posix = "MST7MDT,M3.2.0/2,M11.1.0/2";
    else if (strcmp(timezone_name, "America/Los_Angeles") == 0) posix = "PST8PDT,M3.2.0/2,M11.1.0/2";
    setenv("TZ", posix, 1);
    tzset();
}

void collection_init(void)
{
    col_lock = xSemaphoreCreateMutex();
    col_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, "data_collection");
    if (!col_lock || !col_part || col_part->size % COL_SECTOR) {
        ESP_LOGE(TAG, "Data collection partition unavailable");
        return;
    }
    col_slots = col_part->size / COL_RECORD;
    nvs_handle_t nvs;
    if (nvs_open("collection", NVS_READONLY, &nvs) == ESP_OK) {
        size_t size = sizeof(col_cfg);
        if (nvs_get_blob(nvs, "state", &col_cfg, &size) != ESP_OK || size != sizeof(col_cfg) ||
            col_cfg.magic != COL_NVS_MAGIC || col_cfg.crc != crc32_bytes(&col_cfg, offsetof(col_config_t, crc)))
            memset(&col_cfg, 0, sizeof(col_cfg));
        size = sizeof(ntp_server); nvs_get_str(nvs, "ntp", ntp_server, &size);
        size = sizeof(timezone_name); nvs_get_str(nvs, "tz", timezone_name, &size);
        size = sizeof(telegram_token); nvs_get_str(nvs, "bot", telegram_token, &size);
        size = sizeof(telegram_chat); nvs_get_str(nvs, "chat", telegram_chat, &size);
        nvs_close(nvs);
    }
    apply_timezone();
    send_queue = xQueueCreate(4, sizeof(uint32_t));
    xTaskCreate(collection_task, "collection", 4096, NULL, 4, NULL);
}

static void scan_storage(void)
{
    if (storage_ready || !col_part) return;
    uint32_t max_seq = 0, max_slot = col_slots - 1;
    const uint32_t slots_per_sector = COL_SECTOR / COL_RECORD;
    col_record_t *records = (col_record_t *)scan_buffer;
    const uint32_t sector_count = col_part->size / COL_SECTOR;
    for (uint32_t sector = 0; sector < sector_count; ++sector) {
        if (esp_partition_read(col_part, sector * COL_SECTOR, scan_buffer, COL_SECTOR) != ESP_OK) continue;
        for (uint32_t local = 0; local < slots_per_sector; ++local) {
            col_record_t *candidate = &records[local];
            if (record_valid(candidate) && candidate->log_sequence > max_seq) {
                max_seq = candidate->log_sequence;
                max_slot = sector * slots_per_sector + local;
            }
        }
    }
    next_log_sequence = max_seq + 1;
    if (!next_log_sequence) next_log_sequence = 1;
    write_slot = (max_slot + 1) % col_slots;
    const uint32_t start_sector = ((max_slot + 1) % col_slots) / slots_per_sector;
    const uint32_t start_local = (max_slot + 1) % slots_per_sector;
    for (uint32_t sector_offset = 0; sector_offset < sector_count; ++sector_offset) {
        uint32_t sector = (start_sector + sector_offset) % sector_count;
        if (esp_partition_read(col_part, sector * COL_SECTOR, scan_buffer, COL_SECTOR) != ESP_OK) continue;
        uint32_t first = sector_offset == 0 ? start_local : 0;
        for (uint32_t local = first; local < slots_per_sector; ++local) {
            if (record_valid(&records[local])) summary_apply(&records[local]);
        }
    }
    // Complete the circular scan with records before the next write slot.
    if (start_local &&
        esp_partition_read(col_part, start_sector * COL_SECTOR, scan_buffer, COL_SECTOR) == ESP_OK) {
        for (uint32_t local = 0; local < start_local; ++local) {
            if (record_valid(&records[local])) summary_apply(&records[local]);
        }
    }
    if (col_cfg.session_id > highest_session_id) highest_session_id = col_cfg.session_id;
    if (col_cfg.active && col_cfg.function >= 1 && col_cfg.function <= 7) {
        col_summary_t *s = summary_for(col_cfg.session_id, false);
        if (s && s->stopped) { col_cfg.active = 0; cfg_save(); }
        else {
            if (s && !s->settings_known) {
                s->interval_ms = col_cfg.interval_ms;
                s->limit_kind = col_cfg.limit_kind;
                s->target = col_cfg.target;
                s->settings_known = true;
            }
            col_cfg.elapsed_ds = s ? s->elapsed_ds : col_cfg.elapsed_ds;
            current_fun = col_cfg.function;
            col_active = true;
            started_ms = esp_timer_get_time() / 1000;
            next_due_ms = started_ms + col_cfg.interval_ms;
            ESP_LOGI(TAG, "Resumed collection %" PRIu32 " after power loss", col_cfg.session_id);
        }
    }
    col_summary_t *latest = summary_for(col_cfg.session_id, false);
    if (latest && !latest->settings_known) {
        latest->interval_ms = col_cfg.interval_ms;
        latest->limit_kind = col_cfg.limit_kind;
        latest->target = col_cfg.target;
        latest->settings_known = true;
    }
    ESP_LOGI(TAG, "Flash records scanned; sessions=%u next sequence=%" PRIu32,
             (unsigned)session_count, next_log_sequence);
    storage_ready = true;
}

static esp_err_t api_error(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    char body[128];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", message);
    return httpd_resp_sendstr(req, body);
}

static int unhex(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool form_value(const char *body, const char *key, char *out, size_t cap)
{
    size_t k = strlen(key);
    const char *p = body;
    while (*p) {
        const char *end = strchr(p, '&');
        if (!end) end = p + strlen(p);
        if ((size_t)(end - p) > k && !strncmp(p, key, k) && p[k] == '=') {
            p += k + 1;
            size_t n = 0;
            while (p < end) {
                char c = *p++;
                if (c == '+') c = ' ';
                else if (c == '%' && end - p >= 2) {
                    int hi = unhex(p[0]), lo = unhex(p[1]);
                    if (hi < 0 || lo < 0) return false;
                    c = (char)((hi << 4) | lo);
                    p += 2;
                }
                if (!c || n + 1 >= cap) return false;
                out[n++] = c;
            }
            out[n] = 0;
            return true;
        }
        p = *end ? end + 1 : end;
    }
    return false;
}

static bool form_number(const char *body, const char *key, uint32_t *out)
{
    char value[32];
    if (!form_value(body, key, value, sizeof(value)) || !value[0] || value[0] == '-') return false;
    char *end;
    unsigned long n = strtoul(value, &end, 10);
    if (*end || n > UINT32_MAX) return false;
    *out = (uint32_t)n;
    return true;
}

static bool read_form(httpd_req_t *req, char *body, size_t cap)
{
    if ((size_t)req->content_len >= cap) return false;
    size_t got = 0;
    while (got < (size_t)req->content_len) {
        int n = httpd_req_recv(req, body + got, req->content_len - got);
        if (n <= 0) return false;
        got += n;
    }
    body[got] = 0;
    return true;
}

static bool query_number(httpd_req_t *req, const char *key, uint32_t *out)
{
    char query[128], value[24];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK || !value[0]) return false;
    char *end;
    unsigned long n = strtoul(value, &end, 10);
    if (*end || n > UINT32_MAX) return false;
    *out = (uint32_t)n;
    return true;
}

static esp_err_t collection_status_get(httpd_req_t *req)
{
    char json[400];
    xSemaphoreTake(col_lock, portMAX_DELAY);
    col_summary_t *s = summary_for(col_cfg.session_id, false);
    uint32_t count = s ? s->count : 0;
    uint32_t used = next_log_sequence > 1 ? next_log_sequence - 1 : 0;
    if (used > col_slots) used = col_slots;
    snprintf(json, sizeof(json),
        "{\"active\":%s,\"session_id\":%" PRIu32 ",\"count\":%" PRIu32
        ",\"interval_ms\":%" PRIu32 ",\"limit_kind\":%u,\"target\":%" PRIu32
        ",\"used_percent\":%u,\"capacity_records\":%" PRIu32
        ",\"time_synced\":%s,\"telegram_configured\":%s,\"paused_for_export\":%s}",
        col_active ? "true" : "false", col_cfg.session_id, count, col_cfg.interval_ms,
        col_cfg.limit_kind, col_cfg.target, col_slots ? used * 100U / col_slots : 0,
        col_slots, time_synced ? "true" : "false",
        telegram_token[0] && telegram_chat[0] ? "true" : "false", export_active ? "true" : "false");
    xSemaphoreGive(col_lock);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static bool valid_collection_settings(uint32_t interval, uint32_t kind, uint32_t target)
{
    if (interval < 500 || interval > 3600000 || kind > 2) return false;
    if (kind == 0) return target == 0;
    if (!target || target > 100000000) return false;
    return kind != 2 || target <= UINT32_MAX / 600U;
}

static bool parse_collection_settings(const char *body, uint32_t *interval, uint32_t *kind, uint32_t *target)
{
    if (!form_number(body, "interval_ms", interval) ||
        !form_number(body, "limit_kind", kind) ||
        !form_number(body, "target", target)) return false;
    return valid_collection_settings(*interval, *kind, *target);
}

static esp_err_t write_settings_record(uint32_t id, uint8_t function, uint32_t interval,
                                       uint32_t kind, uint32_t target, bool resume)
{
    col_record_t record = {
        .session_id = id,
        .value_raw = interval,
        .elapsed_ds = target,
        .kind_flags = COL_START | COL_CONFIG_FLAG | (resume ? COL_RESUME_FLAG : 0),
        .function = function,
        .range = (uint8_t)kind,
    };
    return write_record(&record);
}

static esp_err_t collection_start_post(httpd_req_t *req)
{
    char body[256];
    uint32_t interval, kind, target = 0;
    if (!read_form(req, body, sizeof(body)) ||
        !parse_collection_settings(body, &interval, &kind, &target))
        return api_error(req, "400 Bad Request", "Invalid collection settings");
    if (xSemaphoreTake(col_lock, pdMS_TO_TICKS(1000)) != pdTRUE)
        return api_error(req, "503 Service Unavailable", "Collection is busy");
    if (col_active || !col_part || current_fun < 1 || current_fun > 7 || export_active) {
        xSemaphoreGive(col_lock);
        return api_error(req, "409 Conflict", "Collection cannot start in this mode");
    }
    if (!storage_ready) scan_storage();
    control_state_t state;
    if (!control_get_state(&state) || state.function_pending || state.range_pending) {
        xSemaphoreGive(col_lock);
        return api_error(req, "409 Conflict", "Wait for mode or range change");
    }
    uint32_t next_id = highest_session_id + 1;
    if (!next_id) {
        xSemaphoreGive(col_lock);
        return api_error(req, "507 Insufficient Storage", "Collection ID space is exhausted");
    }
    uint32_t pending_done = col_cfg.pending_done_id, pending_full = col_cfg.pending_full_id;
    col_cfg = (col_config_t){ .session_id = next_id, .interval_ms = interval, .target = target,
        .limit_kind = (uint8_t)kind, .function = current_fun, .display_unit = 0, .active = 1 };
    col_cfg.pending_done_id = pending_done;
    col_cfg.pending_full_id = pending_full;
    esp_err_t err = cfg_save();
    if (err == ESP_OK) {
        col_record_t start = { .session_id = next_id, .utc_s = now_utc_s(),
            .value_raw = interval, .elapsed_ds = target,
            .kind_flags = COL_START | COL_CONFIG_FLAG, .function = current_fun,
            .unit = now_tenth() << 4, .range = (uint8_t)kind };
        err = write_record(&start);
    }
    if (err == ESP_OK) {
        col_active = true;
        last_snapshot_sequence = 0;
        started_ms = esp_timer_get_time() / 1000;
        next_due_ms = started_ms;
    } else {
        col_cfg.active = 0;
        cfg_save();
    }
    xSemaphoreGive(col_lock);
    if (err != ESP_OK) return api_error(req, "500 Internal Server Error", "Unable to start collection");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"accepted\":true}");
}

static esp_err_t collection_update_post(httpd_req_t *req)
{
    char body[256];
    uint32_t id, interval, kind, target;
    if (!read_form(req, body, sizeof(body)) || !form_number(body, "id", &id) ||
        !parse_collection_settings(body, &interval, &kind, &target))
        return api_error(req, "400 Bad Request", "Invalid collection settings");
    if (xSemaphoreTake(col_lock, pdMS_TO_TICKS(1000)) != pdTRUE)
        return api_error(req, "503 Service Unavailable", "Collection is busy");
    if (!storage_ready) scan_storage();
    col_summary_t *s = summary_for(id, false);
    if (!s) { xSemaphoreGive(col_lock); return api_error(req, "404 Not Found", "Collection not found"); }
    if (col_active || export_active || collection_network_busy()) {
        xSemaphoreGive(col_lock);
        return api_error(req, "409 Conflict", "Stop collection or export before editing");
    }
    esp_err_t err = write_settings_record(id, s->function, interval, kind, target, false);
    xSemaphoreGive(col_lock);
    if (err != ESP_OK) return api_error(req, "500 Internal Server Error", "Unable to save collection settings");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"accepted\":true}");
}

static esp_err_t collection_resume_post(httpd_req_t *req)
{
    char body[64];
    uint32_t id;
    if (!read_form(req, body, sizeof(body)) || !form_number(body, "id", &id))
        return api_error(req, "400 Bad Request", "Invalid collection ID");
    if (xSemaphoreTake(col_lock, pdMS_TO_TICKS(1000)) != pdTRUE)
        return api_error(req, "503 Service Unavailable", "Collection is busy");
    if (!storage_ready) scan_storage();
    col_summary_t *s = summary_for(id, false);
    if (!s) { xSemaphoreGive(col_lock); return api_error(req, "404 Not Found", "Collection not found"); }
    if (col_active || export_active || collection_network_busy()) {
        xSemaphoreGive(col_lock);
        return api_error(req, "409 Conflict", "Another collection or export is active");
    }
    if (!s->settings_known || !valid_collection_settings(s->interval_ms, s->limit_kind, s->target)) {
        xSemaphoreGive(col_lock);
        return api_error(req, "409 Conflict", "Save interval and length before continuing");
    }
    if (current_fun != s->function) {
        xSemaphoreGive(col_lock);
        return api_error(req, "409 Conflict", "Select the collection's measurement mode first");
    }
    control_state_t state;
    if (!control_get_state(&state) || state.function_pending || state.range_pending) {
        xSemaphoreGive(col_lock);
        return api_error(req, "409 Conflict", "Wait for mode or range change");
    }
    if ((s->limit_kind == 1 && s->count >= s->target) ||
        (s->limit_kind == 2 && s->elapsed_ds >= s->target * 600U)) {
        xSemaphoreGive(col_lock);
        return api_error(req, "409 Conflict", "Increase the collection length before continuing");
    }
    uint32_t pending_done = col_cfg.pending_done_id, pending_full = col_cfg.pending_full_id;
    col_cfg = (col_config_t){ .session_id = id, .interval_ms = s->interval_ms,
        .target = s->target, .elapsed_ds = s->elapsed_ds, .limit_kind = s->limit_kind,
        .function = s->function, .active = 1, .pending_done_id = pending_done,
        .pending_full_id = pending_full };
    esp_err_t err = cfg_save();
    if (err == ESP_OK) err = write_settings_record(id, s->function, s->interval_ms,
                                                    s->limit_kind, s->target, true);
    if (err == ESP_OK) {
        col_active = true;
        last_snapshot_sequence = 0;
        started_ms = esp_timer_get_time() / 1000;
        next_due_ms = started_ms;
    } else {
        col_cfg.active = 0;
        cfg_save();
    }
    xSemaphoreGive(col_lock);
    if (err != ESP_OK) return api_error(req, "500 Internal Server Error", "Unable to continue collection");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"accepted\":true}");
}

static esp_err_t collection_delete_post(httpd_req_t *req)
{
    char body[64], key[16];
    uint32_t id;
    if (!read_form(req, body, sizeof(body)) || !form_number(body, "id", &id))
        return api_error(req, "400 Bad Request", "Invalid collection ID");
    if (xSemaphoreTake(col_lock, pdMS_TO_TICKS(1000)) != pdTRUE)
        return api_error(req, "503 Service Unavailable", "Collection is busy");
    if (!storage_ready) scan_storage();
    col_summary_t *s = summary_for(id, false);
    if (!s) { xSemaphoreGive(col_lock); return api_error(req, "404 Not Found", "Collection not found"); }
    if (col_active || export_active || collection_network_busy()) {
        xSemaphoreGive(col_lock);
        return api_error(req, "409 Conflict", "Stop collection or export before deleting");
    }
    col_record_t tombstone = { .session_id = id, .kind_flags = COL_DELETE };
    esp_err_t err = write_record(&tombstone);
    if (err == ESP_OK) {
        if (col_cfg.pending_done_id == id) col_cfg.pending_done_id = 0;
        if (col_cfg.pending_full_id == id) col_cfg.pending_full_id = 0;
        cfg_save();
    }
    xSemaphoreGive(col_lock);
    if (err != ESP_OK) return api_error(req, "500 Internal Server Error", "Unable to delete collection");
    nvs_handle_t nvs = 0;
    session_name_key(id, key, sizeof(key));
    if (nvs_open("collection", NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_key(nvs, key);
        nvs_commit(nvs);
        nvs_close(nvs);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"accepted\":true}");
}

static esp_err_t collection_stop_post(httpd_req_t *req)
{
    if (xSemaphoreTake(col_lock, pdMS_TO_TICKS(1000)) != pdTRUE)
        return api_error(req, "503 Service Unavailable", "Collection is busy");
    if (!col_active) { xSemaphoreGive(col_lock); return api_error(req, "409 Conflict", "No active collection"); }
    esp_err_t err = collection_stop_locked(false);
    xSemaphoreGive(col_lock);
    if (err != ESP_OK) return api_error(req, "500 Internal Server Error", "Unable to stop collection");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"accepted\":true}");
}

static esp_err_t collection_rename_post(httpd_req_t *req)
{
    char body[128], name[32], key[16];
    uint32_t id;
    if (!read_form(req, body, sizeof(body)) || !form_number(body, "id", &id) ||
        !form_value(body, "name", name, sizeof(name)) || !name[0])
        return api_error(req, "400 Bad Request", "Invalid collection name");
    col_summary_t *summary = summary_for(id, false);
    if (!summary) return api_error(req, "404 Not Found", "Collection not found");
    nvs_handle_t nvs;
    session_name_key(id, key, sizeof(key));
    esp_err_t err = nvs_open("collection", NVS_READWRITE, &nvs);
    if (err == ESP_OK) err = nvs_set_str(nvs, key, name);
    if (err == ESP_OK) err = nvs_commit(nvs);
    if (nvs) nvs_close(nvs);
    if (err != ESP_OK) return api_error(req, "500 Internal Server Error", "Unable to save collection name");
    strlcpy(summary->name, name, sizeof(summary->name));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"accepted\":true}");
}

static esp_err_t collection_sessions_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "{\"sessions\":[", HTTPD_RESP_USE_STRLEN);
    xSemaphoreTake(col_lock, portMAX_DELAY);
    if (!storage_ready) scan_storage();
    char json[560], escaped_name[70];
    for (size_t i = 0; i < session_count; ++i) {
        col_summary_t *s = &sessions[session_count - 1 - i];
        json_escape_local(s->name, escaped_name, sizeof(escaped_name));
        snprintf(json, sizeof(json),
            "%s{\"id\":%" PRIu32 ",\"name\":\"%s\",\"count\":%" PRIu32 ",\"start_utc_s\":%" PRIu32 ",\"first_sequence\":%" PRIu32
            ",\"last_sequence\":%" PRIu32 ",\"first_utc_s\":%" PRIu32
            ",\"last_utc_s\":%" PRIu32 ",\"elapsed_ds\":%" PRIu32
            ",\"interval_ms\":%" PRIu32 ",\"limit_kind\":%u,\"target\":%" PRIu32
            ",\"settings_known\":%s,\"function\":\"%s\",\"function_id\":%u,\"collecting\":%s}",
            i ? "," : "", s->id, escaped_name, s->count, s->start_utc_s, s->first_seq, s->last_seq,
            s->first_utc_s, s->last_utc_s, s->elapsed_ds, s->interval_ms,
            s->limit_kind, s->target, s->settings_known ? "true" : "false",
            function_name(s->function), s->function, col_active && s->id == col_cfg.session_id ? "true" : "false");
        if (httpd_resp_send_chunk(req, json, HTTPD_RESP_USE_STRLEN) != ESP_OK) break;
    }
    xSemaphoreGive(col_lock);
    httpd_resp_send_chunk(req, "]}", 2);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static int csv_line(const col_record_t *r, char *out, size_t cap)
{
    char utc[32] = "", local[40] = "";
    if (r->utc_s) {
        time_t sec = r->utc_s;
        struct tm tmv;
        gmtime_r(&sec, &tmv);
        strftime(utc, sizeof(utc), "%Y-%m-%dT%H:%M:%S", &tmv);
        localtime_r(&sec, &tmv);
        strftime(local, sizeof(local), "%Y-%m-%dT%H:%M:%S%z", &tmv);
    }
    return snprintf(out, cap, "%" PRIu32 ",%s.%uZ,%s,%" PRIu32 ",%.9g,%s,%s,%s,%u,%u,%" PRIu32 "\n",
        r->log_sequence, utc, (unsigned)((r->unit >> 4) & 0x0f), local, r->elapsed_ds,
        standard_value(r), standard_unit(r->function), function_name(r->function),
        range_name(r->range), r->kind_flags & 8U ? 1U : 0U,
        r->kind_flags & 16U ? 1U : 0U, r->session_id);
}

static esp_err_t collection_records_get(httpd_req_t *req)
{
    uint32_t id, offset = 0, limit = 40;
    if (!query_number(req, "id", &id) ||
        (query_number(req, "offset", &offset) && offset > col_slots) ||
        (query_number(req, "limit", &limit) && (limit == 0 || limit > 100)))
        return api_error(req, "400 Bad Request", "Invalid records query");
    xSemaphoreTake(col_lock, portMAX_DELAY);
    if (!storage_ready) scan_storage();
    if (!summary_for(id, false)) {
        xSemaphoreGive(col_lock);
        return api_error(req, "404 Not Found", "Collection not found");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send_chunk(req, "{\"records\":[", HTTPD_RESP_USE_STRLEN);
    uint32_t emitted = 0, seen = 0;
    char json[270];
    col_record_t r;
    for (uint32_t i = 0; i < col_slots && emitted < limit; ++i) {
        uint32_t slot = (write_slot + i) % col_slots;
        if (!read_slot(slot, &r) || !record_valid(&r) || r.session_id != id ||
            (r.kind_flags & 3U) != COL_SAMPLE) continue;
        if (seen++ < offset) continue;
        snprintf(json, sizeof(json),
            "%s{\"sequence\":%" PRIu32 ",\"utc_s\":%" PRIu32 ",\"elapsed_ds\":%" PRIu32
            ",\"value_raw\":%.9g,\"unit_code\":\"%s\",\"function\":\"%s\","
            "\"range\":\"%s\",\"valid\":%s,\"overrange\":%s}", emitted ? "," : "",
            r.log_sequence, r.utc_s, r.elapsed_ds, standard_value(&r), standard_unit(r.function),
            function_name(r.function), range_name(r.range), r.kind_flags & 8U ? "true" : "false",
            r.kind_flags & 16U ? "true" : "false");
        if (httpd_resp_send_chunk(req, json, HTTPD_RESP_USE_STRLEN) != ESP_OK) break;
        emitted++;
    }
    xSemaphoreGive(col_lock);
    snprintf(json, sizeof(json), "],\"offset\":%" PRIu32 ",\"returned\":%" PRIu32 "}", offset, emitted);
    httpd_resp_send_chunk(req, json, HTTPD_RESP_USE_STRLEN);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t collection_csv_get(httpd_req_t *req)
{
    uint32_t id;
    if (!query_number(req, "id", &id)) return api_error(req, "400 Bad Request", "Missing session ID");
    char filename[80];
    xSemaphoreTake(col_lock, portMAX_DELAY);
    if (!storage_ready) scan_storage();
    col_summary_t *s = summary_for(id, false);
    if (!s || export_active) {
        xSemaphoreGive(col_lock);
        return api_error(req, "404 Not Found", "Session unavailable or export busy");
    }
    uint32_t endpoint = s->last_seq;
    csv_filename(id, s->name, filename, sizeof(filename));
    export_active = true;
    xSemaphoreGive(col_lock);
    httpd_resp_set_type(req, "text/csv; charset=utf-8");
    char disposition[112];
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", filename);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    esp_err_t result = httpd_resp_send_chunk(req,
        "sequence,utc,local,elapsed_ds,value_raw,unit_code,function,range,valid,overrange,session_id\n",
        HTTPD_RESP_USE_STRLEN);
    char line[200];
    col_record_t r;
    uint32_t origin = write_slot;
    for (uint32_t i = 0; result == ESP_OK && i < col_slots; ++i) {
        xSemaphoreTake(col_lock, portMAX_DELAY);
        bool okay = read_slot((origin + i) % col_slots, &r);
        xSemaphoreGive(col_lock);
        if (!okay || !record_valid(&r) || r.session_id != id ||
            (r.kind_flags & 3U) != COL_SAMPLE || r.log_sequence > endpoint) continue;
        int n = csv_line(&r, line, sizeof(line));
        if (n > 0 && n < sizeof(line)) result = httpd_resp_send_chunk(req, line, n);
    }
    if (result == ESP_OK) result = httpd_resp_send_chunk(req, NULL, 0);
    export_active = false;
    return result;
}

static bool telegram_write(esp_http_client_handle_t client, const char *data, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        int n = esp_http_client_write(client, data + sent, length - sent);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

static esp_http_client_handle_t telegram_client(const char *method)
{
    char url[180];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/%s", telegram_token, method);
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    return esp_http_client_init(&config);
}

static bool telegram_text(uint32_t id, bool full)
{
    if (!telegram_token[0] || !telegram_chat[0] || net_state == 0) return false;
    esp_http_client_handle_t client = telegram_client("sendMessage");
    if (!client) return false;
    char body[180];
    snprintf(body, sizeof(body), "chat_id=%s&text=Doer.ee%%20collection%%20%%23%" PRIu32
        "%%20%s", telegram_chat, id, full ? "Flash%%20is%%2095%%25%%20full" : "complete");
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_post_field(client, body, strlen(body));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err == ESP_OK && status == 200;
}

static bool telegram_csv(uint32_t id)
{
    if (!telegram_token[0] || !telegram_chat[0] || net_state == 0 || !time_synced) return false;
    char filename[80];
    xSemaphoreTake(col_lock, portMAX_DELAY);
    col_summary_t *s = summary_for(id, false);
    if (!s || !s->count || export_active) { xSemaphoreGive(col_lock); return false; }
    uint32_t endpoint = s->last_seq;
    uint32_t origin = write_slot;
    csv_filename(id, s->name, filename, sizeof(filename));
    export_active = true;
    xSemaphoreGive(col_lock);

    static const char *csv_header =
        "sequence,utc,local,elapsed_ds,value_raw,unit_code,function,range,valid,overrange,session_id\n";
    static const char *boundary = "doercollectionboundary";
    char prefix[300];
    int prefix_len = snprintf(prefix, sizeof(prefix),
        "--%s\r\nContent-Disposition: form-data; name=\"chat_id\"\r\n\r\n%s\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"document\"; filename=\"%s\"\r\n"
        "Content-Type: text/csv\r\n\r\n", boundary, telegram_chat, boundary, filename);
    char suffix[80];
    int suffix_len = snprintf(suffix, sizeof(suffix), "\r\n--%s--\r\n", boundary);
    char line[200];
    col_record_t r;
    uint32_t bytes = prefix_len + strlen(csv_header) + suffix_len;
    for (uint32_t i = 0; i < col_slots; ++i) {
        if (!read_slot((origin + i) % col_slots, &r) || !record_valid(&r) ||
            r.session_id != id || (r.kind_flags & 3U) != COL_SAMPLE || r.log_sequence > endpoint) continue;
        int n = csv_line(&r, line, sizeof(line));
        if (n > 0 && n < sizeof(line)) bytes += n;
    }
    esp_http_client_handle_t client = telegram_client("sendDocument");
    bool okay = false;
    if (client) {
        char content_type[100];
        snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);
        esp_http_client_set_header(client, "Content-Type", content_type);
        if (esp_http_client_open(client, bytes) == ESP_OK &&
            telegram_write(client, prefix, prefix_len) &&
            telegram_write(client, csv_header, strlen(csv_header))) {
            okay = true;
            for (uint32_t i = 0; i < col_slots; ++i) {
                if (!read_slot((origin + i) % col_slots, &r) || !record_valid(&r) ||
                    r.session_id != id || (r.kind_flags & 3U) != COL_SAMPLE || r.log_sequence > endpoint) continue;
                int n = csv_line(&r, line, sizeof(line));
                if (n > 0 && n < sizeof(line) && !telegram_write(client, line, n)) { okay = false; break; }
            }
            if (okay) okay = telegram_write(client, suffix, suffix_len);
            if (okay) {
                esp_http_client_fetch_headers(client);
                okay = esp_http_client_get_status_code(client) == 200;
            }
        }
        esp_http_client_cleanup(client);
    }
    export_active = false;
    return okay;
}

static void telegram_task(void *arg)
{
    (void)arg;
    while (true) {
        uint32_t requested = 0;
        xQueueReceive(send_queue, &requested, pdMS_TO_TICKS(30000));
        if (!telegram_token[0] || !telegram_chat[0] || net_state == 0) continue;
        if (requested) {
            send_state = 1;
            send_session_id = requested;
            bool okay = telegram_csv(requested);
            send_state = okay ? 2 : 3;
            if (!okay) ESP_LOGW(TAG, "Telegram CSV send failed for session %" PRIu32, requested);
        }
        uint32_t full, done;
        xSemaphoreTake(col_lock, portMAX_DELAY);
        full = col_cfg.pending_full_id;
        done = col_cfg.pending_done_id;
        xSemaphoreGive(col_lock);
        if (full && telegram_text(full, true)) {
            xSemaphoreTake(col_lock, portMAX_DELAY);
            if (col_cfg.pending_full_id == full) { col_cfg.pending_full_id = 0; cfg_save(); }
            xSemaphoreGive(col_lock);
        }
        if (done && telegram_csv(done)) {
            xSemaphoreTake(col_lock, portMAX_DELAY);
            if (col_cfg.pending_done_id == done) { col_cfg.pending_done_id = 0; cfg_save(); }
            xSemaphoreGive(col_lock);
        }
    }
}

static esp_err_t collection_send_post(httpd_req_t *req)
{
    char body[64];
    uint32_t id;
    if (!read_form(req, body, sizeof(body)) || !form_number(body, "id", &id))
        return api_error(req, "400 Bad Request", "Invalid session ID");
    if (!telegram_token[0] || !telegram_chat[0])
        return api_error(req, "409 Conflict", "Configure Telegram in Settings");
    xSemaphoreTake(col_lock, portMAX_DELAY);
    col_summary_t *s = summary_for(id, false);
    bool exists = s && s->count;
    xSemaphoreGive(col_lock);
    if (!exists) return api_error(req, "404 Not Found", "No records in session");
    if (xQueueSend(send_queue, &id, 0) != pdTRUE)
        return api_error(req, "503 Service Unavailable", "Send queue is full");
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"accepted\":true}");
}

static bool valid_hostname(const char *s)
{
    size_t len = strlen(s);
    if (!len || len > 63 || s[0] == '-' || s[len - 1] == '-') return false;
    for (size_t i = 0; i < len; ++i)
        if (!isalnum((unsigned char)s[i]) && s[i] != '.' && s[i] != '-') return false;
    return true;
}

static bool valid_timezone(const char *s)
{
    return !strcmp(s, "America/Chicago") || !strcmp(s, "America/New_York") ||
           !strcmp(s, "America/Denver") || !strcmp(s, "America/Los_Angeles") ||
           !strcmp(s, "UTC");
}

static esp_err_t collection_settings_get(httpd_req_t *req)
{
    char json[270];
    snprintf(json, sizeof(json), "{\"ntp_server\":\"%s\",\"timezone\":\"%s\","
        "\"telegram_configured\":%s,\"time_synced\":%s,\"send_state\":%u,\"send_session_id\":%" PRIu32 "}",
        ntp_server, timezone_name, telegram_token[0] && telegram_chat[0] ? "true" : "false",
        time_synced ? "true" : "false", send_state, send_session_id);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t collection_time_post(httpd_req_t *req)
{
    char body[256], server[64], zone[40];
    if (!read_form(req, body, sizeof(body)) ||
        !form_value(body, "server", server, sizeof(server)) || !valid_hostname(server) ||
        !form_value(body, "timezone", zone, sizeof(zone)) || !valid_timezone(zone))
        return api_error(req, "400 Bad Request", "Invalid time settings");
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("collection", NVS_READWRITE, &nvs);
    if (err == ESP_OK) err = nvs_set_str(nvs, "ntp", server);
    if (err == ESP_OK) err = nvs_set_str(nvs, "tz", zone);
    if (err == ESP_OK) err = nvs_commit(nvs);
    if (nvs) nvs_close(nvs);
    if (err != ESP_OK) return api_error(req, "500 Internal Server Error", "Unable to save time settings");
    strlcpy(ntp_server, server, sizeof(ntp_server));
    strlcpy(timezone_name, zone, sizeof(timezone_name));
    apply_timezone();
    if (sntp_enabled()) sntp_setservername(0, ntp_server);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"accepted\":true}");
}

static esp_err_t collection_telegram_post(httpd_req_t *req)
{
    char body[300], token[96], chat[40];
    if (!read_form(req, body, sizeof(body)) ||
        !form_value(body, "token", token, sizeof(token)) ||
        !form_value(body, "chat", chat, sizeof(chat)))
        return api_error(req, "400 Bad Request", "Invalid Telegram settings");
    if (token[0]) {
        size_t len = strlen(token);
        if (len < 30) return api_error(req, "400 Bad Request", "Invalid bot token");
        for (size_t i = 0; i < len; ++i)
            if (!isalnum((unsigned char)token[i]) && token[i] != ':' && token[i] != '_' && token[i] != '-')
                return api_error(req, "400 Bad Request", "Invalid bot token");
    }
    if (!chat[0]) return api_error(req, "400 Bad Request", "Chat ID is required");
    for (size_t i = 0; chat[i]; ++i)
        if (!isdigit((unsigned char)chat[i]) && !(i == 0 && chat[i] == '-'))
            return api_error(req, "400 Bad Request", "Invalid chat ID");
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("collection", NVS_READWRITE, &nvs);
    if (err == ESP_OK && token[0]) err = nvs_set_str(nvs, "bot", token);
    if (err == ESP_OK) err = nvs_set_str(nvs, "chat", chat);
    if (err == ESP_OK) err = nvs_commit(nvs);
    if (nvs) nvs_close(nvs);
    if (err != ESP_OK) return api_error(req, "500 Internal Server Error", "Unable to save Telegram settings");
    if (token[0]) strlcpy(telegram_token, token, sizeof(telegram_token));
    strlcpy(telegram_chat, chat, sizeof(telegram_chat));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"accepted\":true}");
}

static esp_err_t collection_telegram_test_post(httpd_req_t *req)
{
    if (!telegram_token[0] || !telegram_chat[0])
        return api_error(req, "409 Conflict", "Configure Telegram first");
    bool okay = telegram_text(0, false);
    if (!okay) return api_error(req, "502 Bad Gateway", "Telegram test failed");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"accepted\":true}");
}

esp_err_t collection_register_handlers(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        {.uri="/api/collection/status", .method=HTTP_GET, .handler=collection_status_get},
        {.uri="/api/collection/start", .method=HTTP_POST, .handler=collection_start_post},
        {.uri="/api/collection/stop", .method=HTTP_POST, .handler=collection_stop_post},
        {.uri="/api/collection/rename", .method=HTTP_POST, .handler=collection_rename_post},
        {.uri="/api/collection/update", .method=HTTP_POST, .handler=collection_update_post},
        {.uri="/api/collection/resume", .method=HTTP_POST, .handler=collection_resume_post},
        {.uri="/api/collection/delete", .method=HTTP_POST, .handler=collection_delete_post},
        {.uri="/api/collection/sessions", .method=HTTP_GET, .handler=collection_sessions_get},
        {.uri="/api/collection/records", .method=HTTP_GET, .handler=collection_records_get},
        {.uri="/api/collection/csv", .method=HTTP_GET, .handler=collection_csv_get},
        {.uri="/api/collection/send", .method=HTTP_POST, .handler=collection_send_post},
        {.uri="/api/collection/settings", .method=HTTP_GET, .handler=collection_settings_get},
        {.uri="/api/collection/time", .method=HTTP_POST, .handler=collection_time_post},
        {.uri="/api/collection/telegram", .method=HTTP_POST, .handler=collection_telegram_post},
        {.uri="/api/collection/telegram/test", .method=HTTP_POST, .handler=collection_telegram_test_post},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        esp_err_t err = httpd_register_uri_handler(server, &routes[i]);
        if (err != ESP_OK) return err;
    }
    if (send_queue && !telegram_started) {
        telegram_started = xTaskCreate(telegram_task, "telegram", 6144, NULL, 3, NULL) == pdPASS;
    }
    return ESP_OK;
}

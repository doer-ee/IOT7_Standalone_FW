
#include "main.h"
#include "wifinet.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include <stdbool.h>
#include <stdlib.h>


const char *WIFINET = "WIFINET";

static QueueHandle_t wifinet_evt_queue;

char device_ID[]="0000000000";

uint8_t net_state=0;   //Network state: 0=router disconnected, 1=router connected
volatile bool wifi_config_ap_active = false;
uint8_t wait=0;
static uint8_t time_state=0;

static char wifi_ssid[32];
static char wifi_pass[64];

#define CONFIG_AP_PASSWORD "iot7setup"
#define CONFIG_AP_CHANNEL  6
#define CONFIG_AP_MAX_CONN 4

static httpd_handle_t config_httpd = NULL;
static httpd_handle_t status_httpd = NULL;
static bool wifi_stack_ready = false;
static bool wifi_started = false;
static bool config_apply_started = false;
static unsigned int wifi_connect_attempts = 0;
static unsigned int wifi_disconnect_count = 0;

static esp_err_t event_handler2(void *ctx, system_event_t *event);
static esp_err_t wifi_stack_init_once(void);
static esp_err_t config_page_get_handler(httpd_req_t *req);
static esp_err_t config_save_post_handler(httpd_req_t *req);
static esp_err_t status_page_get_handler(httpd_req_t *req);
static esp_err_t status_api_get_handler(httpd_req_t *req);
static esp_err_t measurement_api_get_handler(httpd_req_t *req);
static esp_err_t control_api_get_handler(httpd_req_t *req);
static esp_err_t control_function_post_handler(httpd_req_t *req);
static esp_err_t control_hold_post_handler(httpd_req_t *req);
static esp_err_t control_zero_post_handler(httpd_req_t *req);
static esp_err_t control_mark_post_handler(httpd_req_t *req);
static void status_httpd_start(void);
static void status_httpd_stop(void);
static void wifi_apply_task(void *arg);
static const char *wifi_disconnect_reason_name(uint8_t reason);
static void wifi_connect_with_log(const char *source);
static void app_wifi_initialise(void);
static void wifi_config_ap_start(void);


static void sntp_set_time_sync_callback(struct timeval *tv)
{
    struct tm timeinfo = {0};
    ESP_LOGI(WIFINET, "tv_sec: %lld", (uint64_t)tv->tv_sec);
    localtime_r((const time_t *)&(tv->tv_sec), &timeinfo);
    ESP_LOGI(WIFINET, "%d %d %d %d:%d:%d", timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    time_state=1;
}

static void esp_initialize_sntp(void)
{
    ESP_LOGI(WIFINET, "Initializing SNTP");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "ntp.aliyun.com");
    sntp_setservername(1, "ntp.ntsc.ac.cn");
    sntp_setservername(2, "edu.ntp.org.cn");
    sntp_setservername(3, "time1.cloud.tencent.com");

	sntp_set_time_sync_notification_cb(&sntp_set_time_sync_callback);
    sntp_init();
}


void request_wifi_config_ap(void)
{
    net_state=0;
    wait=2;
    wifi_config_ap_active = true;
    ESP_LOGI(WIFINET,"starting Wi-Fi configuration AP\r\n");
    if (wifi_stack_ready && wifinet_evt_queue != NULL) {
        uint8_t evt = WIFINET_CONFIG_AP;
        xQueueSend(wifinet_evt_queue, &evt, 0);
    }
}

static const char *wifi_disconnect_reason_name(uint8_t reason)
{
    switch (reason) {
        case WIFI_REASON_UNSPECIFIED: return "UNSPECIFIED";
        case WIFI_REASON_AUTH_EXPIRE: return "AUTH_EXPIRE";
        case WIFI_REASON_ASSOC_EXPIRE: return "ASSOC_EXPIRE";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "4WAY_HANDSHAKE_TIMEOUT";
        case WIFI_REASON_BEACON_TIMEOUT: return "BEACON_TIMEOUT";
        case WIFI_REASON_NO_AP_FOUND: return "NO_AP_FOUND";
        case WIFI_REASON_AUTH_FAIL: return "AUTH_FAIL";
        case WIFI_REASON_ASSOC_FAIL: return "ASSOC_FAIL";
        case WIFI_REASON_HANDSHAKE_TIMEOUT: return "HANDSHAKE_TIMEOUT";
        case WIFI_REASON_CONNECTION_FAIL: return "CONNECTION_FAIL";
        default: return "OTHER";
    }
}

static void wifi_connect_with_log(const char *source)
{
    wifi_connect_attempts++;
    esp_err_t err = esp_wifi_connect();
    if (err == ESP_OK) {
        ESP_LOGI(WIFINET, "STA connect attempt %u started by %s", wifi_connect_attempts, source);
    } else {
        ESP_LOGE(WIFINET, "STA connect attempt %u from %s failed immediately: %s (0x%x)",
                 wifi_connect_attempts, source, esp_err_to_name(err), err);
    }
}
 
/* The event group allows multiple bits for each event,
   but we only care about one event - are we connected
   to the AP with an IP? */

//Wi-Fi event
static esp_err_t event_handler2(void *ctx, system_event_t *event)
{
    switch (event->event_id) {
        case SYSTEM_EVENT_STA_START:
            ESP_LOGI(WIFINET, "STA started");
            wifi_connect_with_log("STA_START");
            break;
        case SYSTEM_EVENT_STA_CONNECTED:
            ESP_LOGI(WIFINET, "STA associated: channel=%u authmode=%d; waiting for DHCP",
                     (unsigned)event->event_info.connected.channel,
                     event->event_info.connected.authmode);
            break;
        case SYSTEM_EVENT_STA_GOT_IP:
            ESP_LOGI(WIFINET, "STA got IP; Wi-Fi connection ready after %u attempts and %u disconnects",
                     wifi_connect_attempts, wifi_disconnect_count);
             
            net_state=1;

            // Once DHCP completes, expose the local measurement and control UI.
            status_httpd_start();

            if(time_state==0) esp_initialize_sntp();
            break;
        case SYSTEM_EVENT_STA_DISCONNECTED:
            wifi_disconnect_count++;
            // system_event_t contains a union; casting the whole event to
            // wifi_event_sta_disconnected_t made the old log report reason 0.
            uint8_t reason = event->event_info.disconnected.reason;
            ESP_LOGW(WIFINET, "STA disconnected #%u: reason=%u (%s), SSID length=%u",
                     wifi_disconnect_count, (unsigned)reason,
                     wifi_disconnect_reason_name(reason),
                     (unsigned)event->event_info.disconnected.ssid_len);

            /* This is a workaround as ESP32 WiFi libs don't currently
               auto-reassociate. */
            wifi_connect_with_log("STA_DISCONNECTED");
           net_state=0;
            break;
        default:
            break;
    }
    return ESP_OK;
}

static esp_err_t wifi_stack_init_once(void)
{
    if (wifi_stack_ready) {
        return ESP_OK;
    }

    tcpip_adapter_init();
    esp_err_t err = esp_event_loop_init(event_handler2, NULL);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        return err;
    }

    wifi_stack_ready = true;
    return ESP_OK;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static bool form_get_value(const char *body, const char *key, char *out, size_t out_size)
{
    char copy[256];
    size_t key_len = strlen(key);
    if (out_size == 0 || strlen(body) >= sizeof(copy)) {
        return false;
    }
    strcpy(copy, body);

    char *save_ptr = NULL;
    char *field = strtok_r(copy, "&", &save_ptr);
    while (field != NULL) {
        if (strncmp(field, key, key_len) == 0 && field[key_len] == '=') {
            const char *src = field + key_len + 1;
            size_t out_len = 0;
            while (*src != '\0') {
                char decoded;
                if (*src == '+') {
                    decoded = ' ';
                    src++;
                } else if (*src == '%' && src[1] != '\0' && src[2] != '\0') {
                    int high = hex_value(src[1]);
                    int low = hex_value(src[2]);
                    if (high < 0 || low < 0) return false;
                    decoded = (char)((high << 4) | low);
                    src += 3;
                } else {
                    decoded = *src++;
                }
                if (out_len + 1 >= out_size) {
                    return false;
                }
                if (decoded == '\0') {
                    return false;
                }
                out[out_len++] = decoded;
            }
            out[out_len] = '\0';
            return true;
        }
        field = strtok_r(NULL, "&", &save_ptr);
    }
    return false;
}

/*
 * The read-only dashboard polls status and measurement independently.
 * Measurement values remain in the firmware's raw unit representation until
 * the unit scaling has been checked against a low-voltage reference.
 */
static const char status_page[] =
    "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>IOT7 Meter</title>"
    "<style>body{font-family:system-ui,sans-serif;max-width:42rem;margin:2rem auto;padding:0 1rem;"
    "background:#f5f7fa;color:#17202a}main{background:white;border-radius:12px;padding:1.2rem;"
    "box-shadow:0 2px 12px #0001}h2{margin-top:0}h3{margin:1.4rem 0 .7rem}"
    ".reading{font-size:2.6rem;font-weight:700;line-height:1.15;word-break:break-word}"
    ".reading small{font-size:1.1rem}dl{display:grid;grid-template-columns:9rem 1fr;gap:.55rem 1rem}"
    "dt{color:#667085}dd{margin:0;font-weight:600;word-break:break-word}"
    ".ok{color:#087f5b}.warn{color:#b54708}small{color:#667085}</style></head><body>"
    "<main><h2>IOT7 Meter</h2><p id='state'>Loading status...</p>"
    "<h3>Live measurement</h3><div class='reading'><span id='reading'>Waiting...</span> "
    "<small id='reading_unit'></small></div><p id='reading_state'>Waiting for a sample</p>"
    "<dl><dt>Function</dt><dd id='function'>-</dd><dt>Range</dt><dd id='range'>-</dd>"
    "<dt>Battery</dt><dd id='battery'>-</dd><dt>Sample age</dt><dd id='sample_age'>-</dd>"
    "<dt>Observed interval</dt><dd id='interval'>-</dd>"
    "<dt>Raw data</dt><dd id='raw'>-</dd></dl>"
    "<h3>Device and Wi-Fi</h3><dl><dt>Wi-Fi</dt><dd id='ssid'>-</dd>"
    "<dt>Signal strength</dt><dd id='rssi'>-</dd><dt>Channel</dt><dd id='channel'>-</dd>"
    "<dt>IP address</dt><dd id='ip'>-</dd><dt>Subnet mask</dt><dd id='netmask'>-</dd>"
    "<dt>Gateway</dt><dd id='gateway'>-</dd><dt>Device MAC</dt><dd id='mac'>-</dd>"
    "<dt>Device ID</dt><dd id='device'>-</dd><dt>Firmware</dt><dd id='firmware'>-</dd>"
    "<dt>Free memory</dt><dd id='heap'>-</dd><dt>Uptime</dt><dd id='uptime'>-</dd></dl>"
    "<h3>Controls</h3><p id='control_state'>Loading controls...</p>"
    "<label>Function <select id='control_function' onchange='controlFormDirty=true'>"
    "<option value='1'>DC voltage</option><option value='2'>AC voltage</option>"
    "<option value='3'>DC current (mA)</option><option value='4'>DC current (A)</option>"
    "<option value='5'>AC current (mA)</option><option value='6'>AC current (A)</option>"
    "<option value='7'>Resistance</option><option value='8'>Continuity</option>"
    "<option value='9'>DC power</option><option value='10'>AC power</option>"
    "<option value='11'>Diode</option></select></label>"
    "<button id='function_button' onclick='setFunction()'>Set function</button>"
    "<p><button id='hold_button' onclick='toggleHold()'>Enable hold</button> "
    "<button id='zero_button' onclick='zeroMeasurement()'>Zero</button> "
    "<button id='mark_button' onclick='markMeasurement()'>Mark</button></p>"
    "<p id='control_result'><small>Measurements refresh every second.</small></p></main>"
    "<script>const e=id=>document.getElementById(id);"
    "function text(id,v){e(id).textContent=(v===null||v===undefined||v==='')?'-':v}"
    "function json(url){return fetch(url,{cache:'no-store'}).then(r=>{if(!r.ok)throw Error(r.status);return r.json()})}"
    "function battery(v){if(v===254)return 'Charging';if(v===255)return 'Normal';"
    "if(v===0)return 'Low';if(v>=1&&v<=100)return v+'%';return 'Unknown'}"
    "let lastSequence=0,lastTimestamp=0;let controlFormDirty=false;"
    "let requestedFunction=null;"
    "function refreshMeasurement(){json('/api/measurement').then(m=>{"
    "text('function',m.function);text('range',m.range_label+' (code '+m.range+')');"
    "text('battery',battery(m.battery));"
    "if(!m.sample_ready){text('reading','Waiting...');text('reading_unit','');"
    "text('reading_state',m.range_switching?'Adjusting range':'Waiting for a new sample');"
    "text('sample_age','-');text('raw','-');"
    "text('interval','-');lastSequence=0;lastTimestamp=0;return}"
    "text('sample_age',m.sample_age_ms+' ms');"
    "text('raw',m.value_raw+' / unit code '+m.unit_code+' / sequence '+m.sample_sequence);"
    "if(m.sample_sequence!==lastSequence){if(lastTimestamp&&m.timestamp_ms>lastTimestamp)"
    "text('interval',((m.timestamp_ms-lastTimestamp)/1000).toFixed(2)+' s');"
    "lastTimestamp=m.timestamp_ms;lastSequence=m.sample_sequence}"
    "if(m.hold){text('reading_state','Held sample');e('reading_state').className='warn'}"
    "else if(m.sample_age_ms>5000){text('reading_state','Sample is stale');e('reading_state').className='warn'}"
    "else if(m.overrange){text('reading_state','Overrange');e('reading_state').className='warn'}"
    "else if(!m.valid){text('reading_state','Invalid reading');e('reading_state').className='warn'}"
    "else{text('reading_state','Live sample');e('reading_state').className='ok'}"
    "if(m.overrange){text('reading','OL');text('reading_unit','')}"
    "else if(!m.valid){text('reading','—');text('reading_unit','')}"
    "else{text('reading',(m.sign?'-':'')+m.value_raw);"
    "text('reading_unit',({'uV':'µV','mV':'mV','uA':'µA','mOhm':'mΩ','Ohm':'Ω','uW':'µW'})[m.unit]||m.unit)}"
    "}).catch(()=>{text('reading_state','Failed to load measurement');"
    "e('reading_state').className='warn'})}"
    "function controlPost(url,body){return fetch(url,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body||''})"
    ".then(r=>r.json().then(v=>{if(!r.ok)throw Error(v.error||r.status);return v}))}"
    "function setFunction(){let v=e('control_function').value;requestedFunction=v;controlFormDirty=true;controlPost('/api/control/function','function_id='+encodeURIComponent(v))"
    ".then(v=>text('control_result','Function request '+v.request_id+' accepted')).catch(err=>text('control_result','Function failed: '+err.message))}"
    "function toggleHold(){let v=e('hold_button').dataset.enabled==='1'?'0':'1';controlPost('/api/control/hold','enabled='+v)"
    ".then(v=>text('control_result','Hold request '+v.request_id+' accepted')).catch(err=>text('control_result','Hold failed: '+err.message))}"
    "function zeroMeasurement(){if(!confirm('Disconnect external input before zeroing. Continue?'))return;"
    "controlPost('/api/control/zero','').then(v=>text('control_result','Zero request '+v.request_id+' accepted')).catch(err=>text('control_result','Zero failed: '+err.message))}"
    "function markMeasurement(){controlPost('/api/control/mark','').then(v=>text('control_result','Mark request '+v.request_id+' accepted')).catch(err=>text('control_result','Mark failed: '+err.message))}"
    "function refreshControl(){json('/api/control').then(c=>{"
    "if(requestedFunction!==null&&!c.function_pending&&String(c.function_id)===requestedFunction)requestedFunction=null;"
    "if(requestedFunction===null)controlFormDirty=false;"
    "if(!controlFormDirty&&document.activeElement!==e('control_function'))e('control_function').value=c.function_id;"
    "let busy=c.function_pending||c.zero_pending;"
    "e('function_button').disabled=busy;e('zero_button').disabled=c.zero_pending;"
    "e('mark_button').disabled=!c.sample_available;e('control_function').disabled=busy;"
    "let h=e('hold_button');h.dataset.enabled=c.hold?'1':'0';h.textContent=c.hold?'Release hold':'Enable hold';"
    "text('control_state',c.function_pending?'Changing function...':(c.zero_pending?'Zeroing...':(c.last_command_ok?'Ready':'Last command failed')));"
    "e('control_state').className=c.last_command_ok?'ok':'warn'"
    "}).catch(()=>{text('control_state','Failed to load controls');e('control_state').className='warn'})}"
    "function refreshStatus(){json('/api/status').then(s=>{text('ssid',s.ssid);"
    "text('rssi',s.connected?s.rssi+' dBm':'Disconnected');"
    "text('channel',s.connected?s.channel:'-');text('ip',s.ip);text('netmask',s.netmask);"
    "text('gateway',s.gateway);text('mac',s.mac);text('device',s.device_id);"
    "text('firmware',s.firmware);text('heap',s.heap_free+' bytes');"
    "text('uptime',s.uptime_s+' s');text('state',s.connected?'Wi-Fi connected':'Wi-Fi disconnected');"
    "e('state').className=s.connected?'ok':'warn'}).catch(()=>{"
    "text('state','Failed to load status');e('state').className='warn'})}"
    "refreshMeasurement();refreshStatus();refreshControl();setInterval(refreshMeasurement,1000);"
    "setInterval(refreshStatus,3000);setInterval(refreshControl,1000);</script></body></html>";

static size_t json_escape(const char *src, char *dst, size_t dst_size)
{
    size_t used = 0;
    if (dst_size == 0) {
        return 0;
    }
    while (*src != '\0' && used + 1 < dst_size) {
        const char *replacement = NULL;
        char escaped[7];
        switch ((unsigned char)*src) {
            case '"': replacement = "\\\""; break;
            case '\\': replacement = "\\\\"; break;
            case '\n': replacement = "\\n"; break;
            case '\r': replacement = "\\r"; break;
            case '\t': replacement = "\\t"; break;
            default:
                if ((unsigned char)*src < 0x20) {
                    snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned char)*src);
                    replacement = escaped;
                }
                break;
        }
        if (replacement != NULL) {
            size_t replacement_len = strlen(replacement);
            if (used + replacement_len >= dst_size) {
                break;
            }
            memcpy(dst + used, replacement, replacement_len);
            used += replacement_len;
        } else {
            dst[used++] = *src;
        }
        src++;
    }
    dst[used] = '\0';
    return used;
}

static void status_ip_string(const ip4_addr_t *addr, char *out, size_t out_size)
{
    if (out_size == 0) {
        return;
    }
    if (addr == NULL) {
        strlcpy(out, "0.0.0.0", out_size);
        return;
    }
    ip4addr_ntoa_r(addr, out, (int)out_size);
}

static esp_err_t status_api_get_handler(httpd_req_t *req)
{
    wifi_ap_record_t ap_info;
    memset(&ap_info, 0, sizeof(ap_info));
    bool connected = esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK && net_state != 0;

    tcpip_adapter_ip_info_t ip_info;
    memset(&ip_info, 0, sizeof(ip_info));
    bool has_ip = tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &ip_info) == ESP_OK;
    char ip[16] = "0.0.0.0";
    char netmask[16] = "0.0.0.0";
    char gateway[16] = "0.0.0.0";
    if (has_ip) {
        status_ip_string(&ip_info.ip, ip, sizeof(ip));
        status_ip_string(&ip_info.netmask, netmask, sizeof(netmask));
        status_ip_string(&ip_info.gw, gateway, sizeof(gateway));
    }

    uint8_t mac[6] = {0};
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char mac_string[18];
    snprintf(mac_string, sizeof(mac_string), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    const esp_app_desc_t *app = esp_ota_get_app_description();
    char escaped_ssid[100];
    char escaped_device[32];
    char escaped_firmware[64];
    json_escape(wifi_ssid, escaped_ssid, sizeof(escaped_ssid));
    json_escape(device_ID, escaped_device, sizeof(escaped_device));
    json_escape(app != NULL ? app->version : "unknown", escaped_firmware, sizeof(escaped_firmware));

    char *json = calloc(1, 1536);
    if (json == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_ERR_NO_MEM;
    }
    snprintf(json, 1536,
             "{\"connected\":%s,\"ssid\":\"%s\",\"rssi\":%d,"
             "\"channel\":%u,\"ip\":\"%s\",\"netmask\":\"%s\","
             "\"gateway\":\"%s\",\"mac\":\"%s\",\"device_id\":\"%s\","
             "\"firmware\":\"%s\",\"heap_free\":%u,\"uptime_s\":%llu}",
             connected ? "true" : "false", escaped_ssid,
             connected ? (int)ap_info.rssi : -127,
             connected ? (unsigned)ap_info.primary : 0U,
             ip, netmask, gateway, mac_string, escaped_device, escaped_firmware,
             (unsigned)esp_get_free_heap_size(),
             (unsigned long long)(esp_timer_get_time() / 1000000ULL));
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);
    return err;
}

static const char *measurement_function_name(uint8_t function_id)
{
    switch (function_id) {
        case 1: return "DC voltage";
        case 2: return "AC voltage";
        case 3: return "DC current (mA)";
        case 4: return "DC current (A)";
        case 5: return "AC current (mA)";
        case 6: return "AC current (A)";
        case 7: return "Resistance";
        case 8: return "Continuity";
        case 9: return "DC power";
        case 10: return "AC power";
        case 11: return "Diode";
        default: return "Unknown";
    }
}

static const char *measurement_range_name(uint8_t range)
{
    switch (range) {
        case ASW_DCV1: return "DCV1";
        case ASW_DCV2: return "DCV2";
        case ASW_DCV3: return "DCV3";
        case ASW_ACV1: return "ACV1";
        case ASW_ACV2: return "ACV2";
        case ASW_ACV3: return "ACV3";
        case ASW_DCMA: return "DC mA";
        case ASW_DCA: return "DC A";
        case ASW_ACMA: return "AC mA";
        case ASW_ACA: return "AC A";
        case ASW_R2: return "R2";
        case ASW_R3: return "R3";
        case ASW_R4: return "R4";
        case ASW_R5: return "R5";
        case ASW_BEEP: return "Continuity";
        default: return "Unknown";
    }
}

static const char *measurement_unit_name(uint8_t unit_code)
{
    switch (unit_code) {
        case 0x01: return "uV";
        case 0x02: return "mV";
        case 0x05: return "uA";
        case 0x09: return "mOhm";
        case 0x0A: return "Ohm";
        case 0x0C: return "uW";
        default: return "";
    }
}

static esp_err_t measurement_api_get_handler(httpd_req_t *req)
{
    measurement_snapshot_t snapshot = {0};
    bool held = false;
    bool snapshot_present = control_get_display_snapshot(&snapshot, &held);
    bool range_switching = !held && snapshot_present && snapshot.function == current_fun &&
                           snapshot.range != current_sw && current_fun != 9 && current_fun != 10;
    bool sample_ready = held ? snapshot_present :
                        (snapshot_present && snapshot.function == current_fun && !range_switching);
    uint8_t function_id = sample_ready ? snapshot.function : current_fun;
    uint8_t range = sample_ready ? snapshot.range : current_sw;
    uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000ULL;
    uint64_t age_ms = sample_ready && now_ms >= snapshot.timestamp_ms
                          ? now_ms - snapshot.timestamp_ms : 0;
    uint8_t battery = electricity_st;
    char json[512];
    int length = snprintf(json, sizeof(json),
                          "{\"sample_ready\":%s,\"range_switching\":%s,\"valid\":%s,\"overrange\":%s,"
                          "\"function\":\"%s\",\"function_id\":%u,"
                          "\"range\":%u,\"range_label\":\"%s\","
                          "\"value_raw\":%u,\"sign\":%u,"
                          "\"unit_code\":%u,\"unit\":\"%s\","
                          "\"sample_sequence\":%u,\"timestamp_ms\":%llu,"
                          "\"sample_age_ms\":%llu,\"battery\":%u,\"hold\":%s}",
                          sample_ready ? "true" : "false",
                          range_switching ? "true" : "false",
                          sample_ready && snapshot.valid ? "true" : "false",
                          sample_ready && snapshot.overrange ? "true" : "false",
                          measurement_function_name(function_id), (unsigned)function_id,
                          (unsigned)range, measurement_range_name(range),
                          sample_ready ? (unsigned)snapshot.value_raw : 0U,
                          sample_ready ? (unsigned)snapshot.sign : 0U,
                          sample_ready ? (unsigned)snapshot.unit : 0U,
                          sample_ready ? measurement_unit_name(snapshot.unit) : "",
                          sample_ready ? (unsigned)snapshot.sequence : 0U,
                          sample_ready ? (unsigned long long)snapshot.timestamp_ms : 0ULL,
                          (unsigned long long)age_ms, (unsigned)battery,
                          held ? "true" : "false");
    if (length < 0 || (size_t)length >= sizeof(json)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Measurement response is too large");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, length);
}

static esp_err_t control_read_form(httpd_req_t *req, char *body, size_t body_size)
{
    if (req == NULL || body == NULL || body_size == 0 || req->content_len >= body_size) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t received = 0;
    while (received < req->content_len) {
        int count = httpd_req_recv(req, body + received, req->content_len - received);
        if (count <= 0) {
            return ESP_FAIL;
        }
        received += (size_t)count;
    }
    body[received] = '\0';
    return ESP_OK;
}

static esp_err_t control_send_error(httpd_req_t *req, esp_err_t error)
{
    int status_code = 500;
    const char *status_text = "500 Internal Server Error";
    const char *message = "Control request failed";
    if (error == ESP_ERR_INVALID_ARG) {
        status_code = 400;
        status_text = "400 Bad Request";
        message = "Invalid control request";
    } else if (error == ESP_ERR_INVALID_STATE) {
        status_code = 409;
        status_text = "409 Conflict";
        message = "Control is not available in the current state";
    } else if (error == ESP_ERR_TIMEOUT) {
        status_code = 503;
        status_text = "503 Service Unavailable";
        message = "Control queue is full";
    }
    char json[160];
    snprintf(json, sizeof(json), "{\"accepted\":false,\"status\":%d,\"error\":\"%s\"}",
             status_code, message);
    httpd_resp_set_status(req, status_text);
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t control_send_accepted(httpd_req_t *req, uint32_t request_id)
{
    char json[96];
    snprintf(json, sizeof(json), "{\"accepted\":true,\"request_id\":%u}",
             (unsigned)request_id);
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static bool control_function_id_from_name(const char *name, uint8_t *function_id)
{
    if (name == NULL || function_id == NULL) {
        return false;
    }
    struct function_name {
        const char *name;
        uint8_t id;
    };
    static const struct function_name names[] = {
        {"dcv", 1}, {"acv", 2}, {"dcma", 3}, {"dca", 4},
        {"acma", 5}, {"aca", 6}, {"resistance", 7}, {"continuity", 8},
        {"dc_power", 9}, {"ac_power", 10}, {"diode", 11},
    };
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (strcmp(name, names[i].name) == 0) {
            *function_id = names[i].id;
            return true;
        }
    }
    return false;
}

static esp_err_t control_api_get_handler(httpd_req_t *req)
{
    control_state_t state;
    measurement_snapshot_t snapshot = {0};
    bool held = false;
    bool sample_available = control_get_display_snapshot(&snapshot, &held) &&
                            snapshot.timestamp_ms != 0;
    if (!control_get_state(&state)) {
        return control_send_error(req, ESP_ERR_INVALID_STATE);
    }
    char json[768];
    int length = snprintf(json, sizeof(json),
                          "{\"function_id\":%u,\"function\":\"%s\","
                          "\"range\":%u,\"range_label\":\"%s\","
                          "\"hold\":%s,"
                          "\"function_pending\":%s,\"zero_pending\":%s,"
                          "\"last_command_ok\":%s,\"last_mark_sequence\":%u,"
                          "\"request_id\":%u,\"generation\":%u,\"last_error\":%d,"
                          "\"sample_available\":%s}",
                          (unsigned)state.function_id, measurement_function_name(state.function_id),
                          (unsigned)state.range, measurement_range_name(state.range),
                          state.hold ? "true" : "false",
                          state.function_pending ? "true" : "false",
                          state.zero_pending ? "true" : "false",
                          state.last_command_ok ? "true" : "false",
                          (unsigned)state.last_mark_sequence,
                          (unsigned)state.request_id,
                          (unsigned)state.generation,
                          state.last_error,
                          sample_available ? "true" : "false");
    if (length < 0 || (size_t)length >= sizeof(json)) {
        return control_send_error(req, ESP_ERR_NO_MEM);
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, length);
}

static esp_err_t control_function_post_handler(httpd_req_t *req)
{
    char body[128];
    char value[32];
    uint8_t function_id = 0;
    if (control_read_form(req, body, sizeof(body)) != ESP_OK) {
        return control_send_error(req, ESP_ERR_INVALID_ARG);
    }
    if (form_get_value(body, "function_id", value, sizeof(value))) {
        char *end = NULL;
        unsigned long parsed = strtoul(value, &end, 10);
        if (*value == '\0' || end == value || *end != '\0' || parsed > 255) {
            return control_send_error(req, ESP_ERR_INVALID_ARG);
        }
        function_id = (uint8_t)parsed;
    } else if (form_get_value(body, "function", value, sizeof(value))) {
        if (!control_function_id_from_name(value, &function_id)) {
            return control_send_error(req, ESP_ERR_INVALID_ARG);
        }
    } else {
        return control_send_error(req, ESP_ERR_INVALID_ARG);
    }
    uint32_t request_id = 0;
    esp_err_t err = control_submit_function(function_id, &request_id);
    return err == ESP_OK ? control_send_accepted(req, request_id) : control_send_error(req, err);
}

static esp_err_t control_hold_post_handler(httpd_req_t *req)
{
    char body[64];
    char value[8];
    if (control_read_form(req, body, sizeof(body)) != ESP_OK ||
        !form_get_value(body, "enabled", value, sizeof(value)) ||
        (strcmp(value, "0") != 0 && strcmp(value, "1") != 0)) {
        return control_send_error(req, ESP_ERR_INVALID_ARG);
    }
    uint32_t request_id = 0;
    esp_err_t err = control_submit_hold(strcmp(value, "1") == 0, &request_id);
    return err == ESP_OK ? control_send_accepted(req, request_id) : control_send_error(req, err);
}

static esp_err_t control_zero_post_handler(httpd_req_t *req)
{
    uint32_t request_id = 0;
    esp_err_t err = control_submit_zero(&request_id);
    return err == ESP_OK ? control_send_accepted(req, request_id) : control_send_error(req, err);
}

static esp_err_t control_mark_post_handler(httpd_req_t *req)
{
    uint32_t request_id = 0;
    esp_err_t err = control_submit_mark(&request_id);
    return err == ESP_OK ? control_send_accepted(req, request_id) : control_send_error(req, err);
}

static esp_err_t status_page_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, status_page, HTTPD_RESP_USE_STRLEN);
}

static void status_httpd_stop(void)
{
    if (status_httpd != NULL) {
        httpd_stop(status_httpd);
        status_httpd = NULL;
    }
}

static void status_httpd_start(void)
{
    if (status_httpd != NULL) {
        return;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 12;
    if (httpd_start(&status_httpd, &config) != ESP_OK) {
        status_httpd = NULL;
        ESP_LOGE(WIFINET, "failed to start Wi-Fi status web server");
        return;
    }
    static const httpd_uri_t page_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = status_page_get_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t api_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_api_get_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t measurement_uri = {
        .uri = "/api/measurement",
        .method = HTTP_GET,
        .handler = measurement_api_get_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t control_get_uri = {
        .uri = "/api/control",
        .method = HTTP_GET,
        .handler = control_api_get_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t control_function_uri = {
        .uri = "/api/control/function",
        .method = HTTP_POST,
        .handler = control_function_post_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t control_hold_uri = {
        .uri = "/api/control/hold",
        .method = HTTP_POST,
        .handler = control_hold_post_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t control_zero_uri = {
        .uri = "/api/control/zero",
        .method = HTTP_POST,
        .handler = control_zero_post_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t control_mark_uri = {
        .uri = "/api/control/mark",
        .method = HTTP_POST,
        .handler = control_mark_post_handler,
        .user_ctx = NULL,
    };
    esp_err_t page_err = httpd_register_uri_handler(status_httpd, &page_uri);
    esp_err_t api_err = httpd_register_uri_handler(status_httpd, &api_uri);
    esp_err_t measurement_err = httpd_register_uri_handler(status_httpd, &measurement_uri);
    esp_err_t control_get_err = httpd_register_uri_handler(status_httpd, &control_get_uri);
    esp_err_t control_function_err = httpd_register_uri_handler(status_httpd, &control_function_uri);
    esp_err_t control_hold_err = httpd_register_uri_handler(status_httpd, &control_hold_uri);
    esp_err_t control_zero_err = httpd_register_uri_handler(status_httpd, &control_zero_uri);
    esp_err_t control_mark_err = httpd_register_uri_handler(status_httpd, &control_mark_uri);
    if (page_err != ESP_OK || api_err != ESP_OK || measurement_err != ESP_OK ||
        control_get_err != ESP_OK || control_function_err != ESP_OK ||
        control_hold_err != ESP_OK ||
        control_zero_err != ESP_OK || control_mark_err != ESP_OK) {
        ESP_LOGE(WIFINET, "failed to register web routes: page=%s status=%s measurement=%s control_get=%s function=%s hold=%s zero=%s mark=%s",
                 esp_err_to_name(page_err), esp_err_to_name(api_err),
                 esp_err_to_name(measurement_err), esp_err_to_name(control_get_err),
                 esp_err_to_name(control_function_err),
                 esp_err_to_name(control_hold_err), esp_err_to_name(control_zero_err),
                 esp_err_to_name(control_mark_err));
        status_httpd_stop();
        return;
    }
    ESP_LOGI(WIFINET, "Wi-Fi status Web UI ready at http://<device-ip>/");
}

static const char config_page[] =
    "<!doctype html><html lang='en'><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>IOT7 Wi-Fi</title></head><body>"
    "<h2>IOT7 Wi-Fi Settings</h2>"
    "<p>Enter your router's Wi-Fi name and password. The device will connect after saving.</p>"
    "<form method='post' action='/save'>"
    "<label>Wi-Fi name (SSID)<br><input name='ssid' maxlength='31' required></label><br><br>"
    "<label>Wi-Fi password<br><input name='password' type='password' maxlength='63'></label><br><br>"
    "<button type='submit'>Save and connect</button></form>"
    "<p>Device address: 192.168.4.1</p></body></html>";

static esp_err_t config_page_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, config_page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t config_save_post_handler(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len >= 256) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Form is too large");
        return ESP_FAIL;
    }

    char body[256];
    size_t received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to read form");
            return ESP_FAIL;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    char new_ssid[32] = {0};
    char new_password[64] = {0};
    if (!form_get_value(body, "ssid", new_ssid, sizeof(new_ssid)) || new_ssid[0] == '\0' ||
        !form_get_value(body, "password", new_password, sizeof(new_password))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid SSID or password");
        return ESP_FAIL;
    }

    nvs_handle_t nvs_handle = 0;
    esp_err_t err = nvs_open("wificonfig", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, "SSID", new_ssid);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs_handle, "PASSWORD", new_password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    if (nvs_handle) {
        nvs_close(nvs_handle);
    }
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to save settings");
        return err;
    }

    ESP_LOGI(WIFINET, "Web Wi-Fi settings saved: SSID='%s' (%u bytes), password length=%u (value hidden)",
             new_ssid, (unsigned)strlen(new_ssid), (unsigned)strlen(new_password));

    strlcpy(wifi_ssid, new_ssid, sizeof(wifi_ssid));
    strlcpy(wifi_pass, new_password, sizeof(wifi_pass));
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req, "<html lang='en'><meta charset='utf-8'><body><h2>Settings saved</h2>"
                           "<p>The device is connecting to your router's Wi-Fi. Please wait.</p></body></html>");

    if (!config_apply_started) {
        config_apply_started = true;
        if (xTaskCreate(wifi_apply_task, "wifi_apply", 4096, NULL, 5, NULL) != pdPASS) {
            config_apply_started = false;
            ESP_LOGE(WIFINET, "failed to create Wi-Fi apply task");
        }
    }
    return ESP_OK;
}

static void config_httpd_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    if (httpd_start(&config_httpd, &config) != ESP_OK) {
        ESP_LOGE(WIFINET, "failed to start Wi-Fi configuration web server");
        return;
    }

    static const httpd_uri_t get_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = config_page_get_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t save_uri = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = config_save_post_handler,
        .user_ctx = NULL,
    };
    httpd_register_uri_handler(config_httpd, &get_uri);
    httpd_register_uri_handler(config_httpd, &save_uri);
}

static void wifi_apply_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1200));
    if (config_httpd != NULL) {
        httpd_stop(config_httpd);
        config_httpd = NULL;
    }

    wifi_config_t wifi_config;
    bzero(&wifi_config, sizeof(wifi_config));
    strlcpy((char *)wifi_config.sta.ssid, wifi_ssid, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, wifi_pass, sizeof(wifi_config.sta.password));
    ESP_LOGI(WIFINET, "Switching AP to STA: SSID='%s', password length=%u (value hidden)",
             wifi_ssid, (unsigned)strlen(wifi_pass));
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(WIFINET, "esp_wifi_set_mode(STA): %s (0x%x)", esp_err_to_name(err), err);
    ESP_ERROR_CHECK(err);
    err = esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
    ESP_LOGI(WIFINET, "esp_wifi_set_config(STA): %s (0x%x)", esp_err_to_name(err), err);
    ESP_ERROR_CHECK(err);
    wifi_config_ap_active = false;
    net_state = 0;
    wifi_connect_with_log("WEB_SAVE");
    config_apply_started = false;
    vTaskDelete(NULL);
}

static void wifi_config_ap_start_internal(void)
{
    ESP_ERROR_CHECK(wifi_stack_init_once());

    status_httpd_stop();

    if (config_httpd != NULL) {
        httpd_stop(config_httpd);
        config_httpd = NULL;
    }

    wifi_config_t ap_config;
    bzero(&ap_config, sizeof(ap_config));
    char ap_ssid[33] = {0};
    size_t id_len = strlen(device_ID);
    const char *suffix = id_len > 4 ? device_ID + id_len - 4 : device_ID;
    snprintf(ap_ssid, sizeof(ap_ssid), "IOT7-Setup-%s", suffix);
    strlcpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
    strlcpy((char *)ap_config.ap.password, CONFIG_AP_PASSWORD, sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = strlen(ap_ssid);
    ap_config.ap.channel = CONFIG_AP_CHANNEL;
    ap_config.ap.max_connection = CONFIG_AP_MAX_CONN;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_AP, &ap_config));
    if (!wifi_started) {
        ESP_ERROR_CHECK(esp_wifi_start());
        wifi_started = true;
    }
    wifi_config_ap_active = true;
    net_state = 0;
    ESP_LOGI(WIFINET, "Wi-Fi configuration AP: SSID=%s address=http://192.168.4.1/ (password hidden)",
             ap_ssid);
    config_httpd_start();
}

static void wifi_config_ap_start(void)
{
    wifi_config_ap_start_internal();
}

// Initialize Wi-Fi STA
static void app_wifi_initialise(void)
{
    nvs_handle_t wificonfig_get_handle = 0;
    esp_err_t err;
    size_t Len;
    ESP_ERROR_CHECK(wifi_stack_init_once());

    wifi_config_t wifi_config;
    bzero(&wifi_config, sizeof(wifi_config_t));

    err = nvs_open("wificonfig", NVS_READWRITE, &wificonfig_get_handle);
    if (err == ESP_OK) {
        Len = sizeof(wifi_ssid);
        err = nvs_get_str(wificonfig_get_handle, "SSID", (char *)wifi_ssid, &Len);
        if (err == ESP_OK) {
            strlcpy((char *)wifi_config.sta.ssid, wifi_ssid, sizeof(wifi_config.sta.ssid));
            ESP_LOGI(WIFINET, "NVS Wi-Fi SSID='%s' (%u bytes)", wifi_ssid,
                     (unsigned)strlen(wifi_ssid));
        } else {
            ESP_LOGW(WIFINET, "NVS Wi-Fi SSID read: %s (0x%x), required buffer=%u",
                     esp_err_to_name(err), err, (unsigned)Len);
        }
        Len = sizeof(wifi_pass);
        err = nvs_get_str(wificonfig_get_handle, "PASSWORD", (char *)wifi_pass, &Len);
        if (err == ESP_OK) {
            strlcpy((char *)wifi_config.sta.password, wifi_pass, sizeof(wifi_config.sta.password));
            ESP_LOGI(WIFINET, "NVS Wi-Fi password length=%u (value hidden)",
                     (unsigned)strlen(wifi_pass));
            if (strlen(wifi_pass) > 0 && strlen(wifi_pass) < 8) {
                ESP_LOGW(WIFINET, "Stored password is shorter than the WPA/WPA2 8-byte minimum; verify whether the router is open");
            }
        } else {
            ESP_LOGW(WIFINET, "NVS Wi-Fi password read: %s (0x%x), required buffer=%u",
                     esp_err_to_name(err), err, (unsigned)Len);
        }
        nvs_close(wificonfig_get_handle);
    } else {
        ESP_LOGW(WIFINET, "Wi-Fi configuration namespace is not available: %s", esp_err_to_name(err));
    }

    if (wifi_ssid[0] == '\0') {
        ESP_LOGW(WIFINET, "No Wi-Fi SSID is stored; starting configuration AP instead of STA");
        wifi_config_ap_start_internal();
        return;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(WIFINET, "esp_wifi_set_mode(STA): %s (0x%x)", esp_err_to_name(err), err);
    ESP_ERROR_CHECK(err);
    err = esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config);
    ESP_LOGI(WIFINET, "esp_wifi_set_config(STA): %s (0x%x)", esp_err_to_name(err), err);
    ESP_ERROR_CHECK(err);
    if (!wifi_started) {
        err = esp_wifi_start();
        ESP_LOGI(WIFINET, "esp_wifi_start(): %s (0x%x)", esp_err_to_name(err), err);
        ESP_ERROR_CHECK(err);
        wifi_started = true;
    } else {
        wifi_connect_with_log("STA_REINITIALISE");
    }
}

void wifinet_task(void *arg)
{
    uint8_t evt;
    wifinet_evt_queue = xQueueCreate(3, sizeof(evt));
    if (wifinet_evt_queue == NULL) {
        ESP_LOGE(WIFINET, "Failed to create Wi-Fi event queue");
        vTaskDelete(NULL);
        return;
    }

    while (wait == 0) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (wait == 2) {
        wifi_config_ap_start();
    } else {
        app_wifi_initialise();
    }

    while (true) {
        if (xQueueReceive(wifinet_evt_queue, &evt, portMAX_DELAY) == pdTRUE &&
            evt == WIFINET_CONFIG_AP) {
            wifi_config_ap_start();
        }
    }
}

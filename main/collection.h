#ifndef DOER_COLLECTION_H
#define DOER_COLLECTION_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

typedef struct {
    uint32_t session_id;
    uint32_t first_sequence;
    uint32_t last_sequence;
    uint32_t record_count;
    uint32_t first_utc_s;
    uint32_t last_utc_s;
    uint32_t interval_ms;
    uint8_t function_id;
    uint8_t display_unit;
    bool active;
    bool partial;
} collection_session_t;

void collection_init(void);
bool collection_is_active(void);
bool collection_alert_pending(void);
bool collection_network_busy(void);
uint8_t collection_active_function(void);
esp_err_t collection_register_handlers(httpd_handle_t server);
const char *collection_ntp_server(void);
void collection_time_synced(void);
bool collection_time_is_synced(void);

#endif

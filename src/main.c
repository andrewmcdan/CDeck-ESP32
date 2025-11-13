// SPDX-License-Identifier: MIT
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define SUPV_UART_PORT UART_NUM_1
#define SUPV_UART_TXD GPIO_NUM_17
#define SUPV_UART_RXD GPIO_NUM_16
#define SUPV_UART_BAUD 115200
#define SUPV_RX_BUF_SIZE 1024
#define SUPV_LINE_BUF 512

#define TELEMETRY_PERIOD_MS 2000

static const char *TAG = "supervisor";

typedef struct {
    bool lte;
    bool wifi;
    bool bt;
    bool bridge_enable;
    bool lid_open;
    bool charger_online;
} supervisor_switch_state_t;

typedef struct {
    int battery_pct;
    int pack_mv;
    int pack_ma;
    float mcu_temp_c;
    int unread_ext;
    char heltec[16];
    char mcu[16];
    bool poweroff_armed;
    uint64_t last_mesh_event_us;
    supervisor_switch_state_t switches;
} supervisor_state_t;

static supervisor_state_t g_state;
static SemaphoreHandle_t g_state_mutex;

static uint64_t uptime_seconds(void) {
    return esp_timer_get_time() / 1000000ULL;
}

static void supervisor_state_snapshot(supervisor_state_t *out) {
    if (!out) {
        return;
    }
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    *out = g_state;
    xSemaphoreGive(g_state_mutex);
}

static supervisor_switch_state_t supervisor_switch_snapshot(void) {
    supervisor_switch_state_t snapshot;
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    snapshot = g_state.switches;
    xSemaphoreGive(g_state_mutex);
    return snapshot;
}

static void supervisor_state_init(void) {
    g_state_mutex = xSemaphoreCreateMutex();
    if (!g_state_mutex) {
        ESP_LOGE(TAG, "Failed to allocate state mutex");
        abort();
    }
    memset(&g_state, 0, sizeof(g_state));
    g_state.battery_pct = 78;
    g_state.pack_mv = 11750;
    g_state.pack_ma = -420;
    g_state.mcu_temp_c = 36.5f;
    g_state.unread_ext = 0;
    g_state.switches = (supervisor_switch_state_t){
        .lte = true,
        .wifi = false,
        .bt = true,
        .bridge_enable = true,
        .lid_open = false,
        .charger_online = true,
    };
    snprintf(g_state.heltec, sizeof(g_state.heltec), "ok");
    snprintf(g_state.mcu, sizeof(g_state.mcu), "proto-0.1");
    g_state.last_mesh_event_us = esp_timer_get_time();
}

static void supervisor_uart_init(void) {
    const uart_config_t cfg = {
        .baud_rate = SUPV_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    ESP_ERROR_CHECK(uart_param_config(SUPV_UART_PORT, &cfg));
    ESP_ERROR_CHECK(
        uart_set_pin(SUPV_UART_PORT, SUPV_UART_TXD, SUPV_UART_RXD, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(SUPV_UART_PORT, SUPV_RX_BUF_SIZE, 0, 0, NULL, 0));
}

static int compute_last_msg_age(const supervisor_state_t *state, uint64_t now_us) {
    if (!state || state->last_mesh_event_us == 0 || now_us < state->last_mesh_event_us) {
        return 0;
    }
    const uint64_t delta = now_us - state->last_mesh_event_us;
    const uint64_t seconds = delta / 1000000ULL;
    return seconds > (uint64_t)INT_MAX ? INT_MAX : (int)seconds;
}

static cJSON *build_switch_object(const supervisor_switch_state_t *sw) {
    cJSON *obj = cJSON_CreateObject();
    if (!obj || !sw) {
        return obj;
    }
    cJSON_AddBoolToObject(obj, "lte", sw->lte);
    cJSON_AddBoolToObject(obj, "wifi", sw->wifi);
    cJSON_AddBoolToObject(obj, "bt", sw->bt);
    cJSON_AddBoolToObject(obj, "bridge_enable", sw->bridge_enable);
    cJSON_AddBoolToObject(obj, "lid_open", sw->lid_open);
    cJSON_AddBoolToObject(obj, "charger_online", sw->charger_online);
    return obj;
}

static void append_telemetry_fields(cJSON *obj, const supervisor_state_t *state, uint64_t now_us, bool include_switch) {
    if (!obj || !state) {
        return;
    }
    cJSON_AddNumberToObject(obj, "battery_pct", state->battery_pct);
    cJSON_AddNumberToObject(obj, "pack_mv", state->pack_mv);
    cJSON_AddNumberToObject(obj, "pack_ma", state->pack_ma);
    cJSON_AddNumberToObject(obj, "mcu_temp_c", state->mcu_temp_c);
    cJSON_AddNumberToObject(obj, "unread_ext", state->unread_ext);
    cJSON_AddNumberToObject(obj, "last_msg_age_s", compute_last_msg_age(state, now_us));
    cJSON_AddStringToObject(obj, "heltec", state->heltec);
    cJSON_AddStringToObject(obj, "mcu", state->mcu);
    cJSON_AddNumberToObject(obj, "uptime_s", now_us / 1000000ULL);
    if (include_switch) {
        cJSON_AddItemToObject(obj, "switch", build_switch_object(&state->switches));
    }
}

static void send_json_object(cJSON *root) {
    if (!root) {
        return;
    }
    char *payload = cJSON_PrintUnformatted(root);
    if (!payload) {
        ESP_LOGE(TAG, "Failed to encode JSON");
        cJSON_Delete(root);
        return;
    }
    const size_t len = strnlen(payload, SUPV_LINE_BUF * 4);
    if (len > 0) {
        uart_write_bytes(SUPV_UART_PORT, payload, len);
        uart_write_bytes(SUPV_UART_PORT, "\n", 1);
    }
    cJSON_free(payload);
    cJSON_Delete(root);
}

static void send_error_reply(const char *id, const char *error) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    if (id) {
        cJSON_AddStringToObject(root, "id", id);
    }
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", error ? error : "unknown_error");
    send_json_object(root);
}

static void send_basic_ok(const char *id) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    if (id) {
        cJSON_AddStringToObject(root, "id", id);
    }
    cJSON_AddBoolToObject(root, "ok", true);
    send_json_object(root);
}

static void send_status_response(const char *id, const supervisor_state_t *state, uint64_t now_us) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    if (id) {
        cJSON_AddStringToObject(root, "id", id);
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON *status = cJSON_CreateObject();
    if (status) {
        append_telemetry_fields(status, state, now_us, true);
        cJSON_AddItemToObject(root, "status", status);
    }
    send_json_object(root);
}

static void send_switch_response(const char *id, const supervisor_switch_state_t *sw) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    if (id) {
        cJSON_AddStringToObject(root, "id", id);
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddItemToObject(root, "switch", build_switch_object(sw));
    send_json_object(root);
}

static void send_telemetry_event(const supervisor_state_t *state, uint64_t now_us) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    cJSON_AddStringToObject(root, "event", "telemetry");
    append_telemetry_fields(root, state, now_us, true);
    send_json_object(root);
}

static void send_switch_event(const supervisor_switch_state_t *sw) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    cJSON_AddStringToObject(root, "event", "switch");
    cJSON_AddItemToObject(root, "switch", build_switch_object(sw));
    send_json_object(root);
}

static void send_poweroff_reply(const char *id) {
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    if (id) {
        cJSON_AddStringToObject(root, "id", id);
    }
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "poweroff_ok", true);
    send_json_object(root);
}

static void handle_clear_unread(void) {
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_state.unread_ext = 0;
    g_state.last_mesh_event_us = esp_timer_get_time();
    xSemaphoreGive(g_state_mutex);
}

static void handle_arm_poweroff(void) {
    xSemaphoreTake(g_state_mutex, portMAX_DELAY);
    g_state.poweroff_armed = true;
    xSemaphoreGive(g_state_mutex);
}

static void process_command(cJSON *root) {
    if (!root) {
        return;
    }
    cJSON *cmd_item = cJSON_GetObjectItemCaseSensitive(root, "cmd");
    if (!cJSON_IsString(cmd_item)) {
        ESP_LOGW(TAG, "Received JSON without cmd");
        return;
    }
    const char *cmd = cmd_item->valuestring;
    cJSON *id_item = cJSON_GetObjectItemCaseSensitive(root, "id");
    const char *id = cJSON_IsString(id_item) ? id_item->valuestring : NULL;
    supervisor_state_t snapshot;
    uint64_t now_us = esp_timer_get_time();
    if (strcmp(cmd, "get_status") == 0) {
        supervisor_state_snapshot(&snapshot);
        send_status_response(id, &snapshot, now_us);
    } else if (strcmp(cmd, "get_switches") == 0) {
        supervisor_switch_state_t sw = supervisor_switch_snapshot();
        send_switch_response(id, &sw);
    } else if (strcmp(cmd, "clear_unread") == 0) {
        handle_clear_unread();
        send_basic_ok(id);
    } else if (strcmp(cmd, "arm_poweroff") == 0) {
        handle_arm_poweroff();
        send_poweroff_reply(id);
    } else if (strcmp(cmd, "ping") == 0) {
        cJSON *root_reply = cJSON_CreateObject();
        if (root_reply) {
            if (id) {
                cJSON_AddStringToObject(root_reply, "id", id);
            }
            cJSON_AddBoolToObject(root_reply, "ok", true);
            cJSON_AddNumberToObject(root_reply, "uptime_s", uptime_seconds());
            send_json_object(root_reply);
        }
    } else {
        send_error_reply(id, "unknown_cmd");
    }
}

static void process_line(const char *line) {
    if (!line || line[0] == '\0') {
        return;
    }
    cJSON *root = cJSON_Parse(line);
    if (!root) {
        ESP_LOGW(TAG, "Failed to parse JSON: %s", line);
        return;
    }
    if (cJSON_GetObjectItem(root, "cmd")) {
        process_command(root);
    } else {
        ESP_LOGI(TAG, "Ignoring JSON without cmd field");
    }
    cJSON_Delete(root);
}

static void uart_reader_task(void *arg) {
    (void)arg;
    uint8_t byte;
    char line[SUPV_LINE_BUF];
    size_t len = 0;
    while (true) {
        const int read = uart_read_bytes(SUPV_UART_PORT, &byte, 1, pdMS_TO_TICKS(100));
        if (read <= 0) {
            continue;
        }
        if (byte == '\r') {
            continue;
        }
        if (byte == '\n') {
            line[len] = '\0';
            if (len > 0) {
                process_line(line);
            }
            len = 0;
            continue;
        }
        if (len + 1 >= sizeof(line)) {
            ESP_LOGW(TAG, "UART line overflow, dropping");
            len = 0;
            continue;
        }
        line[len++] = (char)byte;
    }
}

static void telemetry_task(void *arg) {
    (void)arg;
    while (true) {
        supervisor_state_t snapshot;
        supervisor_state_snapshot(&snapshot);
        const uint64_t now_us = esp_timer_get_time();
        send_telemetry_event(&snapshot, now_us);
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_PERIOD_MS));
    }
}

void app_main(void) {
    supervisor_state_init();
    supervisor_uart_init();
    xTaskCreate(uart_reader_task, "uart_reader", 4096, NULL, 10, NULL);
    xTaskCreate(telemetry_task, "telemetry", 4096, NULL, 5, NULL);
    supervisor_switch_state_t sw = supervisor_switch_snapshot();
    send_switch_event(&sw);
}

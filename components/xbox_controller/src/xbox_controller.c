/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Xbox Series X/S BLE Controller Driver Implementation for ESP32-S3
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"

// NimBLE 协议栈
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

// ESP HID Host 组件
#include "esp_hidh.h"
#include "esp_hid_gap.h"
#include "esp_private/esp_hidh_private.h"
#include "xbox_controller.h"

#define TAG "XBOX_CTRL"

#define ESP_BD_ADDR_STR         "%02X:%02X:%02X:%02X:%02X:%02X"
// NimBLE 内部地址为小端序 (addr[0] 为最低位)，大端打印供人阅读
#define ESP_BD_ADDR_HEX(addr)   (addr)[5], (addr)[4], (addr)[3], (addr)[2], (addr)[1], (addr)[0]

void ble_store_config_init(void);

// 内部状态
static xbox_controller_config_t s_config = {0};
static xbox_controller_data_t   s_current_data = {0};
static esp_hidh_dev_t          *s_connected_dev = NULL;
static uint16_t                 s_rumble_handle = 0;
static bool                     s_is_connected = false;
static TaskHandle_t             s_scan_task_handle = NULL;
static uint8_t                  s_target_bda[6] = {0}; // 存为小端序以匹配 NimBLE
static bool                     s_has_target_mac = false;

// 辅助函数：将人类可读的大端 MAC 字符串解析为 NimBLE 小端 6 字节数组
static bool parse_mac_string(const char *mac_str, uint8_t bda[6])
{
    if (!mac_str || strlen(mac_str) < 17) {
        return false;
    }
    int values[6];
    if (sscanf(mac_str, "%x:%x:%x:%x:%x:%x",
               &values[0], &values[1], &values[2],
               &values[3], &values[4], &values[5]) == 6) {
        for (int i = 0; i < 6; i++) {
            bda[5 - i] = (uint8_t)values[i];
        }
        return true;
    }
    return false;
}

// 辅助函数：检查设备名称是否为 Xbox 手柄
static bool is_xbox_controller_name(const char *name)
{
    if (!name) return false;
    char lower[64] = {0};
    size_t len = strlen(name);
    if (len >= sizeof(lower)) len = sizeof(lower) - 1;
    for (size_t i = 0; i < len; i++) {
        lower[i] = (char)tolower((unsigned char)name[i]);
    }
    return (strstr(lower, "xbox") != NULL);
}

// ==========================================
// Xbox Series X/S HID 报文解码器
// ==========================================
static void parse_xbox_report(const uint8_t *data, size_t length)
{
    if (!data || length < 15) {
        return;
    }

    const uint8_t *report = data;
    size_t report_len = length;

    // 只有当长度为 17 字节且首字节为 0x01 时，才做 1 字节偏移
    if (length >= 17 && data[0] == 0x01) {
        report = &data[1];
        report_len = length - 1;
    }

    if (report_len < 15) {
        return;
    }

    // 1. 模拟摇杆坐标 (16 位无符号整数, 范围 0 ~ 65535, 居中约为 32768)
    s_current_data.joy_left_x  = (uint16_t)(report[0] | (report[1] << 8));
    s_current_data.joy_left_y  = (uint16_t)(report[2] | (report[3] << 8));
    s_current_data.joy_right_x = (uint16_t)(report[4] | (report[5] << 8));
    s_current_data.joy_right_y = (uint16_t)(report[6] | (report[7] << 8));

    // 2. 模拟线性扳机 (10 位精度, 范围 0 ~ 1023)
    s_current_data.trigger_l = (uint16_t)(report[8]  | ((report[9]  & 0x03) << 8));
    s_current_data.trigger_r = (uint16_t)(report[10] | ((report[11] & 0x03) << 8));

    // 3. 十字方向键 (D-Pad Hat Switch, 位于 report[12] 的低 4 位)
    // 0: 居中无按压, 1: 上, 2: 右上, 3: 右, 4: 右下, 5: 下, 6: 左下, 7: 左, 8: 左上
    uint8_t dpad = report[12] & 0x0F;
    s_current_data.dpad_up    = (dpad == 1 || dpad == 2 || dpad == 8);
    s_current_data.dpad_right = (dpad == 2 || dpad == 3 || dpad == 4);
    s_current_data.dpad_down  = (dpad == 4 || dpad == 5 || dpad == 6);
    s_current_data.dpad_left  = (dpad == 6 || dpad == 7 || dpad == 8);

    // 4. 主要动作按键与肩键 (report[13])
    // bit0: A, bit1: B, bit3: X, bit4: Y, bit6: LB, bit7: RB
    uint8_t btn_byte1 = report[13];
    s_current_data.btn_a  = (btn_byte1 & (1 << 0)) != 0;
    s_current_data.btn_b  = (btn_byte1 & (1 << 1)) != 0;
    s_current_data.btn_x  = (btn_byte1 & (1 << 3)) != 0;
    s_current_data.btn_y  = (btn_byte1 & (1 << 4)) != 0;
    s_current_data.btn_lb = (btn_byte1 & (1 << 6)) != 0;
    s_current_data.btn_rb = (btn_byte1 & (1 << 7)) != 0;

    // 5. 系统功能键与摇杆下按 (report[14])
    // bit2: View(窗口/Back), bit3: Menu(三杠/Start), bit4: Xbox(Guide), bit5: LSB, bit6: RSB
    uint8_t btn_byte2 = report[14];
    s_current_data.btn_view = (btn_byte2 & (1 << 2)) != 0;
    s_current_data.btn_menu = (btn_byte2 & (1 << 3)) != 0;
    s_current_data.btn_xbox = (btn_byte2 & (1 << 4)) != 0;
    s_current_data.btn_lsb  = (btn_byte2 & (1 << 5)) != 0;
    s_current_data.btn_rsb  = (btn_byte2 & (1 << 6)) != 0;

    // 6. 分享键 (Series 专有, report[15] 的 bit0)
    if (report_len >= 16) {
        s_current_data.btn_share = (report[15] & (1 << 0)) != 0;
    } else {
        s_current_data.btn_share = false;
    }

    // 分发回调
    if (s_config.event_cb) {
        s_config.event_cb(XBOX_EVENT_DATA, &s_current_data, s_config.user_arg);
    }
}

// ==========================================
// HID Host 底层事件回调
// ==========================================
static void hidh_callback(void *handler_args, esp_event_base_t base, int32_t id, void *event_data)
{
    esp_hidh_event_t event = (esp_hidh_event_t)id;
    esp_hidh_event_data_t *param = (esp_hidh_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDH_OPEN_EVENT: {
        if (param->open.status == ESP_OK) {
            s_is_connected = true;
            s_connected_dev = param->open.dev;

            const uint8_t *bda = esp_hidh_dev_bda_get(param->open.dev);
            const char *dev_name = esp_hidh_dev_name_get(param->open.dev);

            ESP_LOGI(TAG, "==================================================");
            ESP_LOGI(TAG, "  成功连接 Xbox 手柄: %s (" ESP_BD_ADDR_STR ")",
                     dev_name ? dev_name : "Xbox Controller", ESP_BD_ADDR_HEX(bda));
            ESP_LOGI(TAG, "  安全加密绑定完成，HID 输入流已就绪！");
            ESP_LOGI(TAG, "==================================================");

            // 关键：将该设备所有 HID 映射的协议模式设为 REPORT 模式 (1)
            // 避免因手柄无 Protocol Mode 特征值时被默认当成 BOOT 模式而导致通知被丢弃
            if (param->open.dev && param->open.dev->protocol_mode) {
                for (size_t i = 0; i < param->open.dev->config.report_maps_len; i++) {
                    param->open.dev->protocol_mode[i] = ESP_HID_PROTOCOL_MODE_REPORT;
                }
                ESP_LOGI(TAG, "已成功激活手柄 REPORT 协议模式！");
            }

            // 获取并缓存马达控制 Report (ID: 0x03, OUTPUT) 的 GATT 特征值句柄
            esp_hidh_dev_report_t *rumble_rep = esp_hidh_dev_get_report_by_id_and_type(
                param->open.dev, 0, 0x03, ESP_HID_REPORT_TYPE_OUTPUT);
            if (rumble_rep) {
                s_rumble_handle = rumble_rep->handle;
                ESP_LOGI(TAG, "已获取马达控制特征值 Handle: %u (ID: %u)", s_rumble_handle, rumble_rep->report_id);
            } else {
                s_rumble_handle = 34; // Xbox Series 标准 Handle
                ESP_LOGW(TAG, "未在报文映射中匹配到马达特征值，使用默认 Handle: 34");
            }

            // 仅在调试模式下打印完整 Report 映射，避免 UART 慢速输出打塞事件循环
            if (s_config.debug_raw_reports) {
                esp_hidh_dev_dump(param->open.dev, stdout);
            }

            if (s_config.event_cb) {
                s_config.event_cb(XBOX_EVENT_CONNECTED, &s_current_data, s_config.user_arg);
            }
        } else {
            ESP_LOGE(TAG, "连接 Xbox 手柄失败 (status=%d)", param->open.status);
            s_is_connected = false;
            s_connected_dev = NULL;
            s_rumble_handle = 0;
        }
        break;
    }

    case ESP_HIDH_INPUT_EVENT: {
        parse_xbox_report(param->input.data, param->input.length);
        break;
    }

    case ESP_HIDH_BATTERY_EVENT: {
        s_current_data.battery_level = param->battery.level;
        ESP_LOGI(TAG, "Xbox 手柄当前电量: %d%%", param->battery.level);
        if (s_config.event_cb) {
            s_config.event_cb(XBOX_EVENT_BATTERY, &s_current_data, s_config.user_arg);
        }
        break;
    }

    case ESP_HIDH_CLOSE_EVENT: {
        ESP_LOGW(TAG, "Xbox 手柄连接断开！准备自动重新搜索回连...");
        s_is_connected = false;
        s_connected_dev = NULL;
        s_rumble_handle = 0;
        memset(&s_current_data, 0, sizeof(s_current_data));

        if (s_config.event_cb) {
            s_config.event_cb(XBOX_EVENT_DISCONNECTED, NULL, s_config.user_arg);
        }

        // 唤醒自动扫描任务
        if (s_scan_task_handle) {
            xTaskNotifyGive(s_scan_task_handle);
        }
        break;
    }

    default:
        break;
    }
}

// ==========================================
// 自动扫描与连接任务
// ==========================================
static void xbox_scan_and_connect_task(void *pvParameters)
{
    while (1) {
        if (s_is_connected) {
            // 已连接，挂起任务等待断开通知
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }

        ESP_LOGI(TAG, "--------------------------------------------------");
        ESP_LOGI(TAG, "正在扫描周围的 Xbox 手柄 (持续3秒)...");
        ESP_LOGI(TAG, "提示：首次连接请【长按手柄顶部配对小按钮】直到西瓜灯快闪！");
        if (s_has_target_mac) {
            ESP_LOGI(TAG, "目标过滤 MAC: " ESP_BD_ADDR_STR, ESP_BD_ADDR_HEX(s_target_bda));
        }
        ESP_LOGI(TAG, "--------------------------------------------------");

        size_t results_len = 0;
        esp_hid_scan_result_t *results = NULL;

        esp_hid_scan(3, &results_len, &results);

        ESP_LOGI(TAG, "扫描结束，发现 %u 个 HID 广播设备", results_len);

        esp_hid_scan_result_t *target = NULL;
        if (results_len > 0) {
            esp_hid_scan_result_t *r = results;
            while (r) {
                const char *name = r->name ? r->name : "Unknown";
                printf("  [扫描发现] %-25s | RSSI: %3d | MAC: " ESP_BD_ADDR_STR " | 类型: %s\r\n",
                       name, r->rssi, ESP_BD_ADDR_HEX(r->bda), esp_hid_usage_str(r->usage));

                // 1. 如果指定了固定 MAC 地址，进行强匹配
                if (s_has_target_mac) {
                    if (memcmp(r->bda, s_target_bda, 6) == 0) {
                        target = r;
                        break;
                    }
                } else {
                    // 2. 自动匹配模式：名称含 Xbox 或 类型为 Gamepad/Joystick
                    if (is_xbox_controller_name(r->name)) {
                        target = r;
                        break;
                    } else if (r->usage == ESP_HID_USAGE_GAMEPAD || r->usage == ESP_HID_USAGE_JOYSTICK) {
                        if (!target) target = r; // 作为候选
                    }
                }
                r = r->next;
            }

            if (target) {
                ESP_LOGI(TAG, ">>> 正在发起连接目标手柄: %s (" ESP_BD_ADDR_STR ")...",
                         target->name ? target->name : "Xbox Controller",
                         ESP_BD_ADDR_HEX(target->bda));

                esp_hidh_dev_open(target->bda, target->transport, target->ble.addr_type);

                // 等待连接并完成安全握手 (最多等待 6 秒)
                for (int i = 0; i < 30; i++) {
                    vTaskDelay(pdMS_TO_TICKS(200));
                    if (s_is_connected) {
                        break;
                    }
                }
            }

            esp_hid_scan_results_free(results);
        }

        if (!s_is_connected) {
            ESP_LOGW(TAG, "未完成连接，3 秒后自动进行下一轮扫描...");
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
}

// ==========================================
// NimBLE 主机线程
// ==========================================
static void ble_hid_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE 蓝牙主机协议栈运行中...");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ==========================================
// 外部调用 API 实现
// ==========================================
esp_err_t xbox_controller_init(const xbox_controller_config_t *config)
{
    if (config) {
        s_config = *config;
        if (config->target_mac && strlen(config->target_mac) > 0) {
            s_has_target_mac = parse_mac_string(config->target_mac, s_target_bda);
            if (!s_has_target_mac) {
                ESP_LOGW(TAG, "指定的 target_mac 格式非法，已降级为名称自动搜索模式！");
            }
        }
    }

    // 1. 初始化 NVS 闪存 (存储蓝牙绑定密钥必须)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. 初始化 GAP
    ESP_ERROR_CHECK(esp_hid_gap_init(HIDH_BLE_MODE));

    // 3. 初始化 HID Host 驱动
    // 临时提升当前任务优先级，确保 esp_hidh_init 创建的 esp_hidh_events 任务优先级高于 NimBLE 线程 (configMAX_PRIORITIES - 4)，
    // 使得手柄以 125~250Hz 上报按键数据时，事件队列能够被即刻消费，绝不堆积溢出
    UBaseType_t orig_prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, configMAX_PRIORITIES - 3);

    esp_hidh_config_t hidh_cfg = {
        .callback = hidh_callback,
        .event_stack_size = 4096,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(esp_hidh_init(&hidh_cfg));

    vTaskPrioritySet(NULL, orig_prio);

    // 4. 配置 NimBLE 安全绑定 (SMP Bonding - Xbox 手柄连接的关键)
    ble_store_config_init();
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO; // Just Works 配对
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    // 5. 启动 NimBLE 线程
    nimble_port_freertos_init(ble_hid_host_task);

    vTaskDelay(pdMS_TO_TICKS(200));

    // 6. 创建扫描与自动重连任务
    BaseType_t task_ret = xTaskCreate(
        &xbox_scan_and_connect_task,
        "xbox_scan_task",
        5 * 1024,
        NULL,
        3,
        &s_scan_task_handle
    );

    return (task_ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

bool xbox_controller_is_connected(void)
{
    return s_is_connected;
}

esp_err_t xbox_controller_get_data(xbox_controller_data_t *out_data)
{
    if (!out_data) return ESP_ERR_INVALID_ARG;
    *out_data = s_current_data;
    return ESP_OK;
}

esp_err_t xbox_controller_set_rumble(uint8_t strong_rumble,
                                     uint8_t weak_rumble,
                                     uint8_t left_trigger,
                                     uint8_t right_trigger,
                                     uint8_t duration_10ms)
{
    if (!s_is_connected || !s_connected_dev || s_rumble_handle == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    // 限制范围在 0 ~ 100
    if (strong_rumble > 100) strong_rumble = 100;
    if (weak_rumble   > 100) weak_rumble   = 100;
    if (left_trigger  > 100) left_trigger  = 100;
    if (right_trigger > 100) right_trigger = 100;

    // Xbox Series BLE 标准 Haptics Output Report 格式 (Report ID 0x03, 8 字节)
    uint8_t rumble_buf[8];
    rumble_buf[0] = 0x0F;            // 马达使能掩码 (全部 4 个马达使能)
    rumble_buf[1] = right_trigger;   // 右扳机马达
    rumble_buf[2] = left_trigger;    // 左扳机马达
    rumble_buf[3] = weak_rumble;     // 右握把高频弱震
    rumble_buf[4] = strong_rumble;   // 左握把低频强震
    rumble_buf[5] = duration_10ms;   // 持续时间 (例如 50 = 500ms, 0xFF = 持续)
    rumble_buf[6] = 0x00;            // 延迟
    rumble_buf[7] = 0x00;            // 重复计数

    // 关键优化：使用非阻塞 GATT Write Without Response (Write Command)
    // 0 延迟、无需等待 ATT_WRITE_RSP、绝不占用事件循环信号量，彻底规避死锁与 ACL 缓存耗尽
    int rc = ble_gattc_write_no_rsp_flat(s_connected_dev->ble.conn_id, s_rumble_handle, rumble_buf, sizeof(rumble_buf));
    return (rc == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t xbox_controller_stop_rumble(void)
{
    return xbox_controller_set_rumble(0, 0, 0, 0, 0);
}

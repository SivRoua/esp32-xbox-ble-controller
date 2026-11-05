/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * ESP32-S3 BLE Xbox Series Controller Host Demo
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "xbox_controller.h"

#define TAG "APP_MAIN"

// 板载 LED 指示灯 (低电平点亮，GPIO 48)
#define LED_GPIO       48
#define LED_ON_LEVEL   0
#define LED_OFF_LEVEL  1

static void led_init(void)
{
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, LED_OFF_LEVEL);
}

static void led_set(bool on)
{
    gpio_set_level(LED_GPIO, on ? LED_ON_LEVEL : LED_OFF_LEVEL);
}

// 缓存上一次按键状态以实现按下边沿触发判定
static xbox_controller_data_t s_last_data = {0};
static int64_t s_last_analog_print_time = 0;

// 事件与数据回调函数
static void xbox_event_handler(xbox_controller_event_t event, const xbox_controller_data_t *data, void *user_arg)
{
    switch (event) {
    case XBOX_EVENT_CONNECTED:
        ESP_LOGI(TAG, ">>> [手柄已连接] 西瓜灯稳定，所有按键与马达已就绪！");
        led_set(true); // 连上手柄常亮
        // 连接成功后震动欢迎一下 (双握把轻振 300ms)
        xbox_controller_set_rumble(30, 30, 0, 0, 30);
        break;

    case XBOX_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, ">>> [手柄已断开] 正在等待重新连接...");
        led_set(false);
        memset(&s_last_data, 0, sizeof(s_last_data));
        break;

    case XBOX_EVENT_BATTERY:
        if (data) {
            ESP_LOGI(TAG, ">>> [手柄电量报告] 当前电量: %d%%", data->battery_level);
        }
        break;

    case XBOX_EVENT_DATA: {
        if (!data) return;

        // 1. 动作按键按下沿检测 (从松开变为按下)
        if (data->btn_a && !s_last_data.btn_a) {
            ESP_LOGI(TAG, "[按键] >>> A 键按下 (触发左握把低频强震)");
            xbox_controller_set_rumble(70, 0, 0, 0, 15);
        }
        if (data->btn_b && !s_last_data.btn_b) {
            ESP_LOGI(TAG, "[按键] >>> B 键按下 (触发右握把高频弱震)");
            xbox_controller_set_rumble(0, 70, 0, 0, 15);
        }
        if (data->btn_x && !s_last_data.btn_x) {
            ESP_LOGI(TAG, "[按键] >>> X 键按下 (触发左扳机微马达震动)");
            xbox_controller_set_rumble(0, 0, 80, 0, 15);
        }
        if (data->btn_y && !s_last_data.btn_y) {
            ESP_LOGI(TAG, "[按键] >>> Y 键按下 (触发右扳机微马达震动)");
            xbox_controller_set_rumble(0, 0, 0, 80, 15);
        }

        if (data->btn_lb && !s_last_data.btn_lb) ESP_LOGI(TAG, "[按键] >>> LB 键按下");
        if (data->btn_rb && !s_last_data.btn_rb) ESP_LOGI(TAG, "[按键] >>> RB 键按下");
        if (data->btn_view && !s_last_data.btn_view) ESP_LOGI(TAG, "[按键] >>> View (窗口/Back) 键按下");
        if (data->btn_menu && !s_last_data.btn_menu) ESP_LOGI(TAG, "[按键] >>> Menu (三杠/Start) 键按下");
        if (data->btn_xbox && !s_last_data.btn_xbox) ESP_LOGI(TAG, "[按键] >>> Xbox (西瓜徽标) 键按下");
        if (data->btn_share && !s_last_data.btn_share) ESP_LOGI(TAG, "[按键] >>> Share (截图分享) 键按下");
        if (data->btn_lsb && !s_last_data.btn_lsb) ESP_LOGI(TAG, "[按键] >>> 左摇杆下按 (LSB)");
        if (data->btn_rsb && !s_last_data.btn_rsb) ESP_LOGI(TAG, "[按键] >>> 右摇杆下按 (RSB)");

        // 2. 十字方向键 (D-Pad)
        if (data->dpad_up    && !s_last_data.dpad_up)    ESP_LOGI(TAG, "[D-Pad] >>> 上 (Up)");
        if (data->dpad_down  && !s_last_data.dpad_down)  ESP_LOGI(TAG, "[D-Pad] >>> 下 (Down)");
        if (data->dpad_left  && !s_last_data.dpad_left)  ESP_LOGI(TAG, "[D-Pad] >>> 左 (Left)");
        if (data->dpad_right && !s_last_data.dpad_right) ESP_LOGI(TAG, "[D-Pad] >>> 右 (Right)");

        // 3. 模拟摇杆与线性扳机节流打印 (每 100ms 最多输出一次，避免高频打满串口)
        int64_t now_us = esp_timer_get_time();
        if ((now_us - s_last_analog_print_time) > 100000) { // 100ms
            bool joy_moved = (abs((int)data->joy_left_x - 32768) > 3000) ||
                             (abs((int)data->joy_left_y - 32768) > 3000) ||
                             (abs((int)data->joy_right_x - 32768) > 3000) ||
                             (abs((int)data->joy_right_y - 32768) > 3000) ||
                             (data->trigger_l > 50) || (data->trigger_r > 50);

            if (joy_moved) {
                printf("[模拟量] 左摇杆:(%5u, %5u) | 右摇杆:(%5u, %5u) | 扳机 LT:%4u, RT:%4u\r\n",
                       data->joy_left_x, data->joy_left_y,
                       data->joy_right_x, data->joy_right_y,
                       data->trigger_l, data->trigger_r);
                s_last_analog_print_time = now_us;
            }
        }

        s_last_data = *data;
        break;
    }

    default:
        break;
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "=================================================");
    ESP_LOGI(TAG, "   ESP32-S3 BLE Xbox Series 控制器主机固件启动   ");
    ESP_LOGI(TAG, "=================================================");

    led_init();

    // 手柄配置
    xbox_controller_config_t cfg = {
        .target_mac = NULL,             // 设为 NULL 时自动扫描连接名称含 "Xbox" 的设备；也可填入 "F4:6A:D7:A0:5C:59"
        .event_cb = xbox_event_handler,
        .user_arg = NULL,
        .debug_raw_reports = false,     // 如需分析原始 16 进制报文可设为 true
    };

    ESP_ERROR_CHECK(xbox_controller_init(&cfg));

    ESP_LOGI(TAG, "驱动已就绪。首次连接请【长按手柄顶部微型配对键】进入配对模式！");

    // 主循环保持存活
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

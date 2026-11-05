/*
 * SPDX-FileCopyrightText: 2026
 * SPDX-License-Identifier: Apache-2.0
 *
 * Xbox Series X/S BLE Controller Driver for ESP32-S3 (ESP-IDF NimBLE)
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Xbox 手柄所有按键及摇杆数据状态结构体
 */
typedef struct {
    // 动作按键 (true: 按下, false: 松开)
    bool btn_a;
    bool btn_b;
    bool btn_x;
    bool btn_y;

    // 顶部肩键
    bool btn_lb;
    bool btn_rb;

    // 功能与系统按键
    bool btn_view;      // 窗口视图键 (Back / Change View)
    bool btn_menu;      // 菜单键 (Start / Menu)
    bool btn_xbox;      // 中间西瓜徽标键 (Guide)
    bool btn_share;     // 录制/截图分享键 (Series 专有)

    // 摇杆按下按键 (L3 / R3)
    bool btn_lsb;       // 左摇杆下按 (Left Stick Button)
    bool btn_rsb;       // 右摇杆下按 (Right Stick Button)

    // 十字方向键 (D-Pad)
    bool dpad_up;
    bool dpad_down;
    bool dpad_left;
    bool dpad_right;

    // 模拟摇杆坐标 (范围: 0 ~ 65535, 居中中位值约为 32768)
    uint16_t joy_left_x;
    uint16_t joy_left_y;
    uint16_t joy_right_x;
    uint16_t joy_right_y;

    // 模拟线性扳机行程 (范围: 0 ~ 1023, 0 为完全松开, 1023 为按到底)
    uint16_t trigger_l;
    uint16_t trigger_r;

    // 电池电量百分比 (0 ~ 100)
    uint8_t battery_level;
} xbox_controller_data_t;

/**
 * @brief Xbox 控制器事件类型
 */
typedef enum {
    XBOX_EVENT_CONNECTED = 0,   // 手柄已连接并成功绑定
    XBOX_EVENT_DISCONNECTED,    // 手柄连接断开
    XBOX_EVENT_DATA,            // 手柄按键或摇杆数据更新
    XBOX_EVENT_BATTERY,         // 手柄电量更新
} xbox_controller_event_t;

/**
 * @brief 事件回调函数原型
 * @param event 事件类型
 * @param data 手柄最新数据指针 (仅在 XBOX_EVENT_DATA 时有效)
 * @param user_arg 用户传递的私有指针
 */
typedef void (*xbox_controller_event_cb_t)(xbox_controller_event_t event, const xbox_controller_data_t *data, void *user_arg);

/**
 * @brief 驱动配置结构体
 */
typedef struct {
    /**
     * @brief 目标 Xbox 手柄的蓝牙 MAC 地址字符串 (例如 "F4:6A:D7:A0:5C:59")
     *        如果设为 NULL，则开启自动扫描模式，自动连接广播名称包含 "Xbox" 的设备。
     */
    const char *target_mac;

    /**
     * @brief 事件回调函数
     */
    xbox_controller_event_cb_t event_cb;

    /**
     * @brief 用户自定义上下文参数
     */
    void *user_arg;

    /**
     * @brief 是否开启原始 HID 报文 Hex dump 打印 (用于调试)
     */
    bool debug_raw_reports;
} xbox_controller_config_t;

/**
 * @brief 初始化 Xbox 手柄蓝牙主机驱动并启动扫描/连接任务
 *
 * @param config 配置项指针
 * @return esp_err_t ESP_OK 成功，其他为错误码
 */
esp_err_t xbox_controller_init(const xbox_controller_config_t *config);

/**
 * @brief 检查手柄当前是否处于已连接状态
 *
 * @return true 已连接, false 未连接
 */
bool xbox_controller_is_connected(void);

/**
 * @brief 获取手柄最新的按键与摇杆数据镜像
 *
 * @param out_data 输出数据结构体指针
 * @return esp_err_t ESP_OK 获取成功
 */
esp_err_t xbox_controller_get_data(xbox_controller_data_t *out_data);

/**
 * @brief 控制 Xbox 手柄马达震动 (Haptic Rumble)
 *
 * @param strong_rumble 左握把强震马达力度 (0 ~ 100)
 * @param weak_rumble   右握把高频弱震马达力度 (0 ~ 100)
 * @param left_trigger  左扳机独立微型马达力度 (0 ~ 100)
 * @param right_trigger 右扳机独立微型马达力度 (0 ~ 100)
 * @param duration_10ms 震动持续时间，以 10ms 为单位 (例如 50 代表 500ms; 0xFF 代表持续震动)
 * @return esp_err_t ESP_OK 发送成功，ESP_ERR_INVALID_STATE 未连接
 */
esp_err_t xbox_controller_set_rumble(uint8_t strong_rumble,
                                     uint8_t weak_rumble,
                                     uint8_t left_trigger,
                                     uint8_t right_trigger,
                                     uint8_t duration_10ms);

/**
 * @brief 立即停止所有马达震动
 *
 * @return esp_err_t ESP_OK
 */
esp_err_t xbox_controller_stop_rumble(void);

#ifdef __cplusplus
}
#endif

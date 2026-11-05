# ESP32-S3 Xbox Series BLE Controller Host Driver

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.0%20%7C%20v5.1%20%7C%20v5.2%20%7C%20v6.0+-blue.svg)](https://github.com/espressif/esp-idf)
[![Target](https://img.shields.io/badge/Target-ESP32--S3%20%2F%20ESP32-green.svg)](https://www.espressif.com/)
[![License](https://img.shields.io/badge/License-Apache%202.0-orange.svg)](LICENSE)

基于 **ESP-IDF 原生 Apache NimBLE 蓝牙主机协议栈** 实现的 Xbox Series X/S 无线手柄（BLE HID Host）驱动组件。

无需任何 Arduino 依赖，纯 C 语言模块化设计，完美支持 Xbox Series 原装手柄的全部按键、16 位摇杆、10 位线性扳机、电量读取以及四马达触觉反馈（握把+扳机马达震动）。

---

## 🌟 核心特性与技术亮点

许多开发者在 ESP32 上尝试使用原生 ESP-IDF 连接 Xbox Series 手柄时，常会遇到“无法配对”、“连接后按键无反应”或“连接数秒后报 `MBUF alloc stuck` 崩溃断开”等严重问题。本项目彻底攻克了这些底层痛点：

1. **解决 Protocol Mode 丢包陷阱**：
   - Xbox Series 手柄未暴露标准 HID `Protocol Mode` 特征值；ESP-IDF 原生 `nimble_hidh` 默认将其初始化为 `BOOT(0)` 模式，导致所有按键通知直接被底层静默丢弃（`Wrong protocol mode, dropping notification`）。
   - 本驱动在握手建立后主动强制重置 HID 映射的协议模式为 `REPORT(1)`，确保 100% 接收原始按键流。
2. **规避 250Hz 高频上报引发的死锁与 ACL 缓存耗尽**：
   - Xbox 手柄以 125Hz~250Hz 的极高频率持续上报摇杆与按键，而 ESP-IDF 的 `esp_hidh_events` 队列深度仅有 5 个。
   - 本项目将震动输出重构为**非阻塞的 GATT Write Without Response (`ble_gattc_write_no_rsp_flat`)**，0 延迟、无需等待 ACK；同时优化事件循环优先级，彻底杜绝任务死锁与 `ACL buf alloc failed / MBUF alloc stuck` 错误。
3. **全按键与模拟量完整解码**：
   - **动作键**：A、B、X、Y
   - **方向键**：十字 D-Pad（上、下、左、右及对角线判定）
   - **肩键与功能键**：LB、RB、View (窗口)、Menu (三杠)、Xbox (西瓜徽标)、Share (截图分享键)
   - **摇杆与下按键**：双 16 位高精度模拟摇杆（居中约 32768，范围 0~65535）+ LSB、RSB 下按键
   - **线性扳机**：双 10 位高精度霍尔模拟扳机（LT、RT，范围 0~1023）
4. **四马达独立震动反馈**：
   - 支持独立控制手柄内藏的 **4 个马达**：左握把低频强震、右握把高频弱震、左扳机微马达、右扳机微马达，精准调节强度与震动时长。
5. **完整的配对绑定与自动回连**：
   - 支持 SMP Just-Works 加密配对，安全密钥自动持久化至 NVS 闪存。
   - 首次配对后，手柄再次开机即可**毫秒级自动回连**。

---

## 🎮 报文键位映射表 (Xbox Series X/S, PID: 0x0B13)

| 报文偏移 | 信号类型 | 说明 / 取值范围 |
| :--- | :--- | :--- |
| `report[0:1]` | 模拟摇杆 | 左摇杆 X 轴 (0 ~ 65535, 居中约 32768) |
| `report[2:3]` | 模拟摇杆 | 左摇杆 Y 轴 (0 ~ 65535, 居中约 32768) |
| `report[4:5]` | 模拟摇杆 | 右摇杆 X 轴 (0 ~ 65535, 居中约 32768) |
| `report[6:7]` | 模拟摇杆 | 右摇杆 Y 轴 (0 ~ 65535, 居中约 32768) |
| `report[8:9]` | 线性扳机 | 左扳机 LT (10-bit: 0 ~ 1023) |
| `report[10:11]` | 线性扳机 | 右扳机 RT (10-bit: 0 ~ 1023) |
| `report[12]` (低 4 位) | 十字方向键 | Hat Switch (0:松开, 1:上, 2:右上, 3:右, 4:右下, 5:下, 6:左下, 7:左, 8:左上) |
| `report[13]` | 主要动作按键 | bit0: A, bit1: B, bit3: X, bit4: Y, bit6: LB, bit7: RB |
| `report[14]` | 功能按键与摇杆按键 | bit2: View, bit3: Menu, bit4: Xbox, bit5: LSB, bit6: RSB |
| `report[15]` | 分享键 | bit0: Share (Xbox Series 专属按键) |

---

## 📁 目录结构

```text
├── CMakeLists.txt                # 工程根目录 CMake 构建配置
├── sdkconfig.defaults            # 核心配置：启用 NimBLE、增加 ACL 缓冲区等
├── components/
│   └── xbox_controller/         # 模块化 Xbox 手柄驱动组件 (可直接移植到其他项目)
│       ├── CMakeLists.txt
│       ├── idf_component.yml    # IDF 组件定义清单
│       ├── include/
│       │   ├── xbox_controller.h # 对外 API、数据结构与回调接口
│       │   └── esp_hid_gap.h     # BLE GAP 扫描与广播解析
│       └── src/
│           ├── xbox_controller.c # 手柄连接管理、报文解码、非阻塞马达控制
│           └── esp_hid_gap.c     # 动态设备名称与外观过滤
└── main/
    ├── CMakeLists.txt
    └── controller.c              # 演示示例：按键边沿检测、马达振动联动、摇杆节流打印
```

---

## 🚀 快速上手

### 1. 环境准备
- **硬件**：ESP32-S3 开发板（或其他支持 BLE 的 ESP32 芯片）、原装 Xbox Series 无线手柄
- **软件**：ESP-IDF **v5.0 及以上**（已在 ESP-IDF v6.1 验证通过）

### 2. 克隆与编译

```bash
# 克隆仓库
git clone https://github.com/<your-username>/esp32-xbox-ble-controller.git
cd esp32-xbox-ble-controller

# 设置目标芯片为 ESP32-S3
idf.py set-target esp32s3

# 编译工程
idf.py build
```

### 3. 烧录与调试

将开发板通过 USB 连接至电脑（假设端口为 COM6）：

```bash
# 烧录并打开串口监视器
idf.py -p COM6 flash monitor
```

### 4. 手柄配对步骤（关键）
1. 固件运行后，ESP32 会自动开启 BLE 扫描并在串口输出提示。
2. **首次配对进入配对模式**：
   - 先按一下正面的 **Xbox 西瓜徽标键**开机；
   - **必须长按手柄顶部（Type-C 接口正上方）的圆形【配对小按钮】约 2~3 秒**；
   - 此时手柄正面的西瓜灯会从慢闪转为**极快速度的急促快闪**，此时手柄才真正进入 BLE 配对广播模式！
   > ⚠️ **注意**：如果只按正面的西瓜键开机，手柄只会处于慢闪状态（尝试回连上一台旧主机），ESP32 是无法搜索到未配对的手柄的。必须按下顶部的配对小按钮进入快闪状态！
3. ESP32 探测到广播并确认设备为 `Xbox Wireless Controller` 后，将自动发起安全握手与 SMP 加密绑定。
4. **配对成功**：手柄西瓜灯转为**常亮**，且手柄会触发一次清脆的双握把轻振（300ms），提示驱动就绪！
5. **后续自动回连**：绑定信息会自动保存在 ESP32 的 NVS 闪存中。以后使用时，**只需按一下正面西瓜键开机**，手柄与 ESP32 就会在数秒内自动完成回连，无需再次按配对按钮。

---

## 💻 示例代码与 API 指南

### 1. 初始化驱动并注册回调

```c
#include "xbox_controller.h"

static void my_controller_cb(xbox_controller_event_t event, const xbox_controller_data_t *data, void *user_arg)
{
    switch (event) {
    case XBOX_EVENT_CONNECTED:
        ESP_LOGI("APP", "Xbox 手柄已连接！");
        // 连接欢迎震动 (双握把轻振 300ms)
        xbox_controller_set_rumble(30, 30, 0, 0, 30);
        break;

    case XBOX_EVENT_DISCONNECTED:
        ESP_LOGW("APP", "Xbox 手柄断开连接！");
        break;

    case XBOX_EVENT_BATTERY:
        ESP_LOGI("APP", "当前剩余电量: %d%%", data->battery_level);
        break;

    case XBOX_EVENT_DATA:
        // 按下 A 键触发左握把低频强震
        if (data->btn_a) {
            xbox_controller_set_rumble(80, 0, 0, 0, 15);
        }
        // 读取左摇杆坐标
        printf("Joy Left: (%5u, %5u)\n", data->joy_left_x, data->joy_left_y);
        break;
    }
}

void app_main(void)
{
    xbox_controller_config_t cfg = {
        .target_mac = NULL,          // 传 NULL 表示自动搜索名为 Xbox 的手柄；也可指定目标 MAC 地址
        .event_cb = my_controller_cb,
        .user_arg = NULL,
        .debug_raw_reports = false,
    };

    ESP_ERROR_CHECK(xbox_controller_init(&cfg));
}
```

### 2. 控制四马达振动

```c
/**
 * @brief 控制 Xbox 手柄四马达震动 (0~100 强度)
 * @param strong_rumble 左握把低频重马达
 * @param weak_rumble   右握把高频轻马达
 * @param left_trigger  左扳机微马达
 * @param right_trigger 右扳机微马达
 * @param duration_10ms 持续时间 (单位: 10ms，例如 30 代表 300ms)
 */
xbox_controller_set_rumble(100, 0, 50, 0, 20); // 震动 200ms

// 立即停止所有马达
xbox_controller_stop_rumble();
```

---

## 🛠 将本组件移植到你自己的项目

只需将 `components/xbox_controller` 文件夹复制到你现有 ESP-IDF 项目的 `components/` 目录下，并在项目的 `sdkconfig`（或 `sdkconfig.defaults`）中添加以下关键配置：

```ini
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_ROLE_CENTRAL=y
CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y
CONFIG_BT_NIMBLE_GATT_CLIENT=y
CONFIG_BT_NIMBLE_GATT_SERVER=y
CONFIG_BT_NIMBLE_HID_SERVICE=y
CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT=48
CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT=48
CONFIG_BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT=64
```

---

## 🙏 参考与致谢 (References & Acknowledgments)

本项目在立项与最初报文逆向验证阶段，参考了开源社区优秀的先驱工作，特此致谢：

- **[LiZhiY0u/ESP32_connect_XboxController](https://github.com/LiZhiY0u/ESP32_connect_XboxController)**  
  *ESP32通过BLE蓝牙连接Xbox手柄读取所有的信号*  
  本项目参考了该仓库提供的 Xbox 手柄 HID 报文格式与摇杆/按键数据结构。

### 🔄 本项目相对原参考项目的改进与演进：
1. **脱离 Arduino，全面拥抱原生 ESP-IDF**：原参考项目基于 Arduino-ESP32 框架开发；本项目采用标准 ESP-IDF（支持 v5.x / v6.x）和原生 Apache NimBLE 协议栈，更适合商业级固件与对实时性、内存占用敏感的工程。
2. **攻克 ESP-IDF 原生 NimBLE Protocol Mode 缺陷**：原生 ESP-IDF 连接 Xbox Series 时会将缺少该特征值的手柄默认置为 `BOOT` 模式导致按键被丢弃，本项目加入了协议模式强制握手修正。
3. **彻底解决 250Hz 数据洪峰下的死锁断连**：重构了马达控制通道，采用非阻塞 `GATT Write Without Response`，避免了原先在短事件队列与高频上报下的 `MBUF alloc stuck` 崩溃断开问题。

---

## 📄 开源许可证

本项目基于 [Apache License 2.0](LICENSE) 协议开源。欢迎提交 Issue 与 Pull Request！

# 蓝牙发射功率动态调节设计文档 (Music nano)

## 1. 背景与目标

- 设备为经典蓝牙 BR/EDR **A2DP Source（音频发射端）**，连接蓝牙耳机。
- 目标：根据对端（耳机）链路信号强度**动态调节本机发射功率**，达到：
  - 节能（耳机贴近设备时没必要全功率发射）；
  - 保证中距离通信稳定（全低功率无法覆盖）；
  - **不打断正常音频推流**。
- 采用策略：**方案D —— RSSI 直接闭环**（简化版）：
  - 反馈：本机 `esp_bt_gap_read_rssi_delta` 轮询（约 1 Hz），借助信道互易性近似对端接收强度。
  - 控制：`esp_bredr_tx_power_set(lvl, lvl)`（min==max）**直接强制档位**。
  - **弃用 LMP 自动功率控制**，理由：
    1. LMP 是**事后反馈**——对端通常已在丢包后才请求调功率，只能"抢救"当前这次通信，下一次仍可能卡顿；
    2. 想根治"下次不卡"就得**记录设备 ID** 做针对性抬功率，否则只能**全局抬**；而全局抬会按对端里信号最好的设备（如蓝牙音箱）拉高功率，白白浪费；
    3. 该机制调度复杂、维护麻烦。
  - 简化方案：**不记设备 ID、无 NVS、全局单一调度**。用**功率下限 + 冗余增益**替代 LMP 的精细调节，简单且够用。
- 使用场景假设：设备通常像手机一样放在用户口袋里/贴身，一般不会太远。
- 运行方式：静默运行，不输出 UI/日志。

## 2. ESP32 发射功率 API

来源：`components/bt/include/esp32/include/esp_bt.h`

### 2.1 功率档位 `esp_power_level_t` (esp32/esp_bt.h:383-399)

ESP32 支持档位（dBm）：**-12 / -9 / -6 / -3 / 0 / +3 / +6 / +9**

| 枚举 | dBm |
|---|---|
| `ESP_PWR_LVL_N12` | -12 |
| `ESP_PWR_LVL_N9` | -9 |
| `ESP_PWR_LVL_N6` | -6 |
| `ESP_PWR_LVL_N3` | -3 |
| `ESP_PWR_LVL_N0` | 0 |
| `ESP_PWR_LVL_P3` | +3 |
| `ESP_PWR_LVL_P6` | +6 |
| `ESP_PWR_LVL_P9` | +9 |

> 另有向后兼容别名（`ESP_PWR_LVL_N14`→-12 等），勿混淆。

### 2.2 设置/读取 BR/EDR TX 功率 (esp32/esp_bt.h:626-659)

```c
esp_err_t esp_bredr_tx_power_set(esp_power_level_t min_power_level,
                                 esp_power_level_t max_power_level);
esp_err_t esp_bredr_tx_power_get(esp_power_level_t *min_power_level,
                                 esp_power_level_t *max_power_level);
```

- 设置的是 BR/EDR 功率控制算法的 **上下限范围**，影响 inquiry/page/connection 全局功率。
- **默认值**：`min = ESP_PWR_LVL_N0`（0 dBm），`max = ESP_PWR_LVL_P3`（+3 dBm）。
- `min == max` 即**强制固定档位**，同时使 LMP 自动功率控制失效（对端无法再请求调功率）。**方案D 采用此方式**。
- 文档建议：在 `esp_bt_controller_enable()` 之后、任何 RF 传输操作（discovery、profile 初始化等）之前调用。
- 实现位置：`components/bt/controller/esp32/bt.c:2009-2034`，内部调用控制器预编译库 `bredr_txpwr_set/get`。
- 注意：ESP32 经典蓝牙**无 menuconfig 默认功率项**（`BT_CTRL_DFT_TX_POWER_LEVEL` 仅存在于 C3/C6/H2 等纯 BLE 芯片）。
- 本项目当前未设置功率，使用默认 `[0, +3] dBm`（`bt_a2dp.c:547` `BT_CONTROLLER_INIT_CONFIG_DEFAULT()`）。

### 2.3 动态调节可行性（不打断音频流）

- `esp_bredr_tx_power_set` 仅写控制器功率算法边界，**不触碰 ACL/A2DP 连接状态**，无 HCI 断开，流媒体不中断。
- 每次改档位仅写一次控制器，改动瞬时生效，对正在进行的音频推流无影响。

## 3. 链路信号强度获取

### 3.1 连接后实时轮询（唯一控制反馈）

```c
esp_err_t esp_bt_gap_read_rssi_delta(esp_bd_addr_t remote_addr);
```

- 异步请求；结果通过注册的 GAP 回调事件送达：
  `ESP_BT_GAP_READ_RSSI_DELTA_EVT`，参数 `esp_bt_gap_cb_param_t->read_rssi_delta`：
  ```c
  struct read_rssi_delta_param {
      esp_bt_status_t stat;   /* 请求状态 */
      int8_t rssi_delta;      /* 相对黄金接收区间的偏移，-128~127 */
  } read_rssi_delta;
  ```
- **语义**（esp_gap_bt_api.h:19-20, 363-367）：
  - `rssi_delta == 0`：信号落在 **黄金接收区间** `[ESP_BT_GAP_RSSI_LOW_THRLD=-45, ESP_BT_GAP_RSSI_HIGH_THRLD=-20]` dBm，链路质量良好；
  - `rssi_delta > 0`：对端信号过强 → 对端很近 → 本机应**降功率**；
  - `rssi_delta < 0`：对端信号过弱 → 本机应**升功率**。
- **本质**：HCI `HCI_READ_RSSI`（opcode 0x1405）读取的是本机接收机对"对端发出包"的测量，借助**信道互易性**近似对端接收我们信号的情况（详见第 6 节）。
- **限制**：同一时刻仅允许一个在途请求（`BTM_BUSY` 保护，`stack/btm/btm_acl.c:1997-2001`）。必须**顺序轮询**：发出请求 → 收到事件 → 再发下一次。本设计固定 **1 Hz**。
- 调用链路：
  `esp_bt_gap_read_rssi_delta` → `btc_gap_bt_read_rssi_delta`（btc_gap_bt.c:650）→ `BTA_DmReadRSSI` → `BTM_ReadRSSI`（btm_acl.c:1988）→ `btsnd_hcic_read_rssi`（hcicmds.c:1753）→ `HCI_READ_RSSI` → `btm_read_rssi_complete`（btm_acl.c:2414）→ `ESP_BT_GAP_READ_RSSI_DELTA_EVT`。
- 需在已建立 ACL 连接后调用，否则 `BTM_UNKNOWN_ADDR`。

### 3.2 仅扫描时（参考，非实时）

- `ESP_BT_GAP_DISC_RES_EVT` 提供 `ESP_BT_GAP_DEV_PROP_RSSI`（int8_t，-128~127），为本机对设备 inquiry 响应的测量。
- 仅 discovery 期间有效，**不能**作为连接中实时控制信号。

## 4. 设计（方案D：RSSI 直接闭环）

集成点：`main/bt_a2dp.c`、`main/bt_a2dp.h`

### 4.1 参数与状态

| 项 | 值 |
|---|---|
| 起始功率（连接时） | `N0` = 0 dBm |
| 轮询周期 | 1 秒（单个在途请求保护） |
| 档位范围 | `[-9, +9]` dBm，3 dB 步进共 **7 档**：`N9 N6 N3 N0 P3 P6 P9` |
| 升档触发（偏弱） | `delta <= -1` → 升一档 |
| 降档触发（过强） | `delta >= +4` → 降一档 |
| 控制方式 | `esp_bredr_tx_power_set(lvl, lvl)`（min==max 强制） |
| 记忆 | 无设备 ID、无 NVS；断开复位到 `N0` |

```c
/* 档位表：下标即强度索引，从弱到强 */
static const esp_power_level_t LEVELS[] = {
    ESP_PWR_LVL_N9, ESP_PWR_LVL_N6, ESP_PWR_LVL_N3,
    ESP_PWR_LVL_N0, ESP_PWR_LVL_P3, ESP_PWR_LVL_P6,
    ESP_PWR_LVL_P9,
};
#define LEVELS_N   (sizeof(LEVELS)/sizeof(LEVELS[0]))
#define START_IDX  3                 /* N0 = 0 dBm */
#define UP_TRIG     (-1)             /* delta <= -1 升档(偏弱立即升) */
#define DN_TRIG     (+4)             /* delta >= +4 降档(明显过强才降) */

static int8_t s_pwr_idx = START_IDX; /* 当前档位下标 */
static bool  s_rssi_poll_inflight = false;
```

### 4.2 功率更新函数（不对称触发 = 冗余增益）

```c
static void bt_power_update(int8_t rssi_delta)
{
    if (rssi_delta <= UP_TRIG && s_pwr_idx < LEVELS_N - 1) {
        s_pwr_idx++;                          /* 偏弱立即升一档(增益 >1) */
    } else if (rssi_delta >= DN_TRIG && s_pwr_idx > 0) {
        s_pwr_idx--;                          /* 明显过强才保守降一档 */
    } else {
        return;                               /* 盲区 = 冗余，保持不动 */
    }
    esp_bredr_tx_power_set(LEVELS[s_pwr_idx], LEVELS[s_pwr_idx]);
}
```

- **为什么不对称**：离散档位步进 3 dB，把"1.2 倍"冗余增益落地为——**偏弱一出现就升**（`<= -1`），**要很强才降**（`>= +4`）。净效果是功率始终比"刚好够"高一档左右，用户信号变差时不落在边缘。
- **下限 -9 不用 -12**：牺牲一点极限节能，换取更大的"免实时调整"活动范围——贴近/小幅走动时即便本轮不动，链路仍有足够余量。

### 4.3 GAP 回调增加事件分支

`bt_a2dp_gap_cb`（bt_a2dp.c:344）新增：

```c
case ESP_BT_GAP_READ_RSSI_DELTA_EVT: {
    if (param->read_rssi_delta.stat == ESP_BT_STATUS_SUCCESS) {
        bt_power_update(param->read_rssi_delta.rssi_delta);
    }
    s_rssi_poll_inflight = false;            /* 允许下一次轮询 */
    break;
}
```

### 4.4 BT 任务循环轮询

`bt_a2dp_task`（bt_a2dp.c:543）主循环，`s_connected` 分支（bt_a2dp.c:605）约 1 Hz 节拍（复用 200ms tick 计数，第 5 次 tick 触发一次），且 `s_rssi_poll_inflight == false` 时调用：

```c
esp_bt_gap_read_rssi_delta(s_peer_bda);
s_rssi_poll_inflight = true;
```

### 4.5 初始化与复位

- **连接建立**（`ESP_A2D_CONNECTION_STATE_CONNECTED`，bt_a2dp.c:297）：`s_pwr_idx = START_IDX`，立即 `esp_bredr_tx_power_set(N0, N0)`。
- **断开**（bt_a2dp.c:307）：`s_pwr_idx = START_IDX`，`s_rssi_poll_inflight = false`，`esp_bredr_tx_power_set(N0, N0)` 复位。
- 不做设备 ID 记忆：每次连接从 `N0` 出发，由闭环在 1 Hz 内收敛，简单且无状态残留。

### 4.6 其他

- 静默运行，无 UI/日志（若后续需要调试，可加 ESP_LOGI 输出 delta/当前档位）。
- 轮询请求为异步 HCI 命令，开销极小，不影响音频回调（`bt_a2dp_data_cb`）。

## 5. 验证要点（有设备后）

1. 设备贴近时：RSSI delta 持续为正（≥+4），功率逐档降到 `N9` 后停住，音频不卡顿。
2. 拉开距离至约 3-5 m：delta 转负（≤-1），功率逐档升回，音频保持。
3. **弱信号冗余回归**：人为把对端放在信号较差位置时，本机功率应先于卡顿升档（不对称触发带来的提前量）。
4. 断连重连：功率从 `N0` 重新开始，无上一次连接的档位残留。
5. 全程音频流不中断（无 ACL 重建、无 A2DP 状态变化）。
6. 对比节能：近距离时实际档位应明显低于默认 `+3 dBm`。
7. 间隔 1 s 的档位变化瞬时生效，无爆音/中断。

## 6. 附：反馈语义与已知局限（互易性与不对称）

`rssi_delta` 反映的是 **"我们听到对端的强弱"**，用它推断 **"对端听到我们的强弱"** 依赖**信道互易性**。但互易性只对**空间路径损耗**成立，完整链路预算为：

```
我们听到对端 = 对端发射功率 + 对端天线增益 - 路径损耗 + 本机天线增益
对端听到我们 = 本机发射功率 + 本机天线增益 - 路径损耗 + 对端天线增益
```

两侧差值 = `(对端发射功率 - 本机发射功率)`。即**对端设备自身的发射功率/天线越大，我们听到它就越好**，与"它听我们好不好"无关。例如对端换满电设备/大天线 → 我们听到变强（delta 为正）→ 若盲目降功率，对端可能听不到我们。

**本方案对已知局限的处理（有意简化）：**

1. **降档保守**（`>= +4` 才降）：只有"明显过强"才降，把轻微的不对称（对端略强于我们）吸收进盲区，不做 1:1 跟随。
2. **功率下限 -9 dBm**：即使误判降档，也停在 -9 dBm，配合 3-5 m 典型工作距离仍有足够余量，不至于让对端听不到。
3. **使用场景假设**：设备贴身/口袋里，路径短、衰减小，不对称影响通常落在盲区内；对端几乎不可能出现"我们听它强、它听我们弱到断流"的极端不对称。
4. **不引入 LMP/设备记忆**：接受方案D的简单性，规避第 1 节所述 LMP 的复杂调度与全局抬功率浪费。

> 若未来遇到极端不对称设备导致听不到，可升级方向（不在当前范围内）：连接前用 discovery RSSI 定初始档位、或对特定设备 ID 记忆安全下限。

## 7. 参考资料（ESP-IDF v5.5.2 源码位置）

| 内容 | 位置 |
|---|---|
| 功率档位枚举、`esp_bredr_tx_power_set/get` | `components/bt/include/esp32/include/esp_bt.h:383-399, 626-659` |
| 控制器实现 | `components/bt/controller/esp32/bt.c:2009-2034` |
| RSSI delta 事件、阈值宏 | `components/bt/host/bluedroid/api/include/api/esp_gap_bt_api.h:19-20, 283, 363-367, 746-755` |
| `BTM_ReadRSSI` | `components/bt/host/bluedroid/stack/btm/btm_acl.c:1988-2023` |
| HCI Read RSSI 命令 | `components/bt/host/bluedroid/stack/hcic/hcicmds.c:1753` |
| RSSI 完成事件 | `components/bt/host/bluedroid/stack/btm/btm_acl.c:2414-2439` |
| BTC 层转发 | `components/bt/host/bluedroid/btc/profile/std/gap/btc_gap_bt.c:650-655, 1241-1242` |
| 项目 A2DP 源码 | `main/bt_a2dp.c`（stack_up:483 / gap_cb:344 / task loop:543 / connect:605 / disconnect:307） |

# Secondary Motion 配置结构

更新日期：2026-08-23
Schema version：角色 DB 与 preset 当前均为 1。

## 1. 文件布局

```text
<game root>/SecondaryMotion/
├─ data/
│  └─ characters.default.json
├─ presets/
│  ├─ Default.json
│  └─ <other>.json
├─ runtime/
│  ├─ config.json
│  ├─ runtime_status.json
│  └─ developer_command.json
├─ developer/
│  ├─ diagnostics.json
│  ├─ bone_dumps/
│  └─ breast_record.csv
└─ logs/
```

三类配置职责不同：

| 文件 | 职责 | 主要写入者 |
|---|---|---|
| `data/characters.default.json` | 角色技术事实、骨骼、axis、默认值、animation rules | Manager Developer/Characters |
| `presets/<name>.json` | 用户运动参数和角色 override | Manager Apply |
| `runtime/config.json` | 总开关、active preset、revision、可选 global | Manager ConfigService |

`runtime_status.json` 和 `developer_command.json` 是 Runtime/Manager 通信文件，不是 preset。

## 2. runtime/config.json

Manager 当前写出的最小结构：

```json
{
  "revision": 2,
  "enabled": true,
  "active_preset": "Default"
}
```

字段：

| 字段 | 规则 | 说明 |
|---|---|---|
| `revision` | 非负整数 | revision 变化触发完整 hot reload |
| `enabled` | boolean | false 时全局停止自定义写入，但仍加载配置/status |
| `active_preset` | string | 对应 `presets/<name>.json`；拒绝路径分隔符和 `..` |
| `global` | object，可选 | Runtime 低频参数和 legacy marker compatibility |

可选 `global`：

```json
"global": {
  "gait_sample_interval_ms": 50,
  "entity_refresh_interval_ms": 500,
  "replay_verify_window_ms": 150,
  "legacy_marker_mode": false
}
```

三个 interval 必须是正整数。`legacy_marker_mode=false` 是当前默认；true 时 `spring_test.txt` / `amplify_test.txt` 继续作为写入门控。

现行 schema 没有 party compensation。frame dedup 已消除 callback stacking，因此不要再添加 `party_compensation`、`four_member_factor` 等旧字段。

## 3. 角色技术 DB

结构：

```json
{
  "schema_version": 1,
  "characters": {
    "chr_0014_aurora": {
      "display_name": "Snowshine",
      "bones": {
        "right": "R_breast_01_jnt",
        "left": "L_breast_01_jnt",
        "allow_fallback_candidates": true
      },
      "axis": { "name": "Z", "sign": 1 },
      "animation_rules": {
        "optional_clip_fragment": "run"
      },
      "defaults": {
        "gait": {
          "idle":    { "amplitude_deg": 0,  "frequency_hz": 1.2 },
          "walk":    { "amplitude_deg": 3.6, "frequency_hz": 1.5 },
          "run":     { "amplitude_deg": 8.5, "frequency_hz": 1.7 },
          "sprint":  { "amplitude_deg": 12, "frequency_hz": 2.0 },
          "zipline": { "amplitude_deg": 8.5, "frequency_hz": 1.7 }
        },
        "envelope": {
          "amplitude_attack_tau_sec": 0.15,
          "frequency_tau_sec": 0.2,
          "to_idle_release_tau_sec": 0.02
        },
        "jump": { "enabled": false, "mode": "off" },
        "native_amplify": { "factor": 2 }
      }
    }
  }
}
```

DB 规则：

- key 必须是稳定 `chr_id`；显示名不参与 Runtime identity。
- `bones.right/left` 必须同时为空或同时存在。
- 显式骨骼优先；`allow_fallback_candidates` 决定失败后是否用候选表。
- `axis.name` 只接受 X/Y/Z；`sign` 只接受精确 -1/+1。
- `animation_rules` gait 只接受 walk/run/sprint/zipline，最多 16 条。
- `defaults` 为该角色没有 preset override 时的基础值。
- Developer Save 重写已有角色时保留已有 `animation_rules`。

## 4. preset

```json
{
  "schema_version": 1,
  "name": "Default",
  "characters": {
    "chr_0014_aurora": {
      "enabled": true,
      "motion_mode": "synthetic",
      "axis": { "name": "Z", "sign": 1 },
      "gait": {
        "idle":    { "amplitude_deg": 0,  "frequency_hz": 1.2 },
        "walk":    { "amplitude_deg": 20, "amplitude_down_deg": 20, "frequency_hz": 1.7 },
        "run":     { "amplitude_deg": 30, "amplitude_down_deg": 35, "frequency_hz": 2.7 },
        "sprint":  { "amplitude_deg": 45, "amplitude_down_deg": 45, "frequency_hz": 3.4 },
        "zipline": { "amplitude_deg": 15, "amplitude_down_deg": 10, "frequency_hz": 3.0 }
      },
      "envelope": {
        "amplitude_attack_tau_sec": 0.5,
        "frequency_tau_sec": 0.2,
        "to_idle_release_tau_sec": 0.7
      },
      "jump": {
        "enabled": false,
        "mode": "off"
      },
      "native_amplify": { "factor": 2 }
    }
  }
}
```

### motion_mode

只接受：

```text
off / synthetic / amplify_native
```

### gait

五个 gait：

```text
idle / walk / run / sprint / zipline
```

每项：

- `amplitude_deg`：Up；
- `amplitude_down_deg`：Down，缺失或 0 表示与 Up 对称；
- `frequency_hz`：Hz；
- `phase_offset_deg`：相位对齐偏移度数（0-180，缺失默认 0），在步态变化后的相位对齐时叠加到 clip 相位映射上。对称（左右交替）loop 的 run/sprint 通常取 180（反相）。
- `phase_align`：布尔，默认 true。PLL 相位跟踪开关；false 时振荡器相位自由积分（不向 clip 相位收敛）。
- `auto_frequency`：布尔，默认 true。自动频率对齐开关；false 时恒用 `frequency_hz`。
- `freq_dev_threshold`：比例（0-1，默认 0.05 = 5%）。开启 `auto_frequency` 时，振荡器先使用配置 `frequency_hz`；实测动画 loop 频率与当前使用频率的偏差**连续 5 个 20Hz 采样**超过该阈值后，频率以每采样 20% 的有界速率向实测值校正，偏差降到阈值一半时冻结（滞回带）。实测物理频率 = `2 × 实测 loop 频率`（同 `kLocomotionPhaseCyclesPerLoop` 语义）。

与相位对齐相关的 Runtime 行为：

- 相位映射按 `2 × 2π × normalizedTime`（`kLocomotionPhaseCyclesPerLoop = 2`）：一个完整动画 loop 产生两次物理振荡（每步一次），`normalizedTime` 每前进 0.5 即一个完整振荡周期。
- **PLL 相位跟踪**：20Hz 采样在稳定 locomotion loop 上发布相位参考（`2 × 2π × norm + phase_offset_deg`）；振荡器相位保持连续积分，以时间常数 `kPhaseLockTauSec = 0.5s` 向参考做最短路径牵引，并用积分项消除持续的速率偏差。位置牵引与积分项合计最多改变当前频率包络的 ±20%，同时保留 ±1.5Hz 的绝对上限；饱和时限制积分累积，避免为追相位产生半速或倍速晃动。较大的相位差会用更长时间平滑消除。
- 仅在完整读取的 clip 都是同一步态的 `loopStable` 动画（主 loop、zipline 巡索）时启用跟踪和频率测量；`start`/`stop`/`_to_`、`pivot`/`bump`、jump、idle、未知动画及不同步态混合都不提供可靠的状态相位参考，跟踪关闭并清除测量基线，恢复 loop 后重新采样。停滞、无效或不可信的时间增量及超过 250ms 的采样间隔中止连续偏差计数，不允许旧测量继续推进频率拟合。开始移动时立即选择当前步态自己的有效缓存频率或配置值，不沿用上一种步态的拟合值；频率包络仍按 `frequency_tau_sec` 平滑变化。此规则共用于 walk/run/sprint/zipline。
- 启动/切角色/idle→walk 等**幅度≈0** 时刻允许一次不可见的初始硬对齐（`kPhaseSnapAmpThresholdRad = 0.02`）；幅度非零的步态切换只走平滑牵引，绝不 snap。

### envelope

- `amplitude_attack_tau_sec`：幅度进入/变化时间常数；
- `frequency_tau_sec`：普通频率变化时间常数。切换到新的已支持 locomotion 步态、或在停止/跳跃/不支持动画后重新进入 locomotion 时，包络用不大于 0.10 秒的内部时间常数追踪该步态自己的频率目标，补齐本次频率差的约 95% 后恢复配置值。同一步态后续的拟合微调保持配置平滑；快速切换不改变拟合阈值、缓存归属或 PLL 总修正上限，也不重置相位。停止收尾仍冻结频率；
- `to_idle_release_tau_sec`：明确 locomotion stop/to-idle 的释放速度参数 τ。前段保持原指数衰减 `A(t) = A0 × exp(−t/τ)`，到 `h = ln(2) × τ`、振幅剩余一半时接入 `A(t) = A0/2 × max(0, 1 − (t−h)/(2τ))²`。衔接点的振幅和变化速度连续，并在约 `2.693τ` 时都平滑归零；只缩短后段低振幅拖尾，不加快前半振幅的衰减。Up/Down 保持各自初始振幅和比例，共用同一个释放进度；进入 idle 不重启计时。**释放尾**：只有采样到已支持的 stop/to-idle 动画，才允许剩余振幅在后续已识别的 idle loop 中继续衰减，到精确零后交还原生动画。直接从移动进入 idle 不启动释放尾；攻击、技能、跳跃、未知动画（包括与停止动画混合）或采样失败会取消释放尾，不能随后由 idle 恢复。释放全程**频率冻结在振荡器当前值**，只衰减幅度；重新移动会中止释放并从当前振幅进入正常幅度包络。归零时间按包络有效推进时间计算（每步最大 50ms），不是强制让原生 stop clip 在此刻结束；很小的 τ 仍表示很快的释放。

停止资格以已读取的动画身份为依据，不能用 0/NaN 等不可靠权重推断动画不存在。步态候选排序遇到非有限或超出 [0,1] 的权重时按 0 处理，仍只允许已支持的动画参与选择。停止/Jump 的直接合成同样保留上一帧输出记录，读回自己的输出时复用基准，避免逐帧叠加旋转。

### jump

```text
mode = off | landing_damped
```

`landing_damped` 可使用 `amplitude_deg`、`damping_tau_sec`、`frequency_hz`、`max_duration_sec`。产品数据当前默认 Jump disabled。

Jump disabled 时不仅角度目标归零，还会阻止 Compose、SetLocalRotation 和 replay。

### native_amplify

`factor` 是 quaternion delta 的幂放大 K，必须 finite 且大于 0。

## 5. 已删除字段

现行 DB/preset/Manager/Runtime 均不使用：

```text
bone_scale
amplitude_scale
party_compensation
four_member_factor
```

不同角色的视觉幅度完全由各自五个 gait 的 Up/Down 决定。旧文件即使含有 scale 字段，当前 loader 也不会读取或应用。

## 6. 热重载与 Apply

Manager Apply：

```text
PresetService.Write(tool copy)
→ PresetService.SyncMirror(game copy)
→ ConfigService.Write(revision + 1)
→ Runtime rebuilds complete snapshot
→ runtime_status.applied_revision ACK
```

Runtime 约 250 ms 轮询 `runtime/config.json`。只有 revision 变化才尝试新 snapshot；成功后 atomic swap，失败时：

- 当前 active snapshot 不变；
- `applied_revision` 不前进；
- `[CFG]` 写详细字段原因；
- `runtime_status.error` 写稳定摘要；
- 修正同一 revision 的瞬时 malformed runtime config 后，status error 可恢复清除。

## 7. 校验策略

启动与 hot reload 共用同一 loader/final validator。

### 字段缺失

使用结构默认值或已有 profile 值；缺失本身不等于非法。

### 字段存在但非法

拒绝整个新 snapshot，不做“半套角色 override”。主要限制：

- JSON root/nested block 类型正确；
- DB/preset schema_version=1；
- motion mode、jump mode、axis 枚举已知；
- sign 精确 -1/+1；
- 左右骨骼双空或双有；
- runtime revision 为非负整数；
- global interval 为正整数；
- native factor >0；
- `phase_align` / `auto_frequency` 类型必须为 boolean；
- `freq_dev_threshold` 必须为 (0,1) 内的 finite 比例；
- animation rules 数量和 gait 映射合法。

### amplitude / frequency / tau

只要求 JSON number 且 finite，不设置产品上下界；Manager UI 也不额外截断这些数值。单位分别是 degree、Hz、second。

## 8. 模板与更新

正式包携带：

```text
data/characters.default.template.json
presets/Default.template.json
presets/User.template.json
runtime/config.json
```

首次安装的 `CopyIfMissing` 只在游戏目录正式文件不存在时创建，不覆盖已有数据。Runtime DLL 和 proxy loader 属于代码产物，更新时允许覆盖并保留备份。

Manager 的 preset 列表只显示名称通过 Runtime 路径安全规则且正式文件存在的 preset；`*.template.json` 永远不作为可选 preset。旧版本若已经把 `Default.template` 或其他非法/丢失名称写入 `active_preset`，新版 Manager startup 会在正式 `Default.json` 存在时保留 enabled、revision+1 并恢复到 `Default`。如果游戏已经因 initial config invalid 进入 `DISABLED_SAFE`，修复 config 后仍需重启游戏，因为该次 startup 尚未安装 Runtime hooks。

用户采用“删除旧工具文件夹后解压新工具”的更新方式时，新的工具代码/模板会替换旧工具内容；游戏目录现有 DB/preset 的长期 merge/source-of-truth 规则仍需单独定义，不能从模板行为推断为自动覆盖。

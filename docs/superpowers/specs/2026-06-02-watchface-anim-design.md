# Watchface 实时监视动画 — 设计

日期:2026-06-02
目标板:`rpi_pico` / Pico Display Pack 2(320×240)
LVGL:9.3.0(Zephyr v4.2.1 自带)

## 目标

在 PSU 监视器主界面(`ui_Screen1`)**空着的下半屏**加入"动的图画",方向为**实时监视(矢量)**。
全部矢量绘制,几乎不增 RAM(改前 RAM 85% / 余 ~34KB)。

## 约束与决策

- **独立模块**:新建 `src/ui/watchface_anim.{c,h}`,**不修改 SquareLine 生成的 `ui_Screen1.c`**,
  以后重新导出 UI 不会冲掉动画。
- **接入点(仅 2 个非 SquareLine 文件)**:
  - `src/ui/watchface_ui.c`:`watchface_show()` 调 `watchface_anim_init(ui_Screen1)`;
    `watchface_set_ep()` 末尾调 `watchface_anim_update(evt)`(与现有标签更新同上下文,
    由 `psu_ctrl.c` 的 zbus 回调驱动,周期 `PSUCTRL_INTERVAL_MS = 500ms`)。
  - `CMakeLists.txt`:把 `watchface_anim.c` 加入 `app_SRCS`。
- 数据源:事件 `struct psuctrl_data_event { volts, amps, watts, energy, is_kWh }`。

## 模块接口

```c
void watchface_anim_init(lv_obj_t *parent);                       // 建控件
void watchface_anim_update(const struct psuctrl_data_event *evt); // 喂数据
```

## 动效清单

核心:
1. **功率实时曲线** — `lv_chart` 折线,下半屏左侧(~224×100),绿色,`UPDATE_MODE_SHIFT`
   向左滚动,100 点≈50s 历史。Y 轴按观测峰值自适应。源由 `ANIM_CHART_SRC` 编译开关选择,
   默认 `watts`(可一行改 `volts`/`amps`)。值 ×10 存整数提升分辨率。
2. **连接转圈** — `lv_spinner` 居中,启动时显示,收到第一帧数据后删除 → 露出曲线。

可选(均启用):
3. **数值变化反馈** — 大字读数变化超阈值时做一次轻微上跳(translate-y bob)。
   阈值:V 0.05 / A 0.02 / W 0.5 / Wh 0.005,避免噪声抖动每帧触发。
4. **负载弧表** — `lv_arc` 下半屏右侧(~86×86),显示 `watts / ANIM_LOAD_MAX_W`(默认 1200W)
   百分比,`lv_anim` 平滑过渡到新值(400ms, ease-out)。
5. **CC 模式脉动** — 读 `ui_LabelCVCC` 文字,为 "CC" 时电流大字做无限循环呼吸(opacity 255↔90)。
   **注意**:固件当前无真实 CV/CC 回读(`PSUCtrl_CVCC` 为空桩,标签恒为 "CV"),故此动效
   暂不触发;待固件补 CC 回读、把标签置为 "CC" 后自动生效,零额外改动。
6. **开机淡入** — `watchface_anim_init` 中把 `parent` 透明度 0→255,600ms ease-out。

## 已知数据缺口(非本次范围)

`send_psuctrl_data_event` 目前只读电压寄存器 0x20,`amps/watts` 暂为 0。因此默认的功率曲线/
弧表在真机上要等电流/功率回读接好才会跳动;调试期可把 `ANIM_CHART_SRC` 改为 `volts` 立即见效。

## 资源预算

chart 100×int32≈400B + 控件开销;spinner 收数据后删除;弧/动画为矢量。相对 ~34KB 余量可忽略。
LVGL `CONFIG_LV_USE_CHART/SPINNER/ARC` 均已为 y,无需改 Kconfig。

## 验证

`west build -b rpi_pico -d build_lcd2 -- -DCONFIG_PICO_DISPLAY_PACK2=y`(Anaconda 环境)编译通过,
对比改前后 RAM/Flash 占用。

# Azahar_GyroscopeCustomEdition (DiySC01 ver)
这是基于 **Azahar master-c9d2593-dirty (2026-06-03)** 版本修改而来的个人编辑版。
This is a personal custom edition forked from
**Azahar master-c9d2593-dirty (2026-06-03)**.

---

## 关于本分支 / About This Fork
### 中文

我使用 AI 工具创建了很多有趣的新功能。不过虽然它看起来很酷，
但由于新功能大量使用了 AI 编程，所以也许会有很多不可靠的代码。
**我并不推荐将这些可能不可靠的内容合并进主分支。**

我目前只计划小范围分享使用它，但分享就意味着
我必须遵守原版模拟器的 GPLv2.0 协议将代码开源，所以……就是这样发生了 
目前我只在 Windows 中测试了它，效果还不错，其他平台一切未知。
我也并没有精力后续继续维护它，目前它已经能做到让我满意的效果了。

当然，这个模拟器的规范与主分支一样，同样使用 
**GPLv2.0 协议**，且**不可用于非法取得的游戏**，请支持购买正版游戏。

---

### English

I used AI tools to create many interesting new features. While it looks cool, 
a significant portion of the code is AI-generated, 
which may introduce unreliable behaviors. 
**I do not recommend merging these potentially unstable changes into the main branch.**

My current plan is to share this within a small circle. However, 
sharing means I must comply with the original emulator's GPLv2.0 license 
and open-source the code — so here we are 
I have only tested this on **Windows**, and it works well enough. 
Other platforms are completely untested.
I don't have the energy to maintain it long-term;
it has already reached a level I'm satisfied with.

Like the original project, this fork is licensed under **GPLv2.0**. 
**Do not use it with illegally obtained games** — please support official purchases.

---

## 最大改动 / Biggest Change

该分支最大的改动是**陀螺仪相关**的功能。有些游戏使用陀螺仪来模拟体感射击玩法，而原版 Azahar 无法做到这一点。
The biggest change in this fork is **gyroscope/ motion controls**. Some games use gyroscope aiming, 
which the original Azahar doesn't support. This fork allows you to 
control gyroscope rotation using just the **mouse and right analog stick**.

> 鼠标在选中模拟器窗口时，会变为"对穿"模式穿透屏幕边界 —
> 这是预期行为，用于体感瞄准时的无边界鼠标操作。
>
> When the emulator window is focused, the mouse enters a "passthrough" mode — 
> it traverses screen boundaries freely. This is intended for uninterrupted gyro-aiming.

---

## 新增功能 / New Features

### 🎮 陀螺仪相关 / Gyroscope & Motion

| 功能 | Feature |
|------|---------|
| 鼠标操控陀螺仪，支持同时映射给左右摇杆/方向键 | Mouse-controlled gyroscope with simultaneous mapping to analog sticks & D-Pad |
| 可设定陀螺仪初始角度（默认 90° 抬起，适用于射击游戏） | Configurable initial tilt angle (default 90° raised, ideal for shooters) |
| 可限制陀螺仪最大倾斜角度 | Configurable maximum tilt angle |
| 支持自动竖向倾斜 | Automatic vertical tilt |
| [BETA] 自动横向倾斜 | [BETA] Automatic horizontal tilt |
| 控制器联动：C-Stick / Circle Pad / D-Pad / ABXY → 虚拟鼠标体感 | Controller link: C-Stick / Circle Pad / D-Pad / ABXY → virtual mouse gyro |

### 🖥️ 新的自定义布局（百分比版） / Custom Layout (Percentage-based)

| 功能 | Feature |
|------|---------|
| 模拟器顶部栏可自动隐藏 | Auto-hide top toolbar |
| 百分比版自定义布局，改变窗口比例时保持布局效果 | Percentage-based layout that scales with window resizing |
| 给上下屏幕添加圆角和边缘模糊 | Rounded corners + edge blur for both screens |
| 可选上下屏画面作为模糊背景 | Blurred screen background option |

### ⌨️ 按键输入 / Input Mapping

| 功能 | Feature |
|------|---------|
| 跨手柄的自适应按键映射（以 Xbox 布局为主） | Cross-controller adaptive key mapping (Xbox layout based) |
| 默认按键设定同时支持键盘和手柄 | Default bindings work with both keyboard and controller |
| 单一按键支持多重映射和重复映射 | Multi-mapping and duplicate key bindings |
| 按键可映射为下屏幕触屏坐标 | Bind keys to touch-screen coordinates |
| 右键菜单可映射鼠标左右键和滚轮 | Right-click menu supports mouse button & scroll wheel binding |

### 🎨 图形 / Graphics

| 功能 | Feature |
|------|---------|
| 更多自动内部分辨率选项 | More auto internal resolution options |
| 同时使用多个后处理 Shader 滤镜（最多 10 个） | Multi-layer post-processing shader stacking (up to 10) |
| 进入游戏时自动全屏 | Auto fullscreen on game launch |

---

## 构建说明 / Build Instructions

请参照原仓库的构建 Wiki：
[Building From Source](https://github.com/azahar-emu/azahar/wiki)
Please refer to the upstream build wiki:
[Building From Source](https://github.com/azahar-emu/azahar/wiki)

---

## 许可证 / License

**GPLv2.0** — 与上游项目一致。
Same as upstream — licensed under **GPLv2.0**.

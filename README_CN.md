# Speaking Users Overlay -- Mumble 插件

悬浮窗插件，实时显示 Mumble 服务器上当前正在说话的用户列表。

## 功能

- 实时显示说话 / 密语 / 喊话中的用户
- 可调节窗口透明度 (Alpha)
- 窗口置顶 (Always on Top)
- 鼠标穿透模式（点击穿透到后方窗口）
- 按发言时间排序 — 最近发言的用户始终在最上方
- 可配置可见人数 — 限制显示的说话人数，其余可滚动查看（默认 8 人）
- 自动回顶 — 鼠标离开 10 秒或开启穿透后自动回到最近发言列表
- 屏幕边缘吸附 — 窗口不会被拖出屏幕外
- 自动检测系统语言（中文 / English）
- 不显示在任务栏
- 配置自动持久化保存（位置、透明度、开关选项、可见人数等全部保留）
- 使用 GLFW + Dear ImGui (cimgui) 渲染

## 使用说明

### 基本操作

连接到 Mumble 服务器后，覆盖窗口会自动弹出，显示正在说话的用户。

| 操作 | 作用 |
|---|---|
| **拖拽标题区域（"说话列表"文字）** | 移动覆盖窗口位置 |
| **设置按钮** | 打开设置面板 |
| **X 按钮**（右上角） | 隐藏窗口（插件继续运行） |

### 设置面板

点击 **设置** 按钮打开：

| 设置项 | 说明 |
|---|---|
| **透明度** | 滑动条调节窗口透明度 (0.1 ~ 1.0) |
| **窗口置顶** | 保持窗口在其他窗口之上 |
| **鼠标穿透** | 让鼠标点击穿透到窗口后方的程序 |
| **可见发言人数** | 顶部显示的最远发言用户数 (1~64) |
| **重置窗口位置**（按钮） | 将窗口位置和大小重置为默认值 |

### 键盘快捷键

| 快捷键 | 功能 |
|---|---|
| `Ctrl + Shift + P` | 关闭鼠标穿透模式（逃生开关） |
| `Ctrl + Shift + H` | 重新显示被隐藏的窗口 |

### 说话列表行为

- 按发言时间排序：最近开始说话的用户始终排在列表最上方。
- 可见人数限制：默认只显示最近的 8 位发言者（可在设置中调整），其余用户出现在 `--- more ---` 分隔线下方，需要向下滚动查看。
- 滚动不会重置：当有新用户发言或停止发言时，当前滚动位置保持不变，不会被强制重置。
- 自动回顶：满足以下任一条件时，列表自动滚动到顶部：
  - 开启了鼠标穿透模式
  - 鼠标离开窗口超过 10 秒
  一旦用户手动滚动了列表，自动回顶暂停，直到下次空闲期重新激活。

### 窗口丢失或移出屏幕怎么办

窗口有屏幕边缘吸附保护，不能被拖出屏幕外（至少 20% 宽度保留在显示器内）。

如果窗口被隐藏了：
- 打开 **设置 > 显示窗口**（窗口隐藏时才会显示该按钮）。
- 或者按 `Ctrl+Shift+H` 快捷键。

如果窗口跑出屏幕或位置错乱，点击 **设置 > 重置窗口位置** 即可恢复到默认位置。

所有可调节的配置（位置、大小、透明度、穿透模式、置顶、可见发言人数）会在 Mumble 退出时自动保存到磁盘，下次启动插件时自动恢复。

配置文件位置：
- Windows: `%APPDATA%\Mumble\SpeakingOverlay.cfg`
- Linux:   `~/.config/mumble-overlay-plugin.cfg`
- macOS:   `~/.config/mumble-overlay-plugin.cfg`

## 预构建二进制文件

可以从 GitHub 仓库的 **Actions** 标签页下载预构建的 `.mumble_plugin` 文件。找到最近一次成功的构建，滚动到 **Artifacts** 部分，下载 `SpeakingUsersOverlay.mumble_plugin`，双击即可安装到 Mumble。

> 预构建文件仅为了方便使用。如有安全顾虑，强烈建议审查源码并自行构建。

## 构建

### 1. 获取依赖

```bash
# 递归克隆（含子模块）
git clone --recurse-submodules https://github.com/YOUR_USER/mumble-overlay-plugin.git
# 或已克隆后：
git submodule update --init --recursive
```

### 2. 编译

```bash
cmake -B build
cmake --build build
```

产物：`build/plugin.dll` (Windows)、`build/libplugin.so` (Linux)、`build/libplugin.dylib` (macOS)。

### 3. 独立测试（无需 Mumble）

```bash
cmake -B build -DOVERLAY_BUILD_STANDALONE=ON
cmake --build build
./build/overlay_test
```

### 4. 安装到 Mumble

**Windows：**
1. 创建文件夹 `%APPDATA%\Mumble\Plugins`（如果不存在）。
2. 将 `build/Release/plugin.dll` 复制到该文件夹中。
3. （重新）启动 Mumble — 插件会自动加载。
4. 可在 Mumble 的 **设置 > 插件** 中确认插件已运行。

**Linux / macOS：**
1. 将 `build/libplugin.so`（Linux）或 `build/libplugin.dylib`（macOS）复制到：
   - Linux:   `~/.local/share/Mumble/Plugins/`
   - macOS:   `~/Library/Application Support/Mumble/Plugins/`
2. （重新）启动 Mumble。

**打包为 .mumble_plugin（便携式安装）：**
```bash
zip SpeakingUsersOverlay.mumble_plugin plugin.dll manifest.xml
```
双击 `.mumble_plugin` 文件即可安装。

## 架构

```
[Mumble 主线程]                    [渲染线程]
  回调 --> speaking_users (mutex) <-- poll 回调 (只读)
  可调用 MumbleAPI                 禁止调用 MumbleAPI
```

## 协议

MIT。参见 [LICENSE](LICENSE)。

`include/MumblePlugin.h` 头文件来自 Mumble 项目 (BSD-3-Clause)。
依赖项（子模块）使用各自的宽松协议 (MIT / zlib)。

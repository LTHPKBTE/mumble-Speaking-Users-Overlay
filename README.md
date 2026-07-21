# Speaking Users Overlay -- Mumble Plugin

Overlay plugin that displays a real-time list of users currently speaking on a Mumble server.

[>>查看中文说明<<](./README_CN.md)

## Features

- Real-time display of talking / whispering / shouting users
- Adjustable window and text opacity, always-on-top, mouse passthrough
- Recent speakers first, configurable visible count, idle timeout
- Auto language detection (English / Chinese)
- Settings persist across restarts
- Rendered with GLFW + Dear ImGui

## Usage

### Basic Controls

Once connected to a server, a small overlay window appears showing speaking users.

| Control | Action |
|---|---|
| **Drag the title area ("Speaking Users" text)** | Move the overlay window |
| **Settings button** | Open settings panel |
| **X button** (top-right) | Hide the overlay (plugin keeps running) |

### Settings Panel

Click the Settings button to open:

| Setting | Description |
|---|---|
| **Window opacity** | Slider to adjust window background opacity (0.0 ~ 1.0). |
| **Text opacity** | Slider to adjust text and UI element opacity (0.0 ~ 1.0), capped at 0.2 when dangerous mode is off |
| **Allow risky opacity** | When unchecked, text opacity cannot go below 0.2 |
| **Scale** | Content scaling factor (1.0x ~ 4.0x). Affects both font size and window dimensions |
| **Always on top** | Keep window above other windows |
| **Mouse passthrough** | Let mouse clicks pass through the main overlay window (the settings panel stays interactive) |
| **Visible speakers** | Number of recent speakers shown at top (1~64) |
| **Always show all users** | When checked, all known users are always displayed in the list. Hides the "Recently speaking" option below |
| **Show recently speaking users** | (Only visible when "Always show all users" is off) When checked, recently-speaking users are shown dimmed |
| **Idle user opacity** | Opacity for non-speaking/recent users (0.0 ~ 1.0). Also capped at 0.2 when dangerous mode is off |
| **Idle timeout (s)** | Seconds before an idle user is removed from the list (1~120) |
| **Only show current channel** | When checked, only users in the same channel as you are shown |
| **Log to Mumble console** | Enable/disable logging of plugin messages to the Mumble console |
| **Log FPS every 10s** | (Debug) Print current framerate to Mumble log every 10 seconds. Default off |
| **VSync** | Enable/disable vertical sync. Default off, because some GPU drivers (NVIDIA/AMD) busy-wait on vsync causing high CPU usage. When enabled, all FPS settings below have no effect |
| **Auto-detect monitor refresh rate** | Automatically set clickable/settings FPS to your monitor's refresh rate (validated 30-350 Hz). Re-detects on display changes and plugin start. When enabled, the two FPS inputs are locked |
| **Passthrough FPS** | Target frames per second when mouse passthrough is active (15-400). Default 15 |
| **Clickable FPS** | Target FPS when the main window is interactive / clickable (15-400). Default 60, or auto-detected refresh rate |
| **Settings FPS** | Target FPS when the settings panel is open (15-400, highest priority). Default 60, or auto-detected refresh rate |
| **Idle FPS** | Target FPS when no mouse activity is detected for the idle timeout period (15-400). Default 4 |
| **Idle timeout (s)** | Seconds of no mouse activity before dropping to idle FPS (1-120). Default 5 |
| **Show Window** (button) | Re-show a hidden window |
| **Reset Position** (button) | Reset window position and size to defaults |
| **Reset All Settings** (button) | Reset all settings to factory defaults |

### Keyboard Shortcuts

| Shortcut | Action |
|---|---|
| `Ctrl + Shift + P` | Disable mouse passthrough (escape hatch) |
| `Ctrl + Shift + H` | Show a hidden window (if you closed it with X) |

### Mouse Passthrough Behavior

When **Mouse passthrough** is enabled:
- The main overlay window becomes non-interactive — clicks pass through to windows/game behind it.
- The title bar and settings button are hidden on the main window (they would be non-clickable anyway).
- The **Settings panel** remains interactive and unaffected by passthrough, so you can still adjust settings.
- Use `Ctrl+Shift+P` to quickly disable passthrough.

### Framerate Control

The plugin uses multiple FPS profiles with priority-based switching and automatic monitor refresh rate detection. For implementation details (high-res timer, idle detection, GPU driver behaviour), see [Technical Notes](docs/TECHNICAL.md#framerate-control).

### Speaker List Behavior

- Recent speakers first: The list is sorted so the most recently active user is always at the top.
- Visible limit: Only the most recent N speakers are shown in the top section (default 8, configurable in Settings). Additional speakers appear below a `--- more ---` separator when you scroll down.
- Scroll is preserved: Scrolling through the list won't reset when new users start or stop speaking.
- Auto-snap to top: The list automatically scrolls back to the top when:
  - Mouse passthrough is enabled, or
  - The mouse has been away from the window for 10+ seconds.
  Once you manually scroll, auto-snap is suspended until the next idle period.
- Idle users: With "Always show all users" enabled, all known users are displayed and never removed. With "Show recently speaking users", non-speaking users appear dimmed and are pruned after the configured timeout. When both are off, only actively-speaking users are shown.
- Channel filter: Enable "Only show current channel" to limit the list to users in the same channel as you. Channel membership is tracked via Mumble API callbacks.

### If the Window Is Lost or Off-Screen

The window cannot be dragged off-screen — edge clamping keeps at least 20% of it visible on your monitor.

If it's hidden:
- Use **Settings > Show Window** (appears when the window is hidden).
- Or press `Ctrl+Shift+H` to show a hidden window.

If the window is off-screen or mispositioned, use **Settings > Reset Position** to restore it to the default location.

All configurable settings are saved to disk immediately when changed in the Settings panel, and also when the plugin shuts down. They are restored automatically on next load. If the config directory doesn't exist, it is created automatically.

Saved config location:
- Windows: `%APPDATA%\Mumble\Mumble\SpeakingOverlay.cfg`

## Prebuilt Binary

Prebuilt binaries are available from the **Actions** tab of the GitHub repository. Look for the latest successful workflow run, scroll to **Artifacts**, and download `plugin-windows-x64-Release`. Extract the plugin file and follow the [installation instructions](#4-install-to-mumble) below.

> Prebuilt binaries are provided for convenience. It is strongly recommended to review the source code and build from source if you have any security concerns.

## Build

> **Platform support:** Currently only **Windows** is officially supported. Linux and macOS users may try building from source, but the author does not have testing environments for those platforms. Contributions for other platforms are welcome.

### 1. Get Dependencies

```bash
# Clone with submodules
git clone --recurse-submodules https://github.com/YOUR_USER/mumble-overlay-plugin.git
# Or if already cloned:
git submodule update --init --recursive
```

### 2. Compile

```bash
cmake -B build
cmake --build build
```

Output: `build/Release/plugin.dll` (Windows).

### 3. Standalone Test (No Mumble Required)

```bash
cmake -B build -DOVERLAY_BUILD_STANDALONE=ON
cmake --build build
./build/overlay_test
```

### 4. Install to Mumble

**Using Mumble's plugin manager:**
1. Open Mumble, go to **Settings > Plugins**. Click **Install Plugin**.
2. Select the compiled `plugin.dll`.
3. Enable the plugin in the list.

**Manual install:**
- Windows: Copy `plugin.dll` to `%APPDATA%\Mumble\Mumble\Plugins\`

**Bundle as .mumble_plugin (optional):**
```bash
zip SpeakingUsersOverlay.mumble_plugin plugin.dll manifest.xml
```
Double-click the `.mumble_plugin` file or use **Settings > Plugins > Install Plugin**.

> The installation steps above (**Step 4**) apply to both self-built and prebuilt binaries.

## Architecture

See [Technical Notes](docs/TECHNICAL.md#architecture).

## License

MIT. See [LICENSE](LICENSE).

The `include/MumblePlugin.h` header is from the Mumble project (BSD-3-Clause).
Dependencies (submodules) use their own permissive licenses (MIT / zlib).

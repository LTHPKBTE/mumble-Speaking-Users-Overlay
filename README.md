# Speaking Users Overlay -- Mumble Plugin

Overlay plugin that displays a real-time list of users currently speaking on a Mumble server.

[>>查看中文说明<<](./README_CN.md)

## Features

- Real-time display of talking / whispering / shouting users
- Shows ALL users on the server (including idle ones)
- Adjustable window transparency (Alpha) and text/UI opacity
- Always on Top
- Mouse passthrough mode — clicks pass through main overlay window, but settings panel remains interactive
- Recent speakers first — sorting by speaking recency
- Configurable visible count — limit shown speakers, scroll for the rest (default 8)
- Show only talking users, or show all users with dimmed idle ones ("Show idle users" setting)
- Configurable idle timeout — passive users are removed from list after N seconds (default 5)
- Auto-snap to top — list returns to most recent speakers after 10s idle or passthrough
- Screen edge clamping — window cannot be dragged off-screen
- Automatic system language detection (English / Chinese)
- No taskbar entry — won't clutter your taskbar
- Window auto-sizes to fit content on first launch
- Scale setting affects both font size and window size
- Settings persist across restarts — window position, transparency, visible count, etc. are saved
- Rendered with GLFW + Dear ImGui (cimgui)

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
| **Window opacity** | Slider to adjust window background opacity (0.0 ~ 1.0). Capped at 0.2 unless &#34;Allow risky opacity&#34; is checked |
| **Text opacity** | Slider to adjust text and UI element opacity (0.0 ~ 1.0), also capped at 0.2 when dangerous mode is off |
| **Allow risky opacity** | When unchecked, both window and text opacity cannot go below 0.2 |
| **Scale** | Content scaling factor (1.0x ~ 4.0x). Affects both font size and window dimensions |
| **Always on top** | Keep window above other windows |
| **Mouse passthrough** | Let mouse clicks pass through the main overlay window (the settings panel stays interactive) |
| **Visible speakers** | Number of recent speakers shown at top (1~64) |
| **Show idle users** | When checked, non-speaking users are shown dimmed in the list. When unchecked, only actively-speaking users appear |
| **Idle user opacity** | Opacity for non-speaking users (0.0 ~ 1.0). Also capped at 0.2 when dangerous mode is off |
| **Idle timeout (s)** | Seconds before a passive user is removed from the list (1~30). Default 5 |
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

### Speaker List Behavior

- Recent speakers first: The list is sorted so the most recently active user is always at the top.
- Visible limit: Only the most recent N speakers are shown in the top section (default 8, configurable in Settings). Additional speakers appear below a `--- more ---` separator when you scroll down.
- Scroll is preserved: Scrolling through the list won't reset when new users start or stop speaking.
- Auto-snap to top: The list automatically scrolls back to the top when:
  - Mouse passthrough is enabled, or
  - The mouse has been away from the window for 10+ seconds.
  Once you manually scroll, auto-snap is suspended until the next idle period.
- Idle users: When "Show idle users" is enabled, non-speaking (passive) users are displayed in the list with reduced opacity. All users on the server are shown (fetched via Mumble API on connect), so you can see the entire channel at a glance.
- Idle timeout: Passive users are automatically removed from the list after N seconds of inactivity (configurable in Settings, default 5).

### If the Window Is Lost or Off-Screen

The window cannot be dragged off-screen — edge clamping keeps at least 20% of it visible on your monitor.

If it's hidden:
- Use **Settings > Show Window** (appears when the window is hidden).
- Or press `Ctrl+Shift+H` to show a hidden window.

If the window is off-screen or mispositioned, use **Settings > Reset Position** to restore it to the default location.

All configurable settings (position, size, transparency, passthrough, always-on-top, visible speakers count) are automatically saved to disk when Mumble exits and restored next time the plugin loads.

Saved config location:
- Windows: `%APPDATA%\Mumble\SpeakingOverlay.cfg`
- Linux:   `~/.config/mumble-overlay-plugin.cfg`
- macOS:   `~/.config/mumble-overlay-plugin.cfg`

## Prebuilt Binary

Prebuilt binaries for each platform are available from the **Actions** tab of the GitHub repository. Look for the latest successful workflow run, scroll to **Artifacts**, and download the archive for your platform (e.g. `plugin-windows.zip`, `plugin-linux.tar.gz`, or `plugin-macos.tar.gz`). Extract the plugin file and follow the [installation instructions](#4-install-to-mumble) below.

> Prebuilt binaries are provided for convenience. It is strongly recommended to review the source code and build from source if you have any security concerns.

## Build

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

Output: `build/plugin.dll` (Windows), `build/libplugin.so` (Linux), or `build/libplugin.dylib` (macOS).

### 3. Standalone Test (No Mumble Required)

```bash
cmake -B build -DOVERLAY_BUILD_STANDALONE=ON
cmake --build build
./build/overlay_test
```

### 4. Install to Mumble

**Using Mumble's plugin manager:**
1. Open Mumble, go to **Settings > Plugins**. Click **Install Plugin**.
2. Select the compiled `plugin.dll` (Windows), `libplugin.so` (Linux), or `libplugin.dylib` (macOS).
3. Enable the plugin in the list.

**Manual install:**
- Windows: Copy `plugin.dll` to `%APPDATA%\Mumble\Plugins\`
- Linux:   Copy `libplugin.so` to `~/.local/share/Mumble/Plugins/`
- macOS:   Copy `libplugin.dylib` to `~/Library/Application Support/Mumble/Plugins/`

**Bundle as .mumble_plugin (optional):**
```bash
zip SpeakingUsersOverlay.mumble_plugin plugin.dll manifest.xml
```
Double-click the `.mumble_plugin` file or use **Settings > Plugins > Install Plugin**.

> The installation steps above (**Step 4**) apply to both self-built and prebuilt binaries.

## Architecture

```
[Mumble main thread]                [Render thread]
  callbacks --> speaking_users (mutex) <-- poll callback (read only)
  MumbleAPI allowed                  MumbleAPI forbidden
```

## License

MIT. See [LICENSE](LICENSE).

The `include/MumblePlugin.h` header is from the Mumble project (BSD-3-Clause).
Dependencies (submodules) use their own permissive licenses (MIT / zlib).

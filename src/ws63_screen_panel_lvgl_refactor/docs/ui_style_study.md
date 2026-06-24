# UI Style Study: esp32s3-lvgl-terminal

Reference: [Lee-Stone/esp32s3-lvgl-terminal](https://github.com/Lee-Stone/esp32s3-lvgl-terminal)

## 1. Page Structure Pattern

### SquareLine Studio Workflow
- UI designed in SquareLine Studio v1.5.0, exported as C code
- Each screen is a separate `ui_<Name>.c` file with `<Name>_screen_init()`
- All screens declared in `ui.h` with extern globals
- `ui_init()` calls all screen inits and sets initial screen via `lv_scr_load_anim()`

### Page Inventory (Reference Project)
| Page | File | Purpose |
|------|------|---------|
| Main | `ui_Main.c` | App launcher grid + clock + marquee |
| Set | `ui_Set.c` | Brightness slider + WiFi config |
| Serial | `ui_Serial.c` | Serial TX/RX terminal |
| Weather | `ui_Weather.c` | Weather display (API) |
| Music | `ui_Music.cpp` | MP3 player with roller selector |
| GPT | `ui_GPT.c` | AI voice chat |
| Game | `ui_Game.c` | Game selection (2048, etc.) |
| GameSon | `ui_GameSon.c` | Individual game container |
| Cal | `ui_Cal.c` | Calendar widget |
| About | `ui_About.c` | About/credits page |

### Key Takeaway
**One file per page, lazy-init on first navigation.** Pages are not all created at boot; `_ui_screen_change()` calls `target_init()` only when the page is first loaded. This saves RAM on embedded targets.

## 2. Icon Button Pattern (imgbtn)

### Design
Every navigation entry on the Main page uses `lv_imgbtn` with two states:
- `LV_IMGBTN_STATE_RELEASED` → relaxed icon (softer color, slight shadow)
- `LV_IMGBTN_STATE_PRESSED` → active icon (brighter, pressed-in feel)

Each icon button sits inside a borderless `lv_obj` panel (48x67px) with a text label below.

### Color Coding (per-app icon tint)
| App | Label Color | Hex |
|-----|-------------|-----|
| 设置 (Set) | Dark grey | `#2D302D` |
| 串口 (Serial) | Blue | `#1095DE` |
| 天气 (Weather) | Orange | `#EE9540` |
| 音乐 (Music) | Red | `#DE1C00` |
| 小智 (GPT) | Teal | `#34BA9C` |
| 游戏 (Game) | Purple | `#AF54FF` |
| 日历 (Cal) | Red-pink | `#FF5041` |
| 关于 (About) | Brown | `#B48141` |

### Key Takeaway
**Use released/pressed image pairs for tactile feedback.** Even on a capacitive screen, the visual state change makes the UI feel responsive. The icon+label-below layout is clean and touch-friendly at 48px target size.

## 3. Screen Transition Animation

### Implementation (`ui_helpers.c`)
```c
void _ui_screen_change(lv_obj_t ** target, lv_scr_load_anim_t fademode,
                       int spd, int delay, void (*target_init)(void))
{
    if(*target == NULL)
        target_init();
    lv_scr_load_anim(*target, fademode, spd, delay, false);
}
```

- Uses `lv_scr_load_anim()` with `LV_SCR_LOAD_ANIM_FADE_ON` mode
- Speed ~100ms, delay 0ms
- `false` = auto-delete old screen after transition
- Pages are lazily initialized on first visit

### Key Takeaway
**100ms fade transitions feel instant but smooth.** The lazy-init pattern is critical for RAM-constrained MCUs. We should adopt the same approach: only allocate page objects when first visited.

## 4. Task Lifecycle Management

### Pattern (from `task.cpp`)
- Each functional module (WiFi, Serial, Music, GPT) has a dedicated FreeRTOS task
- Tasks are created on-demand when the user enters the page
- Tasks are deleted when the user leaves the page
- Task handles are tracked globally for cleanup

### Key Takeaway
**Create tasks on page enter, delete on page exit.** This prevents idle tasks from consuming CPU/stack. For WS63 LiteOS, we should do the same: SLE sync task, storage task, etc. should be lifecycle-managed by the UI navigation.

## 5. Music Page → Sound Concept (WS63 Adaptation)

### Reference: Music Page
- Roller widget for song selection
- Play/Pause toggle switch (imgbtn with checked state)
- Volume slider (vertical)
- Next-track imgbtn

### WS63 Adaptation: Alert Sound Page
The music concept should be repurposed as **alert/tone configuration**:
- **Completion tone**: played when a laser job finishes
- **Error tone**: played on alarm/fault
- **Touch tone**: click feedback on button press
- Volume slider for each category
- Enable/disable toggles
- NOT a music player — no song roller, no track navigation

## 6. Game Page → Easter Egg Concept (WS63 Adaptation)

### Reference: Game Page
- Grid of game selection buttons (2048, 羊了个羊, 消消乐, 植物大战僵尸)
- Each button launches a sub-page (`ui_GameSon`)
- Back button returns to Main

### WS63 Adaptation: Easter Egg Games
- Hidden entry point (long-press on Dashboard logo or triple-tap)
- Single simple game (e.g., 2048 or snake)
- MUST NOT interfere with STOP/ABORT safety commands
- Low priority task, auto-paused when laser is active
- Default disabled, enabled via Settings toggle

## 7. Settings Page Pattern

### Reference: Set Page
- Brightness slider with live preview
- WiFi on/off toggle (imgbtn with checkable flag)
- WiFi network roller (auto-scan)
- Password entry via keyboard overlay
- Spinner shown during WiFi scan

### WS63 Adaptation
- Brightness slider
- Laser power calibration display
- SLE connection status
- Sound configuration
- Theme selection (dark/light)
- About page (firmware version, hardware info)

## 8. Marquee / Scrolling Text

### Reference
- Top bar has `lv_label` with `LV_LABEL_LONG_SCROLL_CIRCULAR` mode
- Displays welcome message that auto-scrolls

### WS63 Adaptation
- Use for status messages: "Job #42 complete", "SLE connected", etc.
- Also useful for long G-code filenames that don't fit in a fixed label

## 9. Layout Grid on Main Page

### Reference
- 2x4 grid of icon panels inside a `lv_tabview`
- Each panel: `lv_obj` (borderless) → `lv_imgbtn` + `lv_label`
- Consistent 48x48 icon size, 8px gap
- Labels use custom Chinese font (Alimama)

### WS63 Adaptation
- Not an app launcher grid — use a **dashboard layout** instead
- Top: status bar (connection, laser state, time)
- Center: large progress arc / status display
- Bottom: action buttons (Start, Stop, Focus, Settings)

## 10. Summary of Patterns to Adopt

| Pattern | Source | WS63 Usage |
|---------|--------|------------|
| SquareLine page-per-file | `ui_Main.c`, `ui_Set.c` | Each page in own `.c` file |
| Released/pressed imgbtn | All icon buttons | Navigation + action buttons |
| 100ms fade transition | `_ui_screen_change()` | Page navigation |
| Lazy page init | `_ui_screen_change()` | Only create pages on first visit |
| Task on-demand | `task.cpp` | SLE/storage tasks tied to page lifecycle |
| Scrolling label | `ui_LabelInit` | Status marquee, long filenames |
| Brightness slider | `ui_Set.c` | Settings page |
| Back button imgbtn | All sub-pages | Consistent top-left back button |
| Color-coded icons | Main page | Status indicators (green=OK, red=error) |
| Roller for list selection | Music, WiFi | Task list, log list |

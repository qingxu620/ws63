# UI Page Map: WS63 Laser HMI

## Page Hierarchy

```
┌─────────────────────────────────────────────────────┐
│                   Boot Screen                       │
│              (splash + init progress)               │
└──────────────────────┬──────────────────────────────┘
                       │ fade 100ms
                       ▼
┌─────────────────────────────────────────────────────┐
│              Home Dashboard (default)                │
│                                                     │
│  ┌─ Status Bar ──────────────────────────────────┐  │
│  │ SLE ●  RX ●  Laser OFF  Job: --  Time: 00:00 │  │
│  └───────────────────────────────────────────────┘  │
│                                                     │
│  ┌─ Progress Area ──────┐  ┌─ Info Panel ────────┐  │
│  │                      │  │ Speed:  -- mm/s     │  │
│  │    ┌──────────┐      │  │ Power:  -- %        │  │
│  │    │  75%     │      │  │ Lines:  -- / --     │  │
│  │    │ Progress │      │  │ Cache:  -- KB       │  │
│  │    │   Arc    │      │  │ ETA:    --:--       │  │
│  │    └──────────┘      │  └─────────────────────┘  │
│  │                      │                           │
│  │  IDLE / EXECUTING    │                           │
│  └──────────────────────┘                           │
│                                                     │
│  ┌─ Action Bar ──────────────────────────────────┐  │
│  │ [STOP]  [ABORT]  [FOCUS]  [Settings]          │  │
│  └───────────────────────────────────────────────┘  │
└──────────────────────┬──────────────────────────────┘
                       │
          ┌────────────┼────────────┬──────────────┐
          │            │            │              │
          ▼            ▼            ▼              ▼
   ┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐
   │  Job     │ │  Control │ │  Alert   │ │ Settings │
   │ Monitor  │ │  Panel   │ │  Sound   │ │          │
   └──────────┘ └──────────┘ └──────────┘ └──────────┘
                                                │
                                                ▼
                                         ┌──────────┐
                                         │Diagnost- │
                                         │   ics    │
                                         └──────────┘

   Hidden gesture (triple-tap logo):
                                         ┌──────────┐
                                         │ Easter   │
                                         │ Egg Games│
                                         └──────────┘
```

## Page Definitions

### 1. Home Dashboard (`page_home.c`)

**Purpose:** Primary screen — always returns here on idle.

**Layout (320x240 landscape):**
- **Top bar (32px):** SLE status dot, RX status dot, laser state badge, job name, clock
- **Center (160px):** Progress arc (left) + info panel (right)
- **Bottom bar (48px):** STOP, ABORT, FOCUS, Settings buttons

**States:**
| State | Arc | Info Panel | Action Buttons |
|-------|-----|------------|----------------|
| IDLE | Hidden | "Ready" | FOCUS, Settings |
| RECEIVING | Hidden + spinner | "Downloading..." | ABORT |
| READY | Hidden | Job summary | START, ABORT, FOCUS |
| EXECUTING | Animated progress | Live stats | STOP only |
| DONE | 100% green | Summary + time | ABORT (reset), FOCUS |
| ERROR | Red pulse | Error message | ABORT, Settings |
| LINK_LOST | Hidden | "SLE LOST" | None (alert mode) |

**Navigation:**
- Settings icon → Settings page
- Status bar tap → Diagnostics page
- Triple-tap logo → Easter Egg Games (hidden)

### 2. Job Monitor (`page_job_monitor.c`)

**Purpose:** Detailed view of job execution data.

**Content:**
- Job ID and filename
- Total lines vs executed lines
- Byte progress (received / total)
- Cache free space
- Current X/Y position
- Feed rate and laser power
- Execution time
- Error log (last N errors)

**Navigation:** Back button → Dashboard

### 3. Control Panel (`page_control.c`)

**Purpose:** Advanced laser controls beyond the dashboard action bar.

**Content:**
- Focus power slider (0-100%)
- Focus ON/OFF toggle
- Manual jog buttons (if RX supports)
- Power calibration display
- Speed override slider

**Navigation:** Back button → Dashboard

### 4. Alert Sound (`page_alert_sound.c`)

**Purpose:** Configure audio feedback tones.

**Content:**
- **Completion tone:** Enable toggle + volume slider
- **Error tone:** Enable toggle + volume slider
- **Touch feedback:** Enable toggle + volume slider
- Preview button for each tone

**NOT a music player.** No song selection, no playback controls, no track navigation.

**Navigation:** Back button → Settings

### 5. Settings (`page_settings.c`)

**Content:**
- Brightness slider (with live preview)
- Theme toggle (Dark / Light)
- Sound settings → Alert Sound page
- SLE connection info (MAC, RSSI)
- RX firmware version
- Screen firmware version
- Factory reset button

**Navigation:** Back button → Dashboard

### 6. Diagnostics (`page_diagnostics.c`)

**Purpose:** Technical debug information.

**Content:**
- SLE packet counters (TX/RX/ACK/NACK)
- SLE RSSI history
- Connection uptime
- Last error messages
- Memory usage (LVGL heap, stack)
- SPI bus statistics
- Touch coordinate display
- Export log to SD (future)

**Navigation:** Back button → Dashboard

### 7. Easter Egg Games (`page_easter_egg.c`)

**Purpose:** Hidden fun during long engrave waits.

**Access:** Triple-tap on dashboard logo (hidden gesture)

**Content:**
- Single game: 2048 (or similar simple game)
- Score display
- Reset button
- Back to dashboard

**Safety constraints:**
- Game task runs at LOWEST priority
- Auto-pauses on any STOP/ABORT/Error event
- Auto-pauses when laser state changes to EXECUTING
- Default DISABLED — must be enabled in Settings
- Does NOT show in normal navigation

## Navigation Flow

```
Dashboard ──┬──→ Job Monitor (tap progress area)
            ├──→ Control Panel (tap power/speed area)
            ├──→ Settings (tap gear icon)
            │       ├──→ Alert Sound
            │       └──→ About (scroll down)
            ├──→ Diagnostics (tap status bar)
            └──→ Easter Egg (triple-tap logo, hidden)

Any page ──→ Dashboard (back button or home gesture)
```

## Page Transition Rules

| From | To | Animation | Duration |
|------|----|-----------|----------|
| Boot | Dashboard | Fade in | 300ms |
| Dashboard | Any sub-page | Fade on | 100ms |
| Sub-page | Dashboard | Fade on | 100ms |
| Settings | Alert Sound | Fade on | 100ms |
| Any | Emergency STOP | Instant (no anim) | 0ms |

**Critical rule:** STOP and ABORT must NEVER be blocked by a page transition animation. If a transition is in progress and STOP is pressed, the transition must complete immediately and the STOP command must be sent.

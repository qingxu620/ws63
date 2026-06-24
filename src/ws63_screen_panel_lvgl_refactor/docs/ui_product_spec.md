# UI Product Specification: WS63 Laser HMI

## Product Identity

**WS63 Screen Panel** is a **laser engraver local HMI** (Human-Machine Interface), NOT an entertainment terminal.

The screen module (MSP3223, ILI9341V 320x240, FT6336U touch) provides direct machine control and status monitoring at the laser workstation, independent of the PC Host tool.

## Design Philosophy

### Primary Role: Industrial Control Interface
- Real-time laser job status display
- Local safety controls (STOP, ABORT, FOCUS)
- Connection health monitoring (SLE, RX, Host)
- Job progress tracking

### Secondary Role: Technical Showcase
The UI should demonstrate high engineering quality — smooth animations, polished theme, responsive touch — because this is a portfolio piece. "Beautiful AND functional" is the goal.

### Tertiary Roles (Low Priority)
| Feature | Role | Priority |
|---------|------|----------|
| Alert Sounds | Completion/error/touch feedback tones | P2 |
| Easter Egg Games | Hidden fun during long engrave waits | P3 |

## What This Screen IS

| Aspect | Description |
|--------|-------------|
| **Dashboard** | Shows laser state, job progress, SLE link, errors |
| **Safety Panel** | STOP and ABORT are always one tap away |
| **Status Monitor** | Real-time RX state, cache level, execution line |
| **Local Control** | Focus on/off, power adjustment, job start |
| **Config Terminal** | Brightness, sound, theme, diagnostics |

## What This Screen IS NOT

| Anti-Pattern | Reason |
|--------------|--------|
| App Launcher | No app grid — this is a machine, not a phone |
| Music Player | No MP3 playback — only alert tones |
| Game Console | Games are hidden easter eggs, not main features |
| G-code Editor | Host tool handles all G-code editing |
| Second Host Tool | Screen does NOT upload/parse G-code |
| WiFi Config Tool | No WiFi on this hardware |

## User Scenarios

### Scenario 1: Normal Job Flow
1. User uploads job from Host tool via SLE
2. Screen shows "RECEIVING" with progress bar
3. Screen shows "READY" when job is cached
4. User presses START on screen (or Host)
5. Screen shows "EXECUTING" with live progress arc
6. Screen shows "DONE" with completion tone
7. Laser auto-off, screen returns to idle dashboard

### Scenario 2: Emergency Stop
1. User sees problem during engraving
2. Taps STOP on screen (always visible during EXECUTING)
3. Screen sends `@EXEC_STOP` to RX
4. Laser off immediately
5. Screen shows "STOPPED" state

### Scenario 3: Focus Alignment
1. User needs to align laser focus before job
2. Taps FOCUS on dashboard
3. Screen sends `@FOCUS_ON S50` to RX
4. Laser fires at low power for alignment
5. User taps FOCUS OFF or START job (auto-cancels focus)

### Scenario 4: Idle / Waiting
1. No active job
2. Dashboard shows connection status, last job info
3. User can adjust brightness, check diagnostics
4. Easter egg game available via hidden gesture (if enabled)

## Color Language

| Color | Meaning | Usage |
|-------|---------|-------|
| Green (`#00FFCC`) | Active / OK | SLE connected, job running |
| Red (`#FF3366`) | Error / Stop | Error state, STOP button |
| Yellow (`#FFCC00`) | Warning / Idle | Idle state, caution |
| Blue (`#00B3FF`) | Info / Link | SLE link, RX connection |
| Orange (`#FF9900`) | Reset / Abort | ABORT button |
| Dark (`#0A0C10`) | Background | Main background |
| Card (`#121620`) | Surface | Card backgrounds |

## Typography

| Element | Font | Size |
|---------|------|------|
| Dashboard title | Montserrat Bold | 16px |
| Status values | Montserrat | 14px |
| Labels | Montserrat | 10px |
| Progress % | Montserrat | 28px |
| Button text | Montserrat | 10px |

## Safety Rules

1. STOP button must be visible and enabled whenever laser is active
2. ABORT button must be visible when job is loaded
3. Screen must NOT block STOP/ABORT with animations or overlays
4. Easter egg games auto-pause on any safety event
5. FOCUS auto-cancels on job start, STOP, ABORT, or disconnect
6. Screen must show "LINK LOST" prominently if SLE drops during job

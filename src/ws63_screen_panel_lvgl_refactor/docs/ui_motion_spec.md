# UI Motion Specification: WS63 Laser HMI

## Overview

All animations use LVGL's built-in animation engine. Timing is tuned for a 320x240 IPS display at ~30fps refresh.

## Animation Catalog

### 1. Page Transition: Fade

**Trigger:** Navigation between pages
**Duration:** 100ms
**Easing:** Linear (LVGL default)
**Implementation:** `lv_scr_load_anim(LV_SCR_LOAD_ANIM_FADE_ON, 100, 0, false)`

```
Old page:  opacity 255 → 0  (100ms)
New page:  opacity 0 → 255  (100ms)
```

**Special case — Emergency STOP:** No animation. `lv_scr_load()` instant switch if STOP/ABORT is triggered during transition.

### 2. Button Press Feedback

**Trigger:** Touch down on any button
**Duration:** 50ms press, 80ms release
**Visual:** Scale down 5% on press, restore on release

```
Pressed:   scale 256 → 243  (50ms, ease-out)
Released:  scale 243 → 256  (80ms, ease-out)
```

**For imgbtn:** Swap to `_pressed` image source on `LV_EVENT_PRESSED`, back to `_released` on `LV_EVENT_RELEASED`.

### 3. Progress Arc Animation

**Trigger:** Progress value update (0-100%)
**Duration:** 200ms for small changes, 300ms for large jumps (>20%)
**Easing:** Ease-out

```
Arc value: old → new  (200-300ms, ease-out)
```

**Color transitions:**
| State | Arc Color | Transition |
|-------|-----------|------------|
| IDLE | Hidden | Fade out 200ms |
| EXECUTING | Green `#00FFCC` | Fade in 200ms |
| DONE | Green `#00FFCC` | Hold 2s, then fade out |
| ERROR | Red `#FF3366` | Pulse 3x (200ms each) |

### 4. Progress Percentage Text

**Trigger:** Value change during EXECUTING
**Duration:** 150ms
**Easing:** Linear crossfade

```
Old text:  opacity 255 → 0  (75ms)
New text:  opacity 0 → 255  (75ms)
```

### 5. Status Badge Transition

**Trigger:** State change (IDLE → EXECUTING → DONE, etc.)
**Duration:** 200ms
**Visual:** Color morph + text swap

```
Badge background:  old_color → new_color  (200ms)
Badge text:        opacity fade swap       (100ms)
```

### 6. Completion Popup

**Trigger:** Job complete (DONE state)
**Duration:** 300ms appear, auto-dismiss after 3s
**Visual:** Scale from 80% + fade in

```
Appear:   scale 205 → 256, opacity 0 → 255  (300ms, ease-out)
Dismiss:  opacity 255 → 0  (200ms, ease-in)
```

### 7. Error Alert Popup

**Trigger:** Error state entered
**Duration:** 200ms appear, stays until dismissed
**Visual:** Red border pulse + fade in

```
Appear:    opacity 0 → 255  (200ms)
Pulse:     border_width 1→3→1  (300ms loop, 3x)
Dismiss:   opacity 255 → 0  (150ms)
```

### 8. Loading Spinner

**Trigger:** Waiting for RX response (RECEIVING state)
**Duration:** Continuous rotation
**Speed:** 1 revolution per second
**Implementation:** `lv_spinner` with 1000ms period, 90° arc

### 9. Link Lost Flash

**Trigger:** SLE disconnect during active job
**Duration:** 500ms per flash, 3 cycles
**Visual:** Status dot alternates between red and dark

```
Flash:  red(#FF3366) → dark(#1A1A2E)  (250ms)
Repeat 3x, then hold red
```

### 10. Touch Ripple (Optional)

**Trigger:** Touch down anywhere
**Duration:** 200ms
**Visual:** Small circle expands from touch point, fades out
**Implementation:** `lv_anim` on custom draw object

```
Radius:  0 → 30px  (200ms, ease-out)
Opacity: 100 → 0   (200ms, ease-in)
```

## Animation Timing Summary

| Animation | Duration | Easing | Priority |
|-----------|----------|--------|----------|
| Page fade | 100ms | Linear | Normal |
| Button press | 50ms | Ease-out | High |
| Button release | 80ms | Ease-out | High |
| Arc progress | 200-300ms | Ease-out | Normal |
| % text swap | 150ms | Linear | Low |
| Badge color | 200ms | Linear | Normal |
| Completion popup | 300ms | Ease-out | High |
| Error alert | 200ms | Linear | Critical |
| Spinner | 1000ms/rev | Linear | Low |
| Link flash | 250ms/cycle | Linear | Critical |

## Performance Constraints

- **Max concurrent animations:** 4 (to avoid frame drops on WS63)
- **No animation during STOP/ABORT:** All animations pause for safety commands
- **Disable animations on low battery:** If battery powered, reduce to zero animations
- **SPI bus sharing:** Animation frame budget must account for LCD SPI transfer time (~5ms per 320x48 line buffer flush)

## Implementation Notes

### LVGL v9 Animation API
```c
lv_anim_t a;
lv_anim_init(&a);
lv_anim_set_var(&a, target_obj);
lv_anim_set_values(&a, start_val, end_val);
lv_anim_set_duration(&a, duration_ms);
lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
lv_anim_start(&a);
```

### Screen Change Helper
```c
void ui_screen_change(lv_obj_t **target, void (*init_fn)(void),
                      lv_scr_load_anim_t anim, uint16_t ms)
{
    if (*target == NULL)
        init_fn();
    lv_scr_load_anim(*target, anim, ms, 0, false);
}
```

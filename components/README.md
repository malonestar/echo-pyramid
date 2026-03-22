# Local Component Overrides

This directory contains local ESPHome component overrides that shadow upstream components
from the m5stack `esphome-yaml` GitHub source. They are loaded first via the `external_components`
declaration in `echo-pyramid.yaml` and persist across builds — they won't be overwritten by
upstream changes or `esphome clean`.

---

## `pyramidrgb/` — STM32 RGB LED Controller

### Background

The Echo Pyramid uses an STM32 co-processor to drive four groups of 7 RGB LEDs each (two
groups per strip, two strips). The ESPHome `pyramidrgb` component exposes each group as an
`rgb` light by registering three `FloatOutput` channels per group — one each for red, green,
and blue.

Adding LED animations to voice assistant state changes (listening, thinking, replying, etc.)
exposed serious instability in the upstream component: flickering, wrong color flashes, LEDs
getting stuck on incorrect colors mid-transition, and — unexpectedly — measurable latency
added to the voice pipeline itself. State changes that should have been nearly instantaneous
were visibly sluggish.

### Root Cause: The I2C Write Storm

The core problem is how ESPHome's `rgb` light platform drives its outputs during color
transitions. When a light changes color or brightness, the transition engine calls
`write_state()` **separately** on the red output, then the green output, then the blue output
— three independent calls, not one atomic update.

In the upstream component, each `write_state()` call immediately triggered a full hardware
write: 7 individual I2C transactions (one per LED in the group), each 5 bytes. So for a
single color update on one group:

- Red `write_state()` → 7 I2C writes → LEDs briefly show `(new_R, old_G, old_B)` ❌
- Green `write_state()` → 7 I2C writes → LEDs briefly show `(new_R, new_G, old_B)` ❌
- Blue `write_state()` → 7 I2C writes → LEDs finally show the correct color ✓

That's 21 I2C transactions to update one group, and two out of three intermediate states
show the wrong color — which is exactly the flickering and wrong-color flashes that were
visible.

Scaled to all four groups updating simultaneously (which every animation does), and running
at ESPHome's ~30fps transition rate during long brightness sweeps, this produced:

- **84+ I2C transactions per animation frame**, with ~56 of them writing incorrect colors
- A near-continuous I2C bus load that was starving other bus activity
- The I2C contention was adding enough overhead to delay the voice pipeline's state
  callbacks, which is why even non-LED operations felt sluggish

### Fix 1: Deferred Writes via Dirty Flags

The solution is to decouple the color buffer update from the I2C write. The component now
maintains a `channel_dirty_[]` flag array alongside the existing `channel_colors_[]` buffer.

**`write_state()` path (via `set_channel_color_component`):**
- Updates the color component in the buffer
- Sets the channel's dirty flag
- Returns immediately — **no I2C write**

**`loop()` (runs once per ESPHome main loop tick):**
- Iterates all four channels
- For each dirty channel, calls `write_channel_now_()` to flush the full color to hardware
- Clears the dirty flag

Because all three `write_state()` calls for a given channel happen within the same loop
tick (they're driven by the same light component's `loop()`), by the time our `loop()` runs
in the next tick, the buffer holds the final correct RGB value. The hardware never sees a
partial color state.

The result is **one I2C flush per dirty channel per loop tick**, regardless of how many
`write_state()` calls occurred. Four groups updating simultaneously = 4 I2C flushes, each
writing the correct final color, with no intermediate garbage states.

### Fix 2: Per-LED Writes (Burst Write Revert)

An early optimization attempt replaced the 7 individual per-LED writes with a single
29-byte I2C burst write covering all 7 LEDs in one transaction. This worked in theory —
the registers for each channel are laid out sequentially in 4-byte increments — but caused
persistent pink flickers on two of the four groups.

The likely cause is that the STM32 controller's I2C slave implementation has an
auto-increment limit shorter than 28 data bytes. Beyond that limit, subsequent bytes appear
to be written to incorrect register addresses, corrupting the color data for specific LEDs
within a channel.

The fix was to revert to the original per-LED write pattern (7 × 5-byte writes per channel
flush), which the STM32 handles reliably. The deferred-write approach still applies — those
7 writes happen once per tick when the channel is dirty, not three times per `write_state()`
call as before — so the I2C load is still dramatically reduced.

### Fix 3: Stepped Breathing Animation

The listening-phase breathing animation originally used ESPHome's built-in light transition
system with 900ms and 1100ms transition lengths. ESPHome interpolates transitions at roughly
30fps, so a 900ms inhale alone generated ~27 `write_state()` calls per output channel —
around 100+ I2C flushes per second just for the breathing effect, even after Fix 1.

While Fix 1 ensures those writes always contain correct colors, the sheer volume was still
unnecessary bus traffic. The breathing animation was replaced with six explicit lambda steps
(three inhale, three exhale) using `transition_length: 0` and explicit `delay` actions to
shape the timing:

```
Inhale:  0.50 → 0.67 → 0.83 → 1.00   (~310ms between steps)
Exhale:  1.00 → 0.83 → 0.67 → 0.50   (~367ms between steps)
```

Each lambda sets all four groups atomically in a single shot. With six steps per ~2.3s
cycle, this produces roughly **10 I2C writes per second** for the breathing animation,
down from ~100/sec with transitions. The stepped progression is visually smooth at ambient
lighting levels.

The same principle — prefer explicit steps with delays over long ESPHome transitions — is
worth applying to any future animations that need to minimize bus load.

### Summary of Changes

| File | Change |
|------|--------|
| `pyramidrgb/pyramidrgb.h` | Added `loop()` override, `channel_dirty_[]` array, `write_channel_now_()` declaration |
| `pyramidrgb/pyramidrgb.cpp` | `set_channel_color` / `set_channel_color_component` now only update buffer + set dirty flag; `loop()` flushes dirty channels; `write_channel_now_()` uses per-LED writes |
| `pyramidrgb/output/pyramidrgb_output.h` | No logic change; `write_state()` calls `set_channel_color_component` which now defers the I2C write |
| `pyramidrgb/__init__.py` | Unchanged copy of upstream (required for local override to work) |
| `pyramidrgb/output/__init__.py` | Unchanged copy of upstream |
| `echo-pyramid.yaml` | `external_components` updated to load local `pyramidrgb` first; `listening_pulse` script rewritten with stepped lambdas |

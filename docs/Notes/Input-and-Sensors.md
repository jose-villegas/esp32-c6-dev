# Input and Sensors

Part of the platform notes for the Waveshare ESP32-C6-Touch-AMOLED-1.8 - see
[`README.md`](README.md) for the full set. Everything here was verified on the
actual board or read out of the actual source.

---

## Touch input

Three separate traps, and like the panel ones they all fail quietly — the
screen simply feels broken rather than reporting anything.

**1. The FT5x06 NACKs register reads while idle.** Polling it unconditionally
produces a failed I2C transaction every time, and each failure costs a bus
timeout. Doing that once per frame stalled the whole loop from 25 fps to
roughly 0.3 fps, with the console filling with:

```
lcd_panel.io.i2c: panel_io_i2c_rx_buffer(149): i2c transaction failed
FT5x06: esp_lcd_touch_ft5x06_read_data(186): I2C read error!
```

Gate reads on the INT line (GPIO 15, active low). That is what it is for.

**2. INT means "data ready", not "finger down".** It drops briefly mid-touch,
so treating every deassertion as a release makes a held finger flicker between
pressed and released. Require a quiet period — 60 ms works — before declaring
the finger gone.

**3. microui encodes a mouse's interaction model, and touch cannot satisfy
it.** This is the subtle one. `mu_update_control()` only establishes hover on a
frame where the button is *not* held, and a control only submits once it has
focus:

```c
if (mouseover && !ctx->mouse_down) { ctx->hover = id; }
if (ctx->hover == id) { if (ctx->mouse_pressed) { mu_set_focus(ctx, id); } }
```

That is "point at it, then click" — hover on one frame, press on the next. A
touchscreen never produces the first half, because the pointer does not exist
until a finger is already down. Send move and press together and hover is never
set, focus is never taken, and the control never fires.

The fix is to synthesise the missing frame: on a press deliver only the
position, and let the button-down land on the following frame. That costs one
frame of latency (~40 ms, imperceptible) and makes taps reliable.

Worth knowing because it is not specific to buttons — every microui control
resolves interaction through `mu_update_control()`, so anything that reacts to
a press has the same requirement.

The symptom, before the fix, was that a deliberate press of roughly 120 ms
worked while a quick tap did nothing. It "worked" only because the flickering
INT line from trap 2 occasionally faked a not-down frame between two down
frames — one bug accidentally papering over another.

**Sampling rate matters too.** Reading touch once per rendered frame is too
coarse: a frame is ~40 ms here (the blit alone is 25 ms) and a quick tap can be
shorter than that, so taps fall between samples entirely. Poll on a separate
task — 100 Hz is plenty and costs nothing next to rendering — and latch the
press/release edges so an event that happens wholly between two frames is still
delivered to the next one.

**On targets and gestures.** A small back button is fine to aim at with a mouse
and miserable with a fingertip. A swipe up from the bottom edge — what the
board's stock firmware used — has no target to miss, cannot be triggered
accidentally mid-app, and leaves the app the whole screen. Trigger it partway
through the swipe rather than on release, or it feels sluggish.

---

## The IMU's axes do not match the screen's

`launcher/main/input/imu.c` drives the QMI8658 (WHO_AM_I `0x05` at `0x6b`), configured for
+/-8 g and +/-512 dps, which is where 4096 counts per g and 64 counts per dps
come from. All six axes come from one twelve-byte burst read at `0x35` - worth
doing as one transfer, because reading them separately can straddle a sample
update and produce a vector that never physically existed.

How the chip is soldered relative to the panel is a board fact no datasheet can
tell you, and the obvious guess is wrong here:

| Screen direction | Sensor axis |
|---|---|
| down (+y) | `+ax` |
| right (+x) | `-ay` |

Mapping X to X and Y to Y makes the sand fall sideways. Determined by tilting
the board and watching which way it went; the mapping lives in two macros at
the top of `main/apps/sand/app_sand.c`.

One more distinction that is easy to get wrong: the **accelerometer** senses
gravity, so it is what tilting changes and what tells you which way is down.
The **gyroscope** senses rotation *rate*, which is zero however far the board is
tilted as long as it is held still - it tells you the board is being shaken or
spun, nothing about orientation.

---

## The two buttons are not the same kind of device

Worth separating, because a single "read the button" abstraction over them
would be a lie.

**BOOT** is a plain GPIO — pin 9, pulled up, shorted to ground when pressed, so
low means down. It is a *level*, and being a mechanical contact it bounces on
both make and break: read naively, one press becomes several. `button_fsm.c`
debounces it (pure, host-tested, 25 ms) into press and release edges. It also
doubles as the flashing button - see
[Flashing-and-Toolchain.md](Flashing-and-Toolchain.md).

**PWR is not connected to the SoC.** It goes to the AXP2101 power-management
chip, so there is no pin to read. The PMU debounces in hardware and latches a
completed *event* in an interrupt-status register, which has to be fetched over
the shared I2C bus and then cleared. It is an event, not a level — reporting a
`down` state for it would be fiction.

The registers, from the X-Powers datasheet:

| | |
|---|---|
| Enable | `0x41` (INTEN2), bit 3 = power key short press |
| Status | `0x49` (INTSTS2), same bit |
| Clear | write a **one** back to the bit |
| Thresholds | `0x27` (IRQLEVEL/OFFLEVEL/ONLEVEL): bits 5:4 `irqlevel` = long-press IRQ threshold (`00`=1s, `01`=1.5s, `10`=2s, `11`=2.5s); bits 3:2 `offlevel` = power-off threshold (`00`=4s, `01`=6s, `10`=8s, `11`=10s); bits 1:0 `onlevel` = power-on threshold (`00`=128ms, `01`=512ms, `10`=1s, `11`=2s) |
| Power-off enable | `0x22` bit 1 `btn_pwroff_en` (`0`=disabled, `1`=enabled); bit 0 `btn_pwroff_mode` (`0`=power off, `1`=restart) when it fires |

Two traps in that. Clearing is write-one, not write-zero — the intuitive
"write 0 to clear" leaves the flag set and the button appears stuck down
forever. And clearing with `0xFF` would wipe every other latched event
(charging, battery insertion) that something else may care about, so clear only
the bit you consumed.

The long-press interrupt (1-2.5 s, `0x27` bits 5:4) and the PMU's own
power-off (4-10 s, `0x27` bits 3:2) are separate, independently configurable
thresholds, not the same event. Power-off is further gated by `0x22` bit 1,
which firmware can clear entirely. `0x22` and `0x27` default from EFUSE/POR, so
what a given board actually boots with is not knowable from the datasheet
alone; `buttons.c` reads and logs the decoded values once at startup so this
is checkable in the field rather than assumed. Today firmware enables only the
short-press interrupt and leaves the power-off enable at its default, so as
shipped a long PWR hold still cuts power - but that is a default left in
place, not a hardware limit firmware is powerless against.

Both arrive through `input_t` alongside touch, so an app never polls anything
itself. Polling runs at 50 Hz in its own task, deliberately decoupled from the
render loop — which now reaches 1000 fps and would hammer the shared I2C bus if
it read the PMU per frame.

---

## Raw sensor readings feel rigid, and it is not the sensor's fault

Feeding raw accelerometer output straight into anything feels bad, in two
distinct ways that need two distinct fixes. Worth separating, because fixing
only one leaves it feeling broken in the other.

**Noise and abruptness.** The sensor reports a few hundred counts of jitter on
a board sitting still, and a real tilt arrives as a step change. The fix is an
exponential moving average - a lerp toward the reading rather than a jump to it
(`main/apps/sand/tilt.c`). Two details matter more than the lerp:

- Define it by a **time constant**, not a per-frame fraction. "Move 10% each
  frame" changes meaning the moment the framerate does, and this project's has
  already gone 25 -> 43 -> 70 fps.
- Make it **adaptive using the gyroscope**. Heavy smoothing feels laggy when
  the board is genuinely moving; light smoothing feels noisy when it is not.
  The gyro reports rotation rate, which is near zero whenever the board is held
  still no matter how far it is tilted - so it says exactly when to stop
  smoothing and start tracking. Here the time constant slides from 260 ms when
  still to 40 ms when moving.

  This is the honest reason to read the gyro at all: it answers a question the
  accelerometer structurally cannot.

**Quantisation.** The second cause, and the larger one. Grains move to one of
eight neighbours, so a single simulation step can never express "17 degrees off
vertical" - and snapping to the nearest of eight makes a slow tilt arrive in
45-degree jerks. No amount of filtering helps: the filter output was already
smooth, and the quantiser threw that away.

The fix is to move the quantisation into **time**, where there is room for it.
Pick between the two directions bracketing the true angle each step, weighted by
the angle: at 17 degrees, about 62% of frames fall straight down and 38%
down-right. At 70 fps the eye integrates that into continuous flow. Exactly
dithering a colour ramp, applied to a direction, and it costs one random number
per step rather than per grain.

Getting the weight right needs `atan`, since at 22.5 degrees the component ratio
is 0.414 rather than 0.5. Rajan's approximation covers it in integers to within
a degree.

---

## An accelerometer does not measure gravity

It measures gravity plus whatever else is accelerating the device, and a single
reading cannot separate them. Pick the board up and the reading is mostly the
lift - which is what made the sand lurch sideways whenever it was handled.

The usable half-answer: at rest the magnitude is exactly 1 g. A sample whose
magnitude is far from 1 g is *known* to be contaminated, even though a clean one
cannot be proven honest. Those are ignored and the previous estimate held. Wide
bounds (0.7 g to 1.3 g), because rejecting a good sample costs a few
milliseconds of staleness and accepting a bad one throws sand across the screen.

This is also what makes the gyro-adaptive smoothing sound. Tracking *faster*
while the board moves would be exactly wrong if "moving" meant "being shoved" -
but rotation alone keeps the magnitude at 1 g, so genuine turning stays trusted
while handling fails the magnitude test. The two cases are separated, so
responding quickly to real rotation is safe.

---

## Rotating is not shaking, and the gyroscope cannot tell you which

These want opposite responses - a turn should be followed, a shake should
fluidise the pile - so telling them apart matters. The obvious sensor is the
wrong one.

Reading "shaken" off the gyroscope means every deliberate turn of the device
registers as a hard shake. In the sand app that unlocked friction *and* made
every grain prefer to slide sideways, so rotating the board threw the sand at
the walls. Logged while a board was merely being held and tilted, a
gyro-derived shake level sat between 160 and 255 out of 255 - effectively
pinned, the whole time.

Shaking means **accelerating** the device back and forth, and that is the
accelerometer's business. A smooth rotation keeps the magnitude at exactly 1 g
however fast it turns; shaking swings it far away. So the shake level is
derived from how far the magnitude departs from 1 g - the same quantity the
trust gate above already computes, read for its size rather than for which side
of a boundary it falls on.

The gyroscope keeps its honest job: saying when the board is genuinely turning,
which is what the filter's time constant responds to. Both sensors are used,
each for the question it can actually answer.

Two things this cost, worth knowing:

- **A rotation test has to preserve magnitude.** Sweeping `(k, ONE_G - k)` is
  not a rotation - it shrinks the vector to 0.71 g in the middle and reads as
  the device being dropped. Built from the 3-4-5 triangle instead, so the
  components stay exact in integers.
- **Shake smoothing must not be conditioned on the position filter priming.**
  A sample violent enough to be shaking is exactly one the trust gate rejects,
  so the position filter may never prime while the board is being shaken -
  tying the two together left the shake level unsmoothed and snapping to zero
  the instant the board was set down.

---

## Related

- [`../Sand/Simulation-Lessons.md`](../Sand/Simulation-Lessons.md) — how the sand app consumes
  these readings, including free-fall detection and the flat-device throttle.
- [Board-and-Memory.md](Board-and-Memory.md) — where the IMU and buttons sit
  in the board's hardware inventory.

// Browser-side driver for web_sand.c's exported wasm API. Mirrors
// app_sand.c's own frame loop (read gravity, step the sim at a fixed rate,
// pour/erase/detonate at a fixed rate, render) but in JS, since there is no
// gfx.h/microui/IMU driver here to do it in C - see web_sand.c's own top
// comment for exactly what stayed behind on the device.

const COUNTS_PER_G = 4096; // must match web_sand.c's WEB_COUNTS_PER_G

const MODE_PAINT = 0, MODE_ERASE = 1, MODE_DETONATE = 2;

const BRUSH_NAMES = [
  "Sand", "Water", "Stone", "Gas", "Fire", "Wood", "Oil", "Lava",
  "Acid", "Glass", "Snow", "Dirt", "Ice", "Plant",
];

const canvas = document.getElementById("canvas");
const ctx = canvas.getContext("2d", { alpha: false });
const loadingEl = document.getElementById("loading");
const paletteEl = document.getElementById("palette");
const sourceToggle = document.getElementById("source-toggle");
const clearBtn = document.getElementById("clear-btn");
const tiltBtn = document.getElementById("tilt-btn");
const tiltPad = document.getElementById("tilt-pad");
const tiltHandle = document.getElementById("tilt-handle");
const qualitySelect = document.getElementById("quality");
const modeButtons = [...document.querySelectorAll(".mode")];
const fsBtn = document.getElementById("fullscreen-btn");
const fsTarget = document.getElementById("app");
const orientationBtn = document.getElementById("orientation-btn");

let Module, web_init, web_step, web_input, web_clear, web_set_brush,
    web_brush_swatch, web_render, web_screen_w, web_screen_h, web_pixels_ptr;
let screenW = 448, screenH = 368;
let pixelsPtr = 0;
let mode = MODE_PAINT;
let brush = 0;

// LANDSCAPE by default - see web_sand.c's own comment on screen_w/screen_h
// for why this is a genuinely landscape-shaped grid (grid_w > grid_h), not
// a portrait grid rotated for display: gravity, the tilt pad and touch
// input all keep working completely unchanged either way.
let landscape = true;

let pointerDown = false;
let pointerJustPressed = false;
let pointerX = 0, pointerY = 0;

let tiltEnabled = false;               // real DeviceOrientation sensor, on
let tiltAx = 0, tiltAy = COUNTS_PER_G, tiltAz = 0;

let padActive = false;                 // the pad has been dragged at least once
let padAx = 0, padAy = COUNTS_PER_G, padAz = 0;

function toHex(rgb) {
  return "#" + (rgb >>> 0).toString(16).padStart(6, "0");
}

function buildPalette(count) {
  paletteEl.innerHTML = "";
  for (let i = 0; i < count; i++) {
    const el = document.createElement("div");
    el.className = "swatch" + (i === brush ? " selected" : "");
    el.style.background = toHex(web_brush_swatch(i));
    el.textContent = BRUSH_NAMES[i] || `#${i}`;
    el.addEventListener("pointerdown", (e) => {
      e.preventDefault();
      brush = i;
      web_set_brush(i);
      [...paletteEl.children].forEach((c, ci) =>
        c.classList.toggle("selected", ci === i));
    });
    paletteEl.appendChild(el);
  }
}

function setMode(next) {
  mode = next;
  modeButtons.forEach((b) => b.classList.toggle(
    "active", Number(b.dataset.mode) === mode));
}
modeButtons.forEach((b) =>
  b.addEventListener("click", () => setMode(Number(b.dataset.mode))));
setMode(MODE_PAINT);

clearBtn.addEventListener("click", () => web_clear && web_clear());

// DeviceOrientation, gated the same way iOS gates it: a button the user has
// to press, because reading it at all needs a permission prompt on iOS and
// is simply unavailable on a desktop browser - the fallback (fixed straight
// down) is what app_sand.c's own read_gravity_input() does when imu_ready()
// is false, so this page behaves the same way with no sensor at all.
//
// Toggles rather than a one-shot switch: permission is only ever requested
// once (iOS caches the grant for the page's lifetime, and re-prompting on
// every toggle would be obnoxious), but tiltEnabled itself flips freely so
// a press turns the sensor off again and hands gravity back to the pad.
let tiltListenerAttached = false;

function setTiltEnabled(enabled) {
  tiltEnabled = enabled;
  tiltBtn.textContent = enabled ? "Disable device tilt" : "Use device tilt";
  tiltPad.classList.toggle("disabled", enabled);
}

if (typeof DeviceOrientationEvent !== "undefined") {
  tiltBtn.hidden = false;
  tiltBtn.addEventListener("click", async () => {
    if (tiltListenerAttached) {
      setTiltEnabled(!tiltEnabled);
      return;
    }
    if (typeof DeviceOrientationEvent.requestPermission === "function") {
      try {
        const res = await DeviceOrientationEvent.requestPermission();
        if (res !== "granted") return;
      } catch {
        return;
      }
    }
    window.addEventListener("deviceorientation", onDeviceOrientation);
    tiltListenerAttached = true;
    setTiltEnabled(true);
  });
}

function onDeviceOrientation(e) {
  // Approximate, not a calibrated sensor fusion: gamma is left/right lean,
  // beta is front/back, both degrees. Good enough for "tilt the phone and
  // watch the sand slide", which is all this page claims to do.
  const gammaRad = (e.gamma || 0) * Math.PI / 180;
  const betaRad = (e.beta || 0) * Math.PI / 180;
  const gx = Math.max(-1, Math.min(1, Math.sin(gammaRad)));
  const gy = Math.max(-1, Math.min(1, Math.sin(betaRad)));
  const gzSq = Math.max(0, 1 - gx * gx - gy * gy);
  tiltAx = Math.round(gx * COUNTS_PER_G);
  tiltAy = Math.round(gy * COUNTS_PER_G);
  tiltAz = Math.round(Math.sqrt(gzSq) * COUNTS_PER_G);
}

// A virtual accelerometer for anything with no real one - a desktop, or a
// phone before/without the DeviceOrientation permission. Drag the handle the
// direction gravity should pull, same convention sand.h's own coordinate
// comment gives the simulation: x grows right, y grows down, so dragging
// right sets +ax and dragging down sets +ay - the same axes onDeviceOrientation
// feeds from a real sensor above. Unlike a spring-loaded joystick this does
// NOT snap back to centre on release: releasing the handle should read as
// "the device is now held at this angle", the same way letting go of an
// actually-tilted phone does not make it flat again - see the frame loop's
// own gravity-source priority for how this, tiltEnabled and the plain
// straight-down default are chosen between.
let padDragging = false;

function padVectorFromEvent(e) {
  const r = tiltPad.getBoundingClientRect();
  const cx = r.left + r.width / 2;
  const cy = r.top + r.height / 2;
  const radius = r.width / 2;
  let dx = (e.clientX - cx) / radius;
  let dy = (e.clientY - cy) / radius;
  const len = Math.hypot(dx, dy);
  if (len > 1) {
    dx /= len;
    dy /= len;
  }
  return [dx, dy];
}

function setPad(dx, dy) {
  padAx = Math.round(dx * COUNTS_PER_G);
  padAy = Math.round(dy * COUNTS_PER_G);
  padAz = Math.round(Math.sqrt(Math.max(0, 1 - dx * dx - dy * dy)) * COUNTS_PER_G);
  padActive = true;
  tiltPad.classList.add("live");
  const radius = tiltPad.getBoundingClientRect().width / 2;
  tiltHandle.style.transform = `translate(${dx * radius}px, ${dy * radius}px)`;
}

function resetPad() {
  padActive = false;
  padAx = 0;
  padAy = COUNTS_PER_G;
  padAz = 0;
  tiltPad.classList.remove("live");
  tiltHandle.style.transform = "";
}

tiltPad.addEventListener("pointerdown", (e) => {
  if (tiltEnabled) return; // a real sensor is in charge - see tiltBtn's own handler
  tiltPad.setPointerCapture(e.pointerId);
  tiltPad.classList.add("active");
  padDragging = true;
  setPad(...padVectorFromEvent(e));
});
tiltPad.addEventListener("pointermove", (e) => {
  if (!padDragging) return;
  setPad(...padVectorFromEvent(e));
});
["pointerup", "pointercancel"].forEach((ev) =>
  tiltPad.addEventListener(ev, () => {
    padDragging = false;
    tiltPad.classList.remove("active");
  }));
tiltPad.addEventListener("dblclick", resetPad);

function canvasPoint(clientX, clientY) {
  const r = canvas.getBoundingClientRect();
  const x = (clientX - r.left) / r.width * screenW;
  const y = (clientY - r.top) / r.height * screenH;
  return [
    Math.max(0, Math.min(screenW - 1, Math.round(x))),
    Math.max(0, Math.min(screenH - 1, Math.round(y))),
  ];
}

canvas.addEventListener("pointerdown", (e) => {
  canvas.setPointerCapture(e.pointerId);
  [pointerX, pointerY] = canvasPoint(e.clientX, e.clientY);
  pointerDown = true;
  pointerJustPressed = true;
});
canvas.addEventListener("pointermove", (e) => {
  if (!pointerDown) return;
  [pointerX, pointerY] = canvasPoint(e.clientX, e.clientY);
});
["pointerup", "pointercancel", "pointerleave"].forEach((ev) =>
  canvas.addEventListener(ev, () => { pointerDown = false; }));

// Most useful on a phone, where the browser chrome eats a lot of a small
// screen - but offered wherever the API exists. #app, not the canvas alone,
// so the palette/mode controls stay reachable while fullscreen.
const requestFs = fsTarget.requestFullscreen || fsTarget.webkitRequestFullscreen;
const exitFs = document.exitFullscreen || document.webkitExitFullscreen;
if (requestFs) {
  fsBtn.hidden = false;
  fsBtn.addEventListener("click", () => {
    if (document.fullscreenElement || document.webkitFullscreenElement) {
      exitFs.call(document);
    } else {
      requestFs.call(fsTarget).catch(() => {});
    }
  });
  ["fullscreenchange", "webkitfullscreenchange"].forEach((ev) =>
    document.addEventListener(ev, () => {
      const active = !!(document.fullscreenElement || document.webkitFullscreenElement);
      fsBtn.textContent = active ? "✕" : "⛶";
      fsBtn.setAttribute("aria-label", active ? "Exit fullscreen" : "Fullscreen");
    }));
}

// Shared by the quality selector, the orientation toggle, and the initial
// load below - anything that changes web_init()'s own two parameters goes
// through here, so the canvas's raster size/aspect-ratio and the palette's
// selection (web_init() resets brush_index to 0 internally, same as a
// fresh sand_enter() would) never drift out of step with whichever grid
// actually exists now.
function reinitSim() {
  web_init(Number(qualitySelect.value), landscape ? 1 : 0);
  screenW = web_screen_w();
  screenH = web_screen_h();
  // web_pixels_ptr() only returns a real address once web_init() has
  // allocated the buffer - which just happened, on the line above. The
  // buffer address is stable across every LATER web_init() call (see
  // web_pixels_ptr()'s own comment in web_sand.c), so re-reading it here
  // on every reinit is a formality, not a requirement - but it is what
  // makes that guarantee airtight rather than assumed.
  pixelsPtr = web_pixels_ptr();
  canvas.width = screenW;
  canvas.height = screenH;
  canvas.style.aspectRatio = `${screenW} / ${screenH}`;
  brush = 0;
  web_set_brush(0);
  [...paletteEl.children].forEach((c, ci) => c.classList.toggle("selected", ci === 0));
}

qualitySelect.addEventListener("change", reinitSim);

orientationBtn.addEventListener("click", () => {
  landscape = !landscape;
  orientationBtn.textContent = landscape ? "Portrait" : "Landscape";
  reinitSim();
});

let lastFrame = 0;

function frame(now) {
  requestAnimationFrame(frame);
  if (!web_step) return;

  let dt = lastFrame ? now - lastFrame : 16;
  lastFrame = now;
  dt = Math.max(1, Math.min(dt, 100)); // same TILT_MAX_DT_MS-style clamp as the device

  // Real sensor first, then the virtual pad (see its own comment for why it
  // does not reset on release), then plain straight down - the same
  // fallback app_sand.c's read_gravity_input() uses when imu_ready() is
  // false.
  let ax, ay, az;
  if (tiltEnabled) {
    [ax, ay, az] = [tiltAx, tiltAy, tiltAz];
  } else if (padActive) {
    [ax, ay, az] = [padAx, padAy, padAz];
  } else {
    [ax, ay, az] = [0, COUNTS_PER_G, 0];
  }
  const rotation = 0;

  web_step(dt, ax, ay, az, rotation);
  // A press implies "down" for the frame it happens on even if the
  // matching release already landed before this frame ran - a mousedown
  // immediately followed by a mouseup in the same JS tick (a fast real tap,
  // or a synthetic click) would otherwise read pointerDown as already
  // false here, losing the press edge entirely. web_input()'s own emitter-
  // placement branch returns early on `!down`, so this matters for more
  // than bookkeeping.
  const down = pointerDown || pointerJustPressed;
  web_input(mode, down ? 1 : 0, pointerJustPressed ? 1 : 0,
    sourceToggle.checked ? 1 : 0, pointerX, pointerY, dt);
  pointerJustPressed = false;

  web_render();
  // A fresh view every frame: ALLOW_MEMORY_GROWTH means wasm memory can be
  // reallocated to a new ArrayBuffer, which would detach any view held
  // across that - see web_sand.c's web_pixels_ptr() for why the pointer
  // itself stays valid regardless.
  const bytes = new Uint8ClampedArray(
    Module.HEAPU8.buffer, pixelsPtr, screenW * screenH * 4);
  ctx.putImageData(new ImageData(bytes, screenW, screenH), 0, 0);
}

SandModule().then((mod) => {
  Module = mod;
  web_init = Module.cwrap("web_init", "number", ["number", "number"]);
  web_step = Module.cwrap("web_step", null,
    ["number", "number", "number", "number", "number"]);
  web_input = Module.cwrap("web_input", null,
    ["number", "number", "number", "number", "number", "number", "number"]);
  web_clear = Module.cwrap("web_clear", null, []);
  web_set_brush = Module.cwrap("web_set_brush", null, ["number"]);
  web_brush_swatch = Module.cwrap("web_brush_swatch", "number", ["number"]);
  web_screen_w = Module.cwrap("web_screen_w", "number", []);
  web_screen_h = Module.cwrap("web_screen_h", "number", []);
  const web_brush_count = Module.cwrap("web_brush_count", "number", []);

  web_render = Module.cwrap("web_render", null, []);
  web_pixels_ptr = Module.cwrap("web_pixels_ptr", "number", []);

  buildPalette(web_brush_count());
  reinitSim();

  loadingEl.hidden = true;
  requestAnimationFrame(frame);
});

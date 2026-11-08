(() => {
  "use strict";

  // The simulation and the ASCII scene are produced in Rust/WebAssembly. The
  // browser is just a window, input bridge, and monospace character painter.
  const COLS = 120;
  const ROWS = 72;
  const CELL_W = 6;
  const CELL_H = 7.5; // 120×72 cells in a true 720×540 (4:3) buffer.
  const GLYPH_H = 8;
  const PALETTE = [
    "#111712", "#1a211a", "#242d23", "#343a2c", "#454936",
    "#565a40", "#707157", "#8b896a", "#a49d7c", "#beb393",
    "#d7ccac", "#f1e7cb", "#574530", "#ba8e60", "#eac28a",
    "#92ac83", "#c97458", "#3a4938", "#d1f0ad", "#6e7666",
  ];
  const KEY = {
    KeyW: 1 << 0, KeyS: 1 << 1, KeyA: 1 << 2, KeyD: 1 << 3,
    ArrowLeft: 1 << 4, ArrowRight: 1 << 5, ArrowUp: 1 << 6, ArrowDown: 1 << 7,
    ShiftLeft: 1 << 8, ShiftRight: 1 << 8, KeyF: 1 << 9,
  };
  const FIRE = 1 << 9;
  const game = document.getElementById("game");
  const canvas = document.getElementById("screen");
  const ctx = canvas.getContext("2d", { alpha: false });
  const loading = document.getElementById("loading");
  const hint = document.getElementById("click-hint");
  const hintTitle = document.getElementById("hint-title");
  const hintSubtitle = document.getElementById("hint-subtitle");
  const speedFill = document.getElementById("speed-fill");
  const movementStatus = document.getElementById("movement-status");
  const feedback = document.getElementById("shot-feedback");
  const settings = document.getElementById("settings-panel");
  const settingsButton = document.getElementById("settings-button");
  const sensitivityInput = document.getElementById("sensitivity");
  const sensitivityValue = document.getElementById("sensitivity-value");
  const resetButton = document.getElementById("reset");
  let wasm;
  let mouseX = 0;
  let mouseY = 0;
  let mouseFire = false;
  let queuedShot = false;
  let heldKeys = new Set();
  let lastTime;
  let fallback = false;
  let pendingLock = false;
  let sensitivity = 1;
  let previousResult = -1;

  try {
    const saved = Number(localStorage.getItem("strafe-lab-sensitivity"));
    if (Number.isFinite(saved) && saved >= 0.1 && saved <= 8) sensitivity = saved;
  } catch (_) { /* Settings storage can be blocked; play still works. */ }
  sensitivityInput.value = String(sensitivity);
  sensitivityValue.value = sensitivity.toFixed(2);

  function setSettings(open) {
    settings.hidden = !open;
    settingsButton.setAttribute("aria-expanded", String(open));
    if (open && document.pointerLockElement === game) document.exitPointerLock();
  }
  settingsButton.addEventListener("click", () => setSettings(settings.hidden));
  document.getElementById("settings-close").addEventListener("click", () => {
    setSettings(false);
    settingsButton.focus();
  });
  sensitivityInput.addEventListener("input", () => {
    sensitivity = Number(sensitivityInput.value);
    sensitivityValue.value = sensitivity.toFixed(2);
    try { localStorage.setItem("strafe-lab-sensitivity", String(sensitivity)); } catch (_) {}
  });

  function reset() {
    if (wasm) wasm.game_reset();
    heldKeys.clear();
    mouseFire = false;
    queuedShot = false;
    mouseX = mouseY = 0;
    feedback.textContent = "";
    previousResult = -1;
    game.focus({ preventScroll: true });
  }
  resetButton.addEventListener("click", reset);

  function clearInput() {
    heldKeys.clear();
    mouseFire = false;
    queuedShot = false;
    mouseX = mouseY = 0;
  }
  document.addEventListener("keydown", (event) => {
    if (event.code === "Escape") {
      if (!settings.hidden) setSettings(false);
      if (document.pointerLockElement === game) document.exitPointerLock();
      clearInput();
      return;
    }
    if (event.target instanceof HTMLInputElement || event.target instanceof HTMLButtonElement) return;
    if (event.code === "KeyR") {
      event.preventDefault();
      if (!event.repeat) reset();
      return;
    }
    if (Object.prototype.hasOwnProperty.call(KEY, event.code)) {
      event.preventDefault();
      heldKeys.add(event.code);
      // A very quick tap can begin and end between animation frames.
      if (event.code === "KeyF" && !event.repeat) queuedShot = true;
    }
  });
  document.addEventListener("keyup", (event) => {
    heldKeys.delete(event.code);
  });
  window.addEventListener("blur", clearInput);
  document.addEventListener("visibilitychange", () => {
    if (document.hidden) clearInput();
    lastTime = undefined;
  });

  function useFallback() {
    pendingLock = false;
    fallback = true;
    hint.hidden = false;
    hintTitle.textContent = "МЫШЬ НЕ ЗАХВАЧЕНА";
    hintSubtitle.textContent = "стрелки — обзор, F или ЛКМ — выстрел";
  }
  async function captureMouse() {
    if (!game.requestPointerLock) return useFallback();
    pendingLock = true;
    try {
      // Raw movement avoids OS acceleration where the browser permits it.
      try {
        await game.requestPointerLock({ unadjustedMovement: true });
      } catch (err) {
        // Some browsers do not support raw input, but do support pointer lock.
        if (err && err.name === "NotSupportedError") await game.requestPointerLock();
        else throw err;
      }
    } catch (_) {
      useFallback();
    } finally {
      pendingLock = false;
    }
  }
  game.addEventListener("pointerdown", (event) => {
    if (event.button !== 0 || !wasm) return;
    event.preventDefault();
    game.focus({ preventScroll: true });
    if (document.pointerLockElement === game || fallback) {
      mouseFire = true;
      queuedShot = true;
    } else if (!pendingLock) captureMouse();
  });
  document.addEventListener("pointerup", (event) => {
    if (event.button === 0) mouseFire = false;
  });
  document.addEventListener("pointerlockchange", () => {
    const locked = document.pointerLockElement === game;
    game.classList.toggle("locked", locked);
    if (locked) {
      // Some browsers emit a synthetic warp movement before lockchange.
      // Discard it so clicking the field does not jerk the crosshair.
      mouseX = mouseY = 0;
      fallback = false;
      hint.hidden = true;
    } else {
      clearInput();
      if (!fallback) {
        hint.hidden = false;
        hintTitle.textContent = "КЛИКНИ ПО ПОЛЮ";
        hintSubtitle.textContent = "чтобы захватить мышь и начать";
      }
    }
  });
  document.addEventListener("pointerlockerror", () => {
    if (!pendingLock && document.pointerLockElement !== game) useFallback();
  });
  document.addEventListener("mousemove", (event) => {
    if (document.pointerLockElement === game && game.classList.contains("locked")) {
      mouseX += event.movementX || 0;
      mouseY += event.movementY || 0;
    }
  });
  game.addEventListener("contextmenu", (event) => event.preventDefault());

  function inputMask() {
    let mask = (mouseFire || queuedShot) ? FIRE : 0;
    for (const code of heldKeys) mask |= KEY[code] || 0;
    return mask;
  }

  // Cache glyph pixels once. Rendering each of 8,640 characters with fillText
  // every frame is far too slow on some machines; composing the cached cells
  // into one ImageData buffer keeps the trainer responsive without a GPU.
  const glyphCanvas = document.createElement("canvas");
  glyphCanvas.width = 128 * CELL_W;
  glyphCanvas.height = GLYPH_H;
  const glyphContext = glyphCanvas.getContext("2d", { willReadFrequently: true });
  glyphContext.font = '8px "Courier New", ui-monospace, monospace';
  glyphContext.textBaseline = "top";
  const glyphWidth = glyphContext.measureText("M").width;
  glyphContext.setTransform(CELL_W / glyphWidth, 0, 0, 1, 0, 0);
  glyphContext.fillStyle = "#fff";
  for (let code = 32; code <= 126; code++) {
    glyphContext.fillText(String.fromCharCode(code), code * glyphWidth, 0.5);
  }
  const atlas = glyphContext.getImageData(0, 0, glyphCanvas.width, GLYPH_H).data;
  const masks = Array.from({ length: 128 }, (_, code) => {
    const mask = new Uint8Array(CELL_W * GLYPH_H);
    for (let y = 0; y < GLYPH_H; y++) {
      for (let x = 0; x < CELL_W; x++) {
        mask[y * CELL_W + x] = atlas[(y * glyphCanvas.width + code * CELL_W + x) * 4 + 3];
      }
    }
    return mask;
  });
  const rgb = PALETTE.map((color) => [
    parseInt(color.slice(1, 3), 16), parseInt(color.slice(3, 5), 16), parseInt(color.slice(5, 7), 16),
  ]);
  const glyphCache = new Map();
  const image = ctx.createImageData(canvas.width, canvas.height);
  const pixels = new Uint32Array(image.data.buffer);

  function cellPixels(glyph, fg, bg) {
    const key = (fg * PALETTE.length + bg) * 128 + glyph;
    let cached = glyphCache.get(key);
    if (cached) return cached;
    cached = new Uint32Array(CELL_W * GLYPH_H);
    const fore = rgb[fg] || rgb[0];
    const back = rgb[bg] || rgb[0];
    const mask = masks[glyph] || masks[32];
    for (let i = 0; i < cached.length; i++) {
      const a = mask[i];
      const r = (back[0] * (255 - a) + fore[0] * a + 127) / 255 | 0;
      const g = (back[1] * (255 - a) + fore[1] * a + 127) / 255 | 0;
      const b = (back[2] * (255 - a) + fore[2] * a + 127) / 255 | 0;
      // ImageData is RGBA, represented by little-endian 32-bit pixels.
      cached[i] = (0xff000000 | (b << 16) | (g << 8) | r) >>> 0;
    }
    glyphCache.set(key, cached);
    return cached;
  }

  function paint(cells) {
    for (let row = 0; row < ROWS; row++) {
      for (let col = 0; col < COLS; col++) {
        const offset = (row * COLS + col) * 4;
        const cell = cellPixels(cells[offset], cells[offset + 1], cells[offset + 2]);
        const top = Math.floor(row * CELL_H);
        const height = Math.floor((row + 1) * CELL_H) - top;
        let dst = top * canvas.width + col * CELL_W;
        for (let y = 0; y < height; y++) {
          const src = y * CELL_W;
          pixels[dst] = cell[src];
          pixels[dst + 1] = cell[src + 1];
          pixels[dst + 2] = cell[src + 2];
          pixels[dst + 3] = cell[src + 3];
          pixels[dst + 4] = cell[src + 4];
          pixels[dst + 5] = cell[src + 5];
          dst += canvas.width;
        }
      }
    }
    ctx.putImageData(image, 0, 0);
  }

  function updateHud() {
    const speed = wasm.game_speed();
    const accurate = Boolean(wasm.game_accurate());
    speedFill.style.width = `${Math.max(0, Math.min(100, speed * 100))}%`;
    speedFill.classList.toggle("moving", !accurate);
    movementStatus.classList.toggle("moving", !accurate);
    movementStatus.textContent = accurate ? "ТОЧНЫЙ ВЫСТРЕЛ" : "В ДВИЖЕНИИ";
    const result = wasm.game_result();
    if (result !== previousResult) {
      feedback.textContent = ["", "ПРОМАХ", "ПОПАДАНИЕ", "ПОПАДАНИЕ В ГОЛОВУ"][result] || "";
      previousResult = result;
    }
  }

  function frame(now) {
    const dt = lastTime === undefined ? 0 : Math.min((now - lastTime) / 1000, 0.05);
    lastTime = now;
    const ptr = wasm.game_frame(dt, inputMask(), mouseX, mouseY, sensitivity);
    queuedShot = false;
    mouseX = mouseY = 0;
    paint(new Uint8Array(wasm.memory.buffer, ptr, COLS * ROWS * 4));
    updateHud();
    requestAnimationFrame(frame);
  }

  async function start() {
    try {
      const response = await fetch("/engine.wasm");
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const bytes = await response.arrayBuffer();
      const module = await WebAssembly.instantiate(bytes, {
        env: { sinf: Math.sin, cosf: Math.cos, sqrtf: Math.sqrt },
      });
      wasm = module.instance.exports;
      loading.hidden = true;
      requestAnimationFrame(frame);
    } catch (error) {
      console.error("Не удалось запустить Rust-движок:", error);
      loading.classList.add("error");
      loading.textContent = "НЕ УДАЛОСЬ ЗАГРУЗИТЬ ДВИЖОК. Запустите приложение через cargo run и откройте адрес из терминала.";
    }
  }
  start();
})();

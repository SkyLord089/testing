(() => {
  "use strict";

  // Rust supplies the complete ASCII scene; this page is only its window and input bridge.
  const COLS = 160;
  const ROWS = 96;
  const CELL_W = 6;
  const CELL_H = 7.5; // 960×720 logical 4:3, stretched by CSS to 16:9.
  const GLYPH_H = 8;
  const PALETTE = [
    "#101510", "#171f18", "#212b21", "#303a2b", "#414b37",
    "#566048", "#6d765a", "#8b9375", "#aab096", "#c8cbb3",
    "#e1e1c7", "#f2ecd7", "#574530", "#ba8e60", "#eac28a",
    "#92ac83", "#c97458", "#3a4938", "#d1f0ad", "#6e7666",
    "#111512", "#242b25", "#49554b", "#8b998a", "#c2cbba",
    "#393729", "#5c5640", "#988b69", "#d3c19a", "#4b3020",
    "#916344", "#dfaa71", "#292719", "#413b26", "#5b5236",
    "#756947", "#92845d", "#afa17b", "#cbbd97",
  ];
  const KEY = {
    KeyW: 1 << 0, KeyS: 1 << 1, KeyA: 1 << 2, KeyD: 1 << 3,
    ShiftLeft: 1 << 8, ShiftRight: 1 << 8, KeyF: 1 << 9,
  };
  const FIRE = 1 << 9;
  const game = document.getElementById("game");
  const canvas = document.getElementById("screen");
  const ctx = canvas.getContext("2d", { alpha: false });
  const errorElement = document.getElementById("error");
  const heldKeys = new Set();
  let wasm;
  let mouseFire = false;
  let queuedShot = false;
  let lastTime;

  function clearInput() {
    heldKeys.clear();
    mouseFire = false;
    queuedShot = false;
  }
  document.addEventListener("keydown", (event) => {
    if (event.ctrlKey || event.metaKey || event.altKey) return;
    if (event.code === "KeyR") {
      event.preventDefault();
      if (!event.repeat && wasm) {
        wasm.game_reset();
        clearInput();
      }
      return;
    }
    if (Object.prototype.hasOwnProperty.call(KEY, event.code)) {
      event.preventDefault();
      heldKeys.add(event.code);
      // Preserve a quick tap even if it starts and ends between animation frames.
      if (event.code === "KeyF" && !event.repeat) queuedShot = true;
    }
  });
  document.addEventListener("keyup", (event) => heldKeys.delete(event.code));
  game.addEventListener("pointerdown", (event) => {
    if (event.button !== 0) return;
    event.preventDefault();
    game.focus({ preventScroll: true });
    mouseFire = true;
    queuedShot = true;
  });
  document.addEventListener("pointerup", (event) => {
    if (event.button === 0) mouseFire = false;
  });
  window.addEventListener("blur", clearInput);
  document.addEventListener("visibilitychange", () => {
    if (document.hidden) clearInput();
    lastTime = undefined;
  });

  function inputMask() {
    let mask = mouseFire || queuedShot ? FIRE : 0;
    for (const code of heldKeys) mask |= KEY[code] || 0;
    return mask;
  }

  // Cache glyph pixels once instead of drawing 15,360 characters per frame.
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

  function frame(now) {
    const dt = lastTime === undefined ? 0 : Math.min((now - lastTime) / 1000, 0.05);
    lastTime = now;
    const ptr = wasm.game_frame(dt, inputMask());
    queuedShot = false;
    paint(new Uint8Array(wasm.memory.buffer, ptr, COLS * ROWS * 4));
    requestAnimationFrame(frame);
  }

  async function start() {
    try {
      const response = await fetch("/engine.wasm");
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      const bytes = await response.arrayBuffer();
      const module = await WebAssembly.instantiate(bytes, { env: { sqrtf: Math.sqrt } });
      wasm = module.instance.exports;
      requestAnimationFrame(frame);
    } catch (error) {
      console.error("Не удалось запустить Rust-движок:", error);
      errorElement.textContent = "Не удалось загрузить комнату. Откройте адрес, напечатанный в терминале приложения.";
      errorElement.hidden = false;
    }
  }
  start();
})();

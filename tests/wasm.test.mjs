// Tests the checked-in Rust/WASM scene using only Node's built-in modules.
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';

const bytes = await readFile(new URL('../web/engine.wasm', import.meta.url));
const { instance } = await WebAssembly.instantiate(bytes, { env: { sqrtf: Math.sqrt } });
const engine = instance.exports;
const COLS = 160;
const ROWS = 96;
const dt = 1 / 120;
const left = 1 << 2;
const right = 1 << 3;
const fire = 1 << 9;

function step(input = 0, count = 1) {
  let pointer;
  for (let i = 0; i < count; i++) pointer = engine.game_frame(dt, input);
  return new Uint8Array(engine.memory.buffer, pointer, COLS * ROWS * 4).slice();
}

test('the prebuilt engine returns a complete ASCII room without a center overlay', () => {
  engine.game_reset();
  const cells = step();
  assert.equal(cells.length, COLS * ROWS * 4);
  for (let i = 0; i < cells.length; i += 4) {
    assert.ok(cells[i] >= 32 && cells[i] <= 126);
    assert.ok(cells[i + 1] < 39 && cells[i + 2] < 39);
  }
  for (let y = ROWS / 2 - 1; y <= ROWS / 2 + 1; y++) {
    for (let x = COLS / 2 - 2; x <= COLS / 2 + 2; x++) {
      assert.ok(cells[(y * COLS + x) * 4 + 2] <= 4, 'center is part of the room');
    }
  }
});

test('the view is fixed, while strafing changes the room and reset restores it', () => {
  engine.game_reset();
  const initial = step();
  const oldLookKeys = (1 << 4) | (1 << 5) | (1 << 6) | (1 << 7);
  assert.deepEqual(step(oldLookKeys, 30), initial);
  const moved = step(left, 35);
  assert.ok(engine.game_speed() > 0.8);
  assert.notDeepEqual(moved.slice(0, COLS * 40 * 4), initial.slice(0, COLS * 40 * 4));
  engine.game_reset();
  assert.equal(engine.game_speed(), 0);
  assert.deepEqual(step(), initial);
});

test('counter-strafing slows movement and a shot is only a temporary flash', () => {
  engine.game_reset();
  step(left, 35);
  const before = engine.game_speed();
  step(right, 5);
  assert.ok(engine.game_speed() < before);

  engine.game_reset();
  const initial = step();
  assert.notDeepEqual(step(fire), initial);
  assert.deepEqual(step(0, 20), initial);
});

test('the page has no visible HUD, control hints, or pointer-lock code', async () => {
  const html = await readFile(new URL('../web/index.html', import.meta.url), 'utf8');
  const js = await readFile(new URL('../web/app.js', import.meta.url), 'utf8');
  assert.match(html, /<canvas\b/);
  assert.doesNotMatch(html, /<button\b|<input\b|<kbd\b|class="(?:controls|top-overlay|bottom-overlay|click-hint)/i);
  assert.doesNotMatch(js, /requestPointerLock|pointerlockchange|mousemove/);
});

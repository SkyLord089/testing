// Optional smoke test for the prebuilt browser engine; uses only Node built-ins.
import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';

const bytes = await readFile(new URL('../web/engine.wasm', import.meta.url));
const { instance } = await WebAssembly.instantiate(bytes, {
  env: { sinf: Math.sin, cosf: Math.cos, sqrtf: Math.sqrt },
});
const engine = instance.exports;
const dt = 1 / 120;
const left = 1 << 2;
const right = 1 << 3;
const fire = 1 << 9;

function step(input = 0, count = 1) {
  let pointer;
  for (let i = 0; i < count; i++) pointer = engine.game_frame(dt, input, 0, 0, 1);
  return pointer;
}

test('prebuilt Rust engine returns a complete ASCII frame', () => {
  engine.game_reset();
  const pointer = step();
  const cells = new Uint8Array(engine.memory.buffer, pointer, 120 * 72 * 4);
  assert.equal(cells.length, 34560);
  for (let i = 0; i < cells.length; i += 4) {
    assert.ok(cells[i] >= 32 && cells[i] <= 126);
    assert.ok(cells[i + 1] < 20 && cells[i + 2] < 20);
  }
});

test('a stationary shot hits the visible head, and result is transient', () => {
  engine.game_reset();
  step(fire);
  assert.equal(engine.game_result(), 3);
  step(0, 100);
  assert.equal(engine.game_result(), 0);
});

test('counter-strafing changes accuracy and reset clears velocity', () => {
  engine.game_reset();
  step(left, 35);
  assert.ok(engine.game_speed() > 0.8);
  assert.equal(engine.game_accurate(), 0);
  let frames = 0;
  while (!engine.game_accurate() && frames++ < 60) step(right);
  assert.ok(frames < 60);
  engine.game_reset();
  assert.equal(engine.game_speed(), 0);
  assert.equal(engine.game_accurate(), 1);
});

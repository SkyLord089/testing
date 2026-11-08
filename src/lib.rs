//! First-person ASCII room and movement simulation. The browser only paints the
//! character cells and forwards input; all scene geometry is drawn in Rust.
#![cfg_attr(target_arch = "wasm32", no_std)]

#[cfg(target_arch = "wasm32")]
use core::panic::PanicInfo;

#[cfg(target_arch = "wasm32")]
#[panic_handler]
fn panic(_info: &PanicInfo<'_>) -> ! {
    core::arch::wasm32::unreachable()
}

#[cfg(target_arch = "wasm32")]
#[link(wasm_import_module = "env")]
extern "C" {
    fn sqrtf(x: f32) -> f32;
}

#[inline]
fn sqrt(x: f32) -> f32 {
    #[cfg(target_arch = "wasm32")]
    unsafe {
        sqrtf(x)
    }
    #[cfg(not(target_arch = "wasm32"))]
    {
        x.sqrt()
    }
}

fn floor_i(x: f32) -> i32 {
    let truncated = x as i32;
    if x < truncated as f32 {
        truncated - 1
    } else {
        truncated
    }
}

fn fract(x: f32) -> f32 {
    x - floor_i(x) as f32
}

pub const COLS: usize = 160;
pub const ROWS: usize = 96;
pub const CELL_COUNT: usize = COLS * ROWS;

const WEAPON_WIDTH: usize = 100;
const WEAPON_HEIGHT: usize = 56;
const WEAPON_GLYPHS: &str = include_str!("../assets/weapon-glyphs.txt");
const WEAPON_COLORS: &str = include_str!("../assets/weapon-colors.txt");

/// Layout consumed by web/app.js: one ASCII glyph and two palette indices.
#[repr(C)]
#[derive(Copy, Clone, Debug, PartialEq, Eq)]
pub struct Cell {
    pub glyph: u8,
    pub fg: u8,
    pub bg: u8,
    pub _reserved: u8,
}

impl Cell {
    const fn new(glyph: u8, fg: u8, bg: u8) -> Self {
        Self {
            glyph,
            fg,
            bg,
            _reserved: 0,
        }
    }
}

pub mod keys {
    pub const FORWARD: u32 = 1 << 0;
    pub const BACK: u32 = 1 << 1;
    pub const LEFT: u32 = 1 << 2;
    pub const RIGHT: u32 = 1 << 3;
    pub const WALK: u32 = 1 << 8;
    pub const FIRE: u32 = 1 << 9;
}

// Source-inspired movement, deliberately approximate rather than CS2 data.
const MAX_SPEED: f32 = 4.3;
const WALK_SPEED: f32 = 2.24;
const ACCEL: f32 = 5.5;
const FRICTION: f32 = 5.2;
const STOP_SPEED: f32 = 1.6;
const PLAYER_RADIUS: f32 = 0.22;
const EYE_HEIGHT: f32 = 1.55;
const WALL_HEIGHT: f32 = 3.4;
// A 90-degree horizontal FOV in a 4:3 logical frame.
const PROJECTION: f32 = 64.0;
const SPAWN_X: f32 = 7.0;
const SPAWN_Y: f32 = 9.4;

// A closed room with no target, props, or obstacles.
const MAP: [&[u8; 14]; 12] = [
    b"##############",
    b"#............#",
    b"#............#",
    b"#............#",
    b"#............#",
    b"#............#",
    b"#............#",
    b"#............#",
    b"#............#",
    b"#............#",
    b"#............#",
    b"##############",
];

fn tile(x: i32, y: i32) -> u8 {
    if x < 0 || y < 0 || x >= 14 || y >= 12 {
        b'#'
    } else {
        MAP[y as usize][x as usize]
    }
}

fn is_open(x: f32, y: f32) -> bool {
    for dx in [-PLAYER_RADIUS, PLAYER_RADIUS] {
        for dy in [-PLAYER_RADIUS, PLAYER_RADIUS] {
            if tile(floor_i(x + dx), floor_i(y + dy)) != b'.' {
                return false;
            }
        }
    }
    true
}

#[derive(Clone, Copy, Debug)]
struct Hit {
    distance: f32,
    along: f32,
    side: u8,
}

/// DDA in map space. Camera-ray parameters are perpendicular projection depth.
fn cast_ray(px: f32, py: f32, dx: f32, dy: f32) -> Hit {
    let mut mx = floor_i(px);
    let mut my = floor_i(py);
    let delta_x = if dx.abs() < 0.00001 {
        1.0e20
    } else {
        (1.0 / dx).abs()
    };
    let delta_y = if dy.abs() < 0.00001 {
        1.0e20
    } else {
        (1.0 / dy).abs()
    };
    let step_x = if dx < 0.0 { -1 } else { 1 };
    let step_y = if dy < 0.0 { -1 } else { 1 };
    let mut side_x = if dx < 0.0 {
        (px - mx as f32) * delta_x
    } else {
        (mx as f32 + 1.0 - px) * delta_x
    };
    let mut side_y = if dy < 0.0 {
        (py - my as f32) * delta_y
    } else {
        (my as f32 + 1.0 - py) * delta_y
    };
    for _ in 0..64 {
        let (distance, side) = if side_x < side_y {
            mx += step_x;
            let distance = side_x;
            side_x += delta_x;
            (distance, 0)
        } else {
            my += step_y;
            let distance = side_y;
            side_y += delta_y;
            (distance, 1)
        };
        if tile(mx, my) != b'.' {
            return Hit {
                distance: distance.max(0.001),
                along: if side == 0 {
                    py + distance * dy
                } else {
                    px + distance * dx
                },
                side,
            };
        }
    }
    Hit {
        distance: 64.0,
        along: 0.0,
        side: 0,
    }
}

#[inline]
fn hash(x: i32, y: i32, salt: u32) -> u32 {
    let mut n = (x as u32).wrapping_mul(0x8da6_b343)
        ^ (y as u32).wrapping_mul(0xd816_3841)
        ^ salt.wrapping_mul(0xcb1a_b31f);
    n ^= n >> 13;
    n = n.wrapping_mul(0x85eb_ca6b);
    n ^ (n >> 16)
}

pub struct Game {
    pub x: f32,
    pub y: f32,
    pub vx: f32,
    pub vy: f32,
    cooldown: f32,
    muzzle: f32,
    cells: [Cell; CELL_COUNT],
}

impl Default for Game {
    fn default() -> Self {
        Self::new()
    }
}

impl Game {
    pub const fn new() -> Self {
        Self {
            x: SPAWN_X,
            y: SPAWN_Y,
            vx: 0.0,
            vy: 0.0,
            cooldown: 0.0,
            muzzle: 0.0,
            cells: [Cell::new(b' ', 0, 0); CELL_COUNT],
        }
    }

    pub fn reset(&mut self) {
        self.x = SPAWN_X;
        self.y = SPAWN_Y;
        self.vx = 0.0;
        self.vy = 0.0;
        self.cooldown = 0.0;
        self.muzzle = 0.0;
    }

    pub fn speed(&self) -> f32 {
        sqrt(self.vx * self.vx + self.vy * self.vy)
    }

    /// The view is fixed straight ahead. Inputs move the player, never the camera angle.
    pub fn update(&mut self, dt: f32, input: u32) {
        let dt = if dt.is_finite() {
            dt.clamp(0.0, 0.05)
        } else {
            0.0
        };
        self.cooldown = (self.cooldown - dt).max(0.0);
        self.muzzle = (self.muzzle - dt).max(0.0);

        let speed = self.speed();
        if speed > 0.0 {
            let next = (speed - speed.max(STOP_SPEED) * FRICTION * dt).max(0.0) / speed;
            self.vx *= next;
            self.vy *= next;
        }
        let forward =
            ((input & keys::FORWARD != 0) as i32 - (input & keys::BACK != 0) as i32) as f32;
        let strafe = ((input & keys::RIGHT != 0) as i32 - (input & keys::LEFT != 0) as i32) as f32;
        let magnitude = sqrt(forward * forward + strafe * strafe);
        if magnitude > 0.0 {
            let wx = strafe / magnitude;
            let wy = -forward / magnitude;
            let wish_speed = if input & keys::WALK != 0 {
                WALK_SPEED
            } else {
                MAX_SPEED
            };
            let add_speed = wish_speed - (self.vx * wx + self.vy * wy);
            if add_speed > 0.0 {
                let acceleration = (ACCEL * wish_speed * dt).min(add_speed);
                self.vx += acceleration * wx;
                self.vy += acceleration * wy;
            }
        }
        let nx = self.x + self.vx * dt;
        if is_open(nx, self.y) {
            self.x = nx;
        } else {
            self.vx = 0.0;
        }
        let ny = self.y + self.vy * dt;
        if is_open(self.x, ny) {
            self.y = ny;
        } else {
            self.vy = 0.0;
        }

        // Firing is visual only: there is deliberately no target or hit feedback.
        if input & keys::FIRE != 0 && self.cooldown <= 0.0 {
            self.cooldown = 0.10;
            self.muzzle = 0.055;
        }
    }

    fn put(&mut self, x: i32, y: i32, glyph: u8, fg: u8, bg: u8) {
        if x >= 0 && y >= 0 && x < COLS as i32 && y < ROWS as i32 {
            self.cells[y as usize * COLS + x as usize] = Cell::new(glyph, fg, bg);
        }
    }

    /// Pre-composed ASCII cells for the close viewmodel. Transparent cells
    /// preserve the ray-cast room behind it.
    fn render_weapon(&mut self) {
        for (row, (glyphs, colors)) in WEAPON_GLYPHS
            .lines()
            .zip(WEAPON_COLORS.lines())
            .take(WEAPON_HEIGHT)
            .enumerate()
        {
            for (col, (glyph, code)) in glyphs
                .bytes()
                .zip(colors.bytes())
                .take(WEAPON_WIDTH)
                .enumerate()
            {
                let bg = match code {
                    b'0'..=b'9' => code - b'0',
                    b'A'..=b'Z' => code - b'A' + 10,
                    b'a'..=b'z' => code - b'a' + 36,
                    _ => continue,
                };
                let fg = match bg {
                    0 | 20 | 21 => 22,
                    22 => 23,
                    23 | 24 => 24,
                    25 => 27,
                    26 | 27 => 28,
                    28 => 11,
                    29 => 30,
                    30 => 31,
                    31 => 14,
                    12 => 13,
                    13 => 14,
                    14 => 11,
                    32 => 34,
                    33 => 35,
                    34 => 36,
                    35 => 37,
                    36 | 37 => 38,
                    38 => 11,
                    _ => 8,
                };
                self.put(64 + col as i32, 40 + row as i32, glyph, fg, bg);
            }
        }
        if self.muzzle > 0.0 {
            self.put(70, 51, b'*', 31, 0);
            self.put(69, 52, b'+', 11, 0);
            self.put(71, 54, b'*', 31, 0);
        }
    }

    fn render_room(&mut self) {
        let horizon = ROWS as f32 * 0.5;
        for x in 0..COLS {
            let ray_x = 2.0 * (x as f32 + 0.5) / COLS as f32 - 1.0;
            let ray_y = -1.0;
            let hit = cast_ray(self.x, self.y, ray_x, ray_y);
            let top = horizon - (WALL_HEIGHT - EYE_HEIGHT) * PROJECTION / hit.distance;
            let bottom = horizon + EYE_HEIGHT * PROJECTION / hit.distance;
            for y in 0..ROWS {
                let row = y as f32 + 0.5;
                let cell = if row < top || row > bottom {
                    let ceiling = row < top;
                    let height = if ceiling {
                        WALL_HEIGHT - EYE_HEIGHT
                    } else {
                        EYE_HEIGHT
                    };
                    let distance = height * PROJECTION / (row - horizon).abs().max(0.01);
                    let wx = self.x + ray_x * distance;
                    let wy = self.y + ray_y * distance;
                    let gx = fract(wx * 0.5);
                    let gy = fract(wy * 0.5);
                    let line_x = gx < 0.035;
                    let line_y = gy < 0.035;
                    let noise = hash(
                        floor_i(wx * 4.0),
                        floor_i(wy * 4.0),
                        if ceiling { 4 } else { 12 },
                    );
                    let glyph = if line_y {
                        b'-'
                    } else if line_x {
                        b':'
                    } else {
                        [b' ', b'.', b' ', b'\'', b' ', b',', b' ', b'.'][(noise % 8) as usize]
                    };
                    let bg = if ceiling {
                        if distance > 4.0 {
                            1
                        } else {
                            2
                        }
                    } else if distance > 6.0 {
                        2
                    } else if distance > 2.5 {
                        3
                    } else {
                        4
                    };
                    let fg = if line_x || line_y { bg + 3 } else { bg + 2 };
                    Cell::new(glyph, fg, bg)
                } else {
                    let world_z = EYE_HEIGHT + (horizon - row) * hit.distance / PROJECTION;
                    let seam_h = fract(world_z * 0.5) < 0.035;
                    let seam_v = fract(hit.along * 0.5) < 0.035;
                    let noise = hash(floor_i(hit.along * 5.0), floor_i(world_z * 5.0), 9);
                    let glyph = if seam_h {
                        b'-'
                    } else if seam_v {
                        b'|'
                    } else {
                        [b'.', b' ', b':', b' ', b'\'', b' ', b',', b' '][(noise % 8) as usize]
                    };
                    let bg = if hit.side == 0 { 4 } else { 3 };
                    let fg = if seam_h || seam_v { bg + 3 } else { bg + 2 };
                    Cell::new(glyph, fg, bg)
                };
                self.cells[y * COLS + x] = cell;
            }
        }
    }

    pub fn render(&mut self) -> &[Cell; CELL_COUNT] {
        self.render_room();
        self.render_weapon();
        &self.cells
    }
}

#[cfg(target_arch = "wasm32")]
mod wasm {
    use super::*;
    use core::cell::UnsafeCell;

    // The host reads the frame only after the non-reentrant call returns.
    struct State(UnsafeCell<Game>);
    unsafe impl Sync for State {}
    static STATE: State = State(UnsafeCell::new(Game::new()));

    #[no_mangle]
    pub extern "C" fn game_frame(dt: f32, input: u32) -> *const Cell {
        let game = unsafe { &mut *STATE.0.get() };
        game.update(dt, input);
        game.render().as_ptr()
    }

    #[no_mangle]
    pub extern "C" fn game_reset() {
        unsafe { &mut *STATE.0.get() }.reset();
    }

    #[no_mangle]
    pub extern "C" fn game_speed() -> f32 {
        unsafe { &*STATE.0.get() }.speed() / MAX_SPEED
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    const DT: f32 = 1.0 / 120.0;
    fn advance(game: &mut Game, input: u32, frames: usize) {
        for _ in 0..frames {
            game.update(DT, input);
        }
    }

    #[test]
    fn strafe_directions_and_reset() {
        let mut game = Game::new();
        advance(&mut game, keys::LEFT, 30);
        assert!(game.x < SPAWN_X);
        game.reset();
        advance(&mut game, keys::RIGHT, 30);
        assert!(game.x > SPAWN_X);
        game.reset();
        assert_eq!(
            (game.x, game.y, game.vx, game.vy),
            (SPAWN_X, SPAWN_Y, 0.0, 0.0)
        );
    }

    #[test]
    fn counter_strafe_stops_faster_than_releasing() {
        let mut coast = Game::new();
        let mut counter = Game::new();
        advance(&mut coast, keys::LEFT, 30);
        advance(&mut counter, keys::LEFT, 30);
        let threshold = MAX_SPEED * 0.34;
        let mut coast_frames = 0;
        while coast.speed() > threshold && coast_frames < 120 {
            advance(&mut coast, 0, 1);
            coast_frames += 1;
        }
        let mut counter_frames = 0;
        while counter.speed() > threshold && counter_frames < 120 {
            advance(&mut counter, keys::RIGHT, 1);
            counter_frames += 1;
        }
        assert!(
            counter_frames < coast_frames,
            "counter={counter_frames}, coast={coast_frames}"
        );
    }

    #[test]
    fn diagonal_movement_is_normalized_and_opposing_keys_cancel() {
        let mut straight = Game::new();
        let mut diagonal = Game::new();
        advance(&mut straight, keys::FORWARD, 25);
        advance(&mut diagonal, keys::FORWARD | keys::RIGHT, 25);
        assert!((straight.speed() - diagonal.speed()).abs() < 0.001);
        let mut both = Game::new();
        advance(&mut both, keys::LEFT | keys::RIGHT, 30);
        assert!(both.speed() < 0.001);
    }

    #[test]
    fn cannot_walk_through_any_wall() {
        for direction in [keys::FORWARD, keys::BACK, keys::LEFT, keys::RIGHT] {
            let mut game = Game::new();
            advance(&mut game, direction, 1200);
            assert!(is_open(game.x, game.y));
            assert!(
                game.x >= 1.0 + PLAYER_RADIUS - 0.001 && game.x <= 13.0 - PLAYER_RADIUS + 0.001
            );
            assert!(
                game.y >= 1.0 + PLAYER_RADIUS - 0.001 && game.y <= 11.0 - PLAYER_RADIUS + 0.001
            );
        }
    }

    #[test]
    fn render_has_ascii_room_and_hands_without_target_or_crosshair() {
        let mut game = Game::new();
        game.render_room();
        let center = game.cells[(ROWS / 2) * COLS + COLS / 2];
        let cells = game.render();
        assert_eq!(cells.len(), CELL_COUNT);
        assert!(cells
            .iter()
            .all(|c| (32..=126).contains(&c.glyph) && c.fg < 39 && c.bg < 39));

        for y in ROWS / 2 - 1..=ROWS / 2 + 1 {
            for x in COLS / 2 - 2..=COLS / 2 + 2 {
                // The center belongs to the room: no crosshair or target overlay.
                assert!(cells[y * COLS + x].bg <= 4);
            }
        }
        assert_eq!(cells[(ROWS / 2) * COLS + COLS / 2], center);
        assert!(cells.iter().any(|c| c.bg == 26), "gloves should be visible");
        assert!(
            cells.iter().any(|c| c.bg == 29),
            "wooden rifle furniture should be visible"
        );
    }

    #[test]
    fn ascii_viewmodel_layers_have_the_expected_dimensions_and_palette() {
        let glyphs: Vec<_> = WEAPON_GLYPHS.lines().collect();
        let colors: Vec<_> = WEAPON_COLORS.lines().collect();
        assert_eq!(glyphs.len(), WEAPON_HEIGHT);
        assert_eq!(colors.len(), WEAPON_HEIGHT);
        for (glyphs, colors) in glyphs.iter().zip(colors.iter()) {
            // The final pipe preserves significant trailing spaces in the glyph layer.
            assert_eq!(glyphs.len(), WEAPON_WIDTH + 1);
            assert!(glyphs.ends_with('|'));
            assert_eq!(colors.len(), WEAPON_WIDTH);
            assert!(glyphs.bytes().all(|c| (32..=126).contains(&c)));
            assert!(colors
                .bytes()
                .all(|c| matches!(c, b'_' | b'0'..=b'9' | b'A'..=b'Z' | b'a'..=b'c')));
        }
    }

    #[test]
    fn view_is_fixed_but_room_changes_when_strafing() {
        let mut game = Game::new();
        let initial = *game.render();
        // Old look-input bits are intentionally ignored.
        advance(&mut game, (1 << 4) | (1 << 5) | (1 << 6) | (1 << 7), 30);
        assert_eq!(&initial, game.render());
        advance(&mut game, keys::LEFT, 35);
        let moved = game.render();
        assert!(initial[..COLS * 60]
            .iter()
            .zip(&moved[..COLS * 60])
            .any(|(a, b)| a != b));
    }

    #[test]
    fn walk_is_slower_and_fire_only_creates_a_transient_flash() {
        let mut run = Game::new();
        let mut walk = Game::new();
        advance(&mut run, keys::RIGHT, 30);
        advance(&mut walk, keys::RIGHT | keys::WALK, 30);
        assert!(walk.speed() < run.speed());
        let mut game = Game::new();
        game.update(DT, keys::FIRE);
        assert!(game.muzzle > 0.0);
        advance(&mut game, 0, 20);
        assert_eq!(game.muzzle, 0.0);
    }

    #[test]
    fn bad_input_and_long_frames_are_bounded() {
        let mut game = Game::new();
        game.update(f32::NAN, keys::RIGHT);
        assert_eq!(game.x, SPAWN_X);
        game.update(99.0, keys::RIGHT);
        assert!(game.x < SPAWN_X + 0.1);
    }
}

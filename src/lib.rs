//! The complete simulation and ASCII renderer. The browser only displays the
//! character cells and forwards input; it does not implement game physics.
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
    fn sinf(x: f32) -> f32;
    fn cosf(x: f32) -> f32;
    fn sqrtf(x: f32) -> f32;
}

#[inline]
fn sin(x: f32) -> f32 {
    #[cfg(target_arch = "wasm32")]
    unsafe {
        sinf(x)
    }
    #[cfg(not(target_arch = "wasm32"))]
    {
        x.sin()
    }
}

#[inline]
fn cos(x: f32) -> f32 {
    #[cfg(target_arch = "wasm32")]
    unsafe {
        cosf(x)
    }
    #[cfg(not(target_arch = "wasm32"))]
    {
        x.cos()
    }
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

pub const COLS: usize = 120;
pub const ROWS: usize = 72;
pub const CELL_COUNT: usize = COLS * ROWS;

/// Layout is consumed as four bytes per cell by web/app.js.
#[repr(C)]
#[derive(Copy, Clone, Debug)]
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
    pub const LOOK_LEFT: u32 = 1 << 4;
    pub const LOOK_RIGHT: u32 = 1 << 5;
    pub const LOOK_UP: u32 = 1 << 6;
    pub const LOOK_DOWN: u32 = 1 << 7;
    pub const WALK: u32 = 1 << 8;
    pub const FIRE: u32 = 1 << 9;
}

// One world unit is approximately 50 Source units. These values are intentionally
// an approximation, not measurements or a drop-in reproduction of CS2 physics.
const MAX_SPEED: f32 = 4.3;
const WALK_SPEED: f32 = 2.24;
const ACCEL: f32 = 5.5;
const FRICTION: f32 = 5.2;
const STOP_SPEED: f32 = 1.6;
const ACCURATE_SPEED: f32 = MAX_SPEED * 0.34;
const PLAYER_RADIUS: f32 = 0.22;
const EYE_HEIGHT: f32 = 1.55;
const WALL_HEIGHT: f32 = 3.8;
// A 90-degree horizontal FOV in the 4:3 logical frame gives a focal
// length of 48 ASCII rows (360 / 7.5 in the 720x540 frame).
const PROJECTION: f32 = 48.0;
const TARGET_X: f32 = 10.0;
const TARGET_Y: f32 = 1.035;
const TARGET_WIDTH: f32 = 2.8;
const TARGET_TOP: f32 = 2.35;
const TARGET_BOTTOM: f32 = 0.15;
const SPAWN_X: f32 = 10.0;
const SPAWN_Y: f32 = 8.3;

// # = surrounding wall, P = the two pillars framing the range.
const MAP: [&[u8; 20]; 16] = [
    b"####################",
    b"#..................#",
    b"#..................#",
    b"#..................#",
    b"#....PP......PP....#",
    b"#....PP......PP....#",
    b"#..................#",
    b"#..................#",
    b"#..................#",
    b"#..................#",
    b"#..................#",
    b"#..................#",
    b"#..PP..........PP..#",
    b"#..................#",
    b"#..................#",
    b"####################",
];

fn tile(x: i32, y: i32) -> u8 {
    if x < 0 || y < 0 || x >= 20 || y >= 16 {
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
    tile: u8,
}

/// DDA in map space. Ray directions do not need to be normalised. With camera
/// rays, the returned parameter is already the perpendicular projection depth.
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
        let wall = tile(mx, my);
        if wall != b'.' {
            let along = if side == 0 {
                py + distance * dy
            } else {
                px + distance * dx
            };
            return Hit {
                distance: distance.max(0.001),
                along,
                side,
                tile: wall,
            };
        }
    }
    Hit {
        distance: 64.0,
        along: 0.0,
        side: 0,
        tile: b'#',
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

/// A small, fixed paper silhouette in the world. Its shape is shared between
/// rendering and hit testing so the visible head is really the head hitbox.
fn silhouette(u: f32, v: f32) -> u8 {
    let hx = (u - 0.5) / 0.12;
    let hy = (v - 0.364) / 0.105;
    if hx * hx + hy * hy < 1.0 {
        return 2; // head
    }
    if (0.45..0.50).contains(&v) && (u - 0.5).abs() < 0.07 {
        return 1;
    }
    if (0.50..0.91).contains(&v) {
        let shoulder = if v < 0.57 {
            0.12 + (v - 0.50) * 1.45
        } else {
            0.22 - (v - 0.57) * 0.18
        };
        if (u - 0.5).abs() < shoulder {
            return 1;
        }
    }
    0
}

/// Results are transient feedback, not recorded statistics.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum ShotResult {
    None = 0,
    Miss = 1,
    Body = 2,
    Head = 3,
}

pub struct Game {
    pub x: f32,
    pub y: f32,
    pub yaw: f32,
    pub pitch: f32,
    pub vx: f32,
    pub vy: f32,
    cooldown: f32,
    muzzle: f32,
    recoil: f32,
    result_time: f32,
    pub last_result: ShotResult,
    rng: u32,
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
            yaw: 0.0,
            pitch: 0.0,
            vx: 0.0,
            vy: 0.0,
            cooldown: 0.0,
            muzzle: 0.0,
            recoil: 0.0,
            result_time: 0.0,
            last_result: ShotResult::None,
            rng: 0x41bd_d50a,
            cells: [Cell::new(b' ', 0, 0); CELL_COUNT],
        }
    }

    pub fn reset(&mut self) {
        self.x = SPAWN_X;
        self.y = SPAWN_Y;
        self.yaw = 0.0;
        self.pitch = 0.0;
        self.vx = 0.0;
        self.vy = 0.0;
        self.cooldown = 0.0;
        self.muzzle = 0.0;
        self.recoil = 0.0;
        self.result_time = 0.0;
        self.last_result = ShotResult::None;
        self.rng = 0x41bd_d50a;
    }

    pub fn speed(&self) -> f32 {
        sqrt(self.vx * self.vx + self.vy * self.vy)
    }

    pub fn speed_fraction(&self) -> f32 {
        (self.speed() / MAX_SPEED).clamp(0.0, 1.0)
    }

    pub fn accurate(&self) -> bool {
        self.speed() <= ACCURATE_SPEED
    }

    pub fn result(&self) -> ShotResult {
        if self.result_time > 0.0 {
            self.last_result
        } else {
            ShotResult::None
        }
    }

    fn random_signed(&mut self) -> f32 {
        let mut n = self.rng;
        n ^= n << 13;
        n ^= n >> 17;
        n ^= n << 5;
        self.rng = n;
        (n & 0xffff) as f32 / 32767.5 - 1.0
    }

    fn shoot(&mut self) {
        self.cooldown = 0.10; // AK-style cadence, with unlimited ammunition.
        self.muzzle = 0.065;
        let movement_spread = (self.speed_fraction() - 0.34).max(0.0) * 0.11;
        let spread = 0.002 + movement_spread + self.recoil * 0.055;
        let shot_yaw = self.yaw + self.random_signed() * spread;
        let shot_pitch = (self.pitch + self.random_signed() * spread).clamp(-0.7, 0.7);
        self.recoil = (self.recoil + 0.11).min(1.0);
        let dx = sin(shot_yaw);
        let dy = -cos(shot_yaw);
        let distance = if dy.abs() > 0.00001 {
            (TARGET_Y - self.y) / dy
        } else {
            -1.0
        };
        self.last_result = ShotResult::Miss;
        if distance > 0.0 && distance < cast_ray(self.x, self.y, dx, dy).distance {
            let at_x = self.x + dx * distance;
            let at_z = EYE_HEIGHT + (sin(shot_pitch) / cos(shot_pitch)) * distance;
            let u = (at_x - (TARGET_X - TARGET_WIDTH * 0.5)) / TARGET_WIDTH;
            let v = (TARGET_TOP - at_z) / (TARGET_TOP - TARGET_BOTTOM);
            if (0.0..=1.0).contains(&u) && (0.0..=1.0).contains(&v) {
                self.last_result = match silhouette(u, v) {
                    2 => ShotResult::Head,
                    1 => ShotResult::Body,
                    _ => ShotResult::Miss,
                };
            }
        }
        self.result_time = 0.42;
    }

    /// Advance by seconds. Mouse deltas are relative raw-ish browser movement;
    /// sensitivity uses CS's familiar 0.022 degrees/count as an approximation.
    pub fn update(&mut self, dt: f32, input: u32, mouse_x: f32, mouse_y: f32, sensitivity: f32) {
        let dt = if dt.is_finite() {
            dt.clamp(0.0, 0.05)
        } else {
            0.0
        };
        let sensitivity = if sensitivity.is_finite() {
            sensitivity.clamp(0.1, 8.0)
        } else {
            1.0
        };
        let mx = if mouse_x.is_finite() {
            mouse_x.clamp(-5000.0, 5000.0)
        } else {
            0.0
        };
        let my = if mouse_y.is_finite() {
            mouse_y.clamp(-5000.0, 5000.0)
        } else {
            0.0
        };
        const MOUSE_RADIANS: f32 = 0.000_383_972_45; // 0.022 degrees
        self.yaw += mx * sensitivity * MOUSE_RADIANS;
        self.pitch -= my * sensitivity * MOUSE_RADIANS;
        self.yaw += ((input & keys::LOOK_RIGHT != 0) as i32 - (input & keys::LOOK_LEFT != 0) as i32)
            as f32
            * dt
            * 1.8;
        self.pitch += ((input & keys::LOOK_UP != 0) as i32 - (input & keys::LOOK_DOWN != 0) as i32)
            as f32
            * dt
            * 1.2;
        // Keep angle bounded even in very long sessions.
        const TAU: f32 = core::f32::consts::TAU;
        if self.yaw > TAU {
            self.yaw -= TAU;
        }
        if self.yaw < -TAU {
            self.yaw += TAU;
        }
        self.pitch = self.pitch.clamp(-0.65, 0.65);

        self.cooldown = (self.cooldown - dt).max(0.0);
        self.muzzle = (self.muzzle - dt).max(0.0);
        self.recoil = (self.recoil - dt * 0.65).max(0.0);
        self.result_time = (self.result_time - dt).max(0.0);

        // Source-like ground friction followed by wish-direction acceleration.
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
            let fx = sin(self.yaw);
            let fy = -cos(self.yaw);
            let rx = cos(self.yaw);
            let ry = sin(self.yaw);
            let wx = (fx * forward + rx * strafe) / magnitude;
            let wy = (fy * forward + ry * strafe) / magnitude;
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

        if input & keys::FIRE != 0 && self.cooldown <= 0.0 {
            self.shoot();
        }
    }

    fn put(&mut self, x: i32, y: i32, glyph: u8, fg: u8, bg: u8) {
        if x >= 0 && y >= 0 && x < COLS as i32 && y < ROWS as i32 {
            self.cells[y as usize * COLS + x as usize] = Cell::new(glyph, fg, bg);
        }
    }

    fn line(&mut self, mut x0: i32, mut y0: i32, x1: i32, y1: i32, fg: u8, bg: u8) {
        let dx = (x1 - x0).abs();
        let dy = (y1 - y0).abs();
        let sx = if x0 < x1 { 1 } else { -1 };
        let sy = if y0 < y1 { 1 } else { -1 };
        let glyph = if dx > dy * 2 {
            b'-'
        } else if dy > dx * 2 {
            b'|'
        } else if sx == sy {
            b'\\'
        } else {
            b'/'
        };
        let mut error = dx - dy;
        loop {
            self.put(x0, y0, glyph, fg, bg);
            if x0 == x1 && y0 == y1 {
                break;
            }
            let twice = error * 2;
            if twice > -dy {
                error -= dy;
                x0 += sx;
            }
            if twice < dx {
                error += dx;
                y0 += sy;
            }
        }
    }

    fn polygon(&mut self, points: &[(i32, i32)], fg: u8, bg: u8, texture: &[u8], outline: u8) {
        let mut min_x = COLS as i32;
        let mut max_x = 0;
        let mut min_y = ROWS as i32;
        let mut max_y = 0;
        for &(x, y) in points {
            min_x = min_x.min(x);
            max_x = max_x.max(x);
            min_y = min_y.min(y);
            max_y = max_y.max(y);
        }
        for y in min_y.max(0)..=max_y.min(ROWS as i32 - 1) {
            for x in min_x.max(0)..=max_x.min(COLS as i32 - 1) {
                // Even/odd fill at cell centres, avoiding floating point here.
                let mut inside = false;
                let mut j = points.len() - 1;
                for i in 0..points.len() {
                    let (xi, yi) = points[i];
                    let (xj, yj) = points[j];
                    if (yi > y) != (yj > y) {
                        let crossing = xi as f32
                            + (y as f32 + 0.5 - yi as f32) * (xj - xi) as f32 / (yj - yi) as f32;
                        if (x as f32 + 0.5) < crossing {
                            inside = !inside;
                        }
                    }
                    j = i;
                }
                if inside {
                    let idx = (hash(x, y, 91) as usize) % texture.len();
                    self.put(x, y, texture[idx], fg, bg);
                }
            }
        }
        for i in 0..points.len() {
            let (x0, y0) = points[i];
            let (x1, y1) = points[(i + 1) % points.len()];
            self.line(x0, y0, x1, y1, outline, bg);
        }
    }

    fn render_weapon(&mut self) {
        // This is a low, close, right-handed viewmodel rather than a world object.
        // Every part of the AK and the gloved hands is built from ASCII cells.
        let metal = b"##%=:+#";
        let fabric = b"##::%+=#";
        let wood = b"=##+%==";

        // Rear arm behind the rifle.
        self.polygon(
            &[
                (110, 61),
                (119, 61),
                (124, 67),
                (124, 73),
                (106, 73),
                (105, 68),
            ],
            7,
            2,
            fabric,
            8,
        );
        self.polygon(
            &[(111, 64), (119, 65), (122, 72), (110, 72), (107, 69)],
            6,
            1,
            b"::###%",
            7,
        );

        // Barrel, gas tube and front iron sight.
        self.polygon(
            &[(72, 43), (74, 42), (98, 51), (97, 53), (74, 45)],
            9,
            1,
            metal,
            10,
        );
        self.line(75, 43, 97, 51, 8, 1);
        self.line(75, 42, 75, 39, 9, 1);
        self.line(74, 39, 76, 39, 10, 1);
        self.put(73, 42, b'[', 10, 1);
        self.put(74, 42, b']', 10, 1);

        // Brown fore-end and black upper receiver.
        self.polygon(
            &[(82, 46), (88, 47), (100, 52), (97, 56), (87, 52), (83, 49)],
            13,
            12,
            wood,
            14,
        );
        self.line(86, 48, 97, 53, 14, 12);
        self.polygon(
            &[
                (96, 50),
                (102, 51),
                (115, 57),
                (113, 62),
                (104, 62),
                (96, 56),
            ],
            8,
            1,
            metal,
            9,
        );
        self.line(99, 52, 111, 57, 10, 1);
        self.line(102, 54, 110, 58, 7, 1);
        self.put(105, 56, b'O', 10, 1);
        self.put(108, 57, b'=', 10, 1);

        // Distinctive curved magazine.
        self.polygon(
            &[
                (98, 57),
                (103, 58),
                (106, 62),
                (107, 66),
                (105, 70),
                (103, 72),
                (98, 72),
                (101, 67),
                (101, 63),
                (97, 60),
            ],
            8,
            1,
            b"#%#:=+",
            9,
        );
        self.line(102, 61, 104, 66, 9, 1);
        self.line(101, 70, 104, 67, 7, 1);

        // Pistol grip and rear hand, then the forward supporting glove.
        self.polygon(
            &[(109, 60), (114, 61), (114, 67), (111, 70), (107, 67)],
            8,
            1,
            fabric,
            9,
        );
        self.polygon(
            &[
                (110, 61),
                (114, 60),
                (118, 62),
                (121, 66),
                (120, 70),
                (115, 70),
                (112, 67),
                (109, 66),
            ],
            8,
            2,
            b"##%=::",
            10,
        );
        self.line(113, 63, 118, 65, 10, 2);
        self.line(112, 65, 117, 67, 9, 2);

        self.polygon(
            &[
                (92, 55),
                (96, 55),
                (100, 58),
                (101, 61),
                (103, 63),
                (101, 66),
                (96, 64),
                (92, 61),
                (90, 58),
            ],
            9,
            2,
            b"#%+::#",
            10,
        );
        self.line(94, 57, 98, 59, 11, 2);
        self.line(93, 59, 97, 61, 8, 2);
        self.polygon(
            &[
                (95, 63),
                (100, 64),
                (105, 68),
                (108, 73),
                (88, 73),
                (89, 69),
            ],
            7,
            2,
            fabric,
            8,
        );
        self.line(93, 68, 101, 70, 9, 2);

        if self.muzzle > 0.0 {
            self.put(70, 41, b'*', 14, 0);
            self.put(69, 43, b'+', 11, 0);
            self.put(71, 44, b'*', 14, 0);
            self.put(70, 45, b'.', 13, 0);
        }
    }

    pub fn render(&mut self) -> &[Cell; CELL_COUNT] {
        let fx = sin(self.yaw);
        let fy = -cos(self.yaw);
        let rx = cos(self.yaw);
        let ry = sin(self.yaw);
        let horizon = ROWS as f32 * 0.5 + sin(self.pitch) / cos(self.pitch) * PROJECTION;
        let mut depths = [0.0_f32; COLS];
        for (x, depth_slot) in depths.iter_mut().enumerate() {
            let camera_x = 2.0 * (x as f32 + 0.5) / COLS as f32 - 1.0;
            let ray_x = fx + rx * camera_x;
            let ray_y = fy + ry * camera_x;
            let hit = cast_ray(self.x, self.y, ray_x, ray_y);
            *depth_slot = hit.distance;
            let top = horizon - (WALL_HEIGHT - EYE_HEIGHT) * PROJECTION / hit.distance;
            let bottom = horizon + EYE_HEIGHT * PROJECTION / hit.distance;
            for y in 0..ROWS {
                let row = y as f32 + 0.5;
                let cell = if row < top {
                    // Sparse textured sky: it follows yaw, but does not fill the
                    // frame with distracting visual noise.
                    let sky_x = x as i32 + (self.yaw * 60.0) as i32;
                    let n = hash(sky_x / 2, y as i32 / 2, 1);
                    let glyph = if n % 29 == 0 {
                        b'.'
                    } else if n % 47 == 0 {
                        b'\''
                    } else {
                        b' '
                    };
                    let bg = if row < horizon * 0.48 { 0 } else { 1 };
                    Cell::new(glyph, 3, bg)
                } else if row <= bottom {
                    let world_z = EYE_HEIGHT + (horizon - row) * hit.distance / PROJECTION;
                    let brick_y = floor_i(world_z * 2.0);
                    let u = hit.along * 1.5 + if brick_y & 1 == 0 { 0.0 } else { 0.5 };
                    let mortar_h = fract(world_z * 2.0) < 0.13;
                    let mortar_v = fract(u) < 0.10;
                    let noise = hash(
                        floor_i(hit.along * 6.0),
                        floor_i(world_z * 6.0),
                        hit.tile as u32,
                    );
                    let glyph = if mortar_h {
                        b'-'
                    } else if mortar_v {
                        b'|'
                    } else {
                        [b'.', b':', b' ', b'.', b'\'', b' ', b',', b':'][(noise % 8) as usize]
                    };
                    let base = if hit.distance < 3.2 {
                        5
                    } else if hit.distance < 7.0 {
                        4
                    } else {
                        3
                    };
                    let bg = if hit.tile == b'P' {
                        (base + 1).min(5)
                    } else {
                        base
                    };
                    let fg = if mortar_h || mortar_v {
                        (bg + 2).min(8)
                    } else {
                        (bg + if hit.side == 0 { 2 } else { 3 }).min(9)
                    };
                    Cell::new(glyph, fg, bg)
                } else {
                    let distance = EYE_HEIGHT * PROJECTION / (row - horizon).max(0.01);
                    let wx = self.x + ray_x * distance;
                    let wy = self.y + ray_y * distance;
                    let gx = fract(wx * 0.67);
                    let gy = fract(wy * 0.67);
                    let line_x = gx < 0.045;
                    let line_y = gy < 0.045;
                    let noise = hash(floor_i(wx * 5.0), floor_i(wy * 5.0), 12);
                    let glyph = if line_y {
                        b'-'
                    } else if line_x {
                        b':'
                    } else {
                        [b'.', b' ', b'\'', b' ', b',', b'.', b' ', b':'][(noise % 8) as usize]
                    };
                    let bg = if distance > 6.0 {
                        2
                    } else if distance > 2.5 {
                        3
                    } else {
                        4
                    };
                    let fg = if line_x || line_y { bg + 3 } else { bg + 2 };
                    Cell::new(glyph, fg, bg)
                };
                self.cells[y * COLS + x] = cell;
            }
        }

        // Draw a world-space board slightly in front of the far wall. Per-column
        // depth checks make the pillars occlude it correctly as you strafe.
        for (x, wall_depth) in depths.iter().enumerate() {
            let camera_x = 2.0 * (x as f32 + 0.5) / COLS as f32 - 1.0;
            let ray_x = fx + rx * camera_x;
            let ray_y = fy + ry * camera_x;
            if ray_y.abs() < 0.00001 {
                continue;
            }
            let distance = (TARGET_Y - self.y) / ray_y;
            if distance <= 0.0 || distance >= *wall_depth {
                continue;
            }
            let world_x = self.x + ray_x * distance;
            let u = (world_x - (TARGET_X - TARGET_WIDTH * 0.5)) / TARGET_WIDTH;
            if !(0.0..=1.0).contains(&u) {
                continue;
            }
            let top = horizon - (TARGET_TOP - EYE_HEIGHT) * PROJECTION / distance;
            let bottom = horizon - (TARGET_BOTTOM - EYE_HEIGHT) * PROJECTION / distance;
            for y in 0..ROWS {
                let row = y as f32 + 0.5;
                if row < top || row > bottom {
                    continue;
                }
                let z = EYE_HEIGHT + (horizon - row) * distance / PROJECTION;
                let v = (TARGET_TOP - z) / (TARGET_TOP - TARGET_BOTTOM);
                let edge_x = u < 0.055 || u > 0.945;
                let edge_y = v < 0.055 || v > 0.945;
                let (glyph, fg, bg) = if edge_x && edge_y {
                    (b'+', 10, 2)
                } else if edge_y {
                    (b'-', 9, 2)
                } else if edge_x {
                    (b'|', 9, 2)
                } else {
                    match silhouette(u, v) {
                        2 => {
                            let center = (u - 0.5).abs() < 0.035 && (v - 0.364).abs() < 0.05;
                            (
                                if center { b'+' } else { b'@' },
                                if center { 14 } else { 11 },
                                6,
                            )
                        }
                        1 => (if (x + y) % 3 == 0 { b'%' } else { b'#' }, 10, 6),
                        _ => (if (x + y) % 4 == 0 { b'.' } else { b' ' }, 5, 1),
                    }
                };
                self.put(x as i32, y as i32, glyph, fg, bg);
            }
        }

        self.render_weapon();
        // A static, CS-like crosshair. It stays on screen regardless of pitch.
        let cx = (COLS / 2) as i32;
        let cy = (ROWS / 2) as i32;
        let fg = if self.result_time > 0.0 && self.last_result != ShotResult::Miss {
            11
        } else {
            18
        };
        self.put(cx - 2, cy, b'-', fg, 0);
        self.put(cx + 2, cy, b'-', fg, 0);
        self.put(cx, cy - 1, b'|', fg, 0);
        self.put(cx, cy + 1, b'|', fg, 0);
        &self.cells
    }
}

#[cfg(target_arch = "wasm32")]
mod wasm {
    use super::*;
    use core::cell::UnsafeCell;

    // WebAssembly is single-threaded and these exports are not re-entrant. The
    // host reads the frame only after `game_frame` returns.
    struct State(UnsafeCell<Game>);
    unsafe impl Sync for State {}
    static STATE: State = State(UnsafeCell::new(Game::new()));

    #[no_mangle]
    pub extern "C" fn game_frame(
        dt: f32,
        input: u32,
        dx: f32,
        dy: f32,
        sensitivity: f32,
    ) -> *const Cell {
        let game = unsafe { &mut *STATE.0.get() };
        game.update(dt, input, dx, dy, sensitivity);
        game.render().as_ptr()
    }

    #[no_mangle]
    pub extern "C" fn game_reset() {
        unsafe { &mut *STATE.0.get() }.reset();
    }

    #[no_mangle]
    pub extern "C" fn game_speed() -> f32 {
        unsafe { &*STATE.0.get() }.speed_fraction()
    }

    #[no_mangle]
    pub extern "C" fn game_accurate() -> u32 {
        unsafe { &*STATE.0.get() }.accurate() as u32
    }

    #[no_mangle]
    pub extern "C" fn game_result() -> u32 {
        unsafe { &*STATE.0.get() }.result() as u32
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const DT: f32 = 1.0 / 120.0;

    fn advance(game: &mut Game, input: u32, frames: usize) {
        for _ in 0..frames {
            game.update(DT, input, 0.0, 0.0, 1.0);
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
            (game.x, game.y, game.yaw, game.vx, game.vy),
            (SPAWN_X, SPAWN_Y, 0.0, 0.0, 0.0)
        );
    }

    #[test]
    fn opposite_strafe_stops_faster_than_releasing() {
        let mut coast = Game::new();
        let mut counter = Game::new();
        advance(&mut coast, keys::LEFT, 30);
        advance(&mut counter, keys::LEFT, 30);
        let mut coast_frames = 0;
        while !coast.accurate() && coast_frames < 120 {
            advance(&mut coast, 0, 1);
            coast_frames += 1;
        }
        let mut counter_frames = 0;
        while !counter.accurate() && counter_frames < 120 {
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
    fn cannot_walk_through_walls_or_pillars() {
        let mut game = Game::new();
        advance(&mut game, keys::FORWARD, 1200);
        assert!(game.y >= 1.0 + PLAYER_RADIUS - 0.001);
        assert!(is_open(game.x, game.y));
        advance(&mut game, keys::LEFT, 1200);
        assert!(is_open(game.x, game.y));
    }

    #[test]
    fn aim_and_shot_use_the_visible_head() {
        let mut game = Game::new();
        game.update(DT, keys::FIRE, 0.0, 0.0, 1.0);
        assert_eq!(game.result(), ShotResult::Head);
        advance(&mut game, 0, 100);
        assert_eq!(game.result(), ShotResult::None);
        game.reset();
        game.update(DT, keys::FIRE, 1000.0, 0.0, 1.0);
        assert_eq!(game.result(), ShotResult::Miss);
    }

    #[test]
    fn render_contains_only_ascii_and_valid_palette() {
        let mut game = Game::new();
        let cells = game.render();
        assert_eq!(cells.len(), CELL_COUNT);
        assert!(cells
            .iter()
            .all(|c| (32..=126).contains(&c.glyph) && c.fg < 20 && c.bg < 20));
        assert!(cells.iter().any(|c| c.glyph == b'@'));
    }

    #[test]
    fn target_has_world_space_parallax_when_strafing() {
        fn head_center(cells: &[Cell]) -> f32 {
            let mut sum = 0usize;
            let mut count = 0usize;
            for (i, cell) in cells.iter().enumerate() {
                if cell.glyph == b'@' && cell.bg == 6 {
                    sum += i % COLS;
                    count += 1;
                }
            }
            assert!(count > 0, "target head should be visible");
            sum as f32 / count as f32
        }
        let mut game = Game::new();
        let initial = head_center(game.render());
        advance(&mut game, keys::LEFT, 35);
        let after = head_center(game.render());
        assert!(after > initial + 3.0, "{initial} -> {after}");
    }

    #[test]
    fn pillars_block_shots_and_walk_is_slower() {
        let mut game = Game::new();
        game.x = 4.5;
        game.yaw = (TARGET_X - game.x).atan2(game.y - TARGET_Y);
        game.update(DT, keys::FIRE, 0.0, 0.0, 1.0);
        assert_eq!(game.result(), ShotResult::Miss);
        let mut run = Game::new();
        let mut walk = Game::new();
        advance(&mut run, keys::RIGHT, 30);
        advance(&mut walk, keys::RIGHT | keys::WALK, 30);
        assert!(walk.speed() < run.speed());
    }

    #[test]
    fn bad_input_and_long_frames_are_bounded() {
        let mut game = Game::new();
        game.update(f32::NAN, keys::RIGHT, f32::NAN, f32::INFINITY, f32::NAN);
        assert_eq!(game.x, SPAWN_X);
        game.update(99.0, keys::RIGHT, 0.0, 0.0, 1.0);
        assert!(game.x < SPAWN_X + 0.1);
        assert!(game.yaw.is_finite() && game.pitch.is_finite());
    }
}

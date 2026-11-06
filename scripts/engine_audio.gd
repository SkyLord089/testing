extends Node3D
## Small procedural audio rig: no external samples are required.

var engine_player: AudioStreamPlayer3D
var skid_player: AudioStreamPlayer3D
var wind_player: AudioStreamPlayer
var sample_rate = 22050

func _ready() -> void:
    engine_player = AudioStreamPlayer3D.new()
    engine_player.stream = _engine_wave()
    engine_player.unit_size = 7.0
    engine_player.max_distance = 70.0
    engine_player.volume_db = -18.0
    add_child(engine_player)
    engine_player.play()
    skid_player = AudioStreamPlayer3D.new()
    skid_player.stream = _noise_wave(true)
    skid_player.unit_size = 5.0
    skid_player.max_distance = 55.0
    skid_player.volume_db = -60.0
    add_child(skid_player)
    skid_player.play()
    wind_player = AudioStreamPlayer.new()
    wind_player.stream = _noise_wave(false)
    wind_player.volume_db = -60.0
    add_child(wind_player)
    wind_player.play()

func update_engine(rpm: float, throttle: float, speed: float, skid: float, active: bool) -> void:
    if not engine_player:
        return
    engine_player.pitch_scale = clampf(rpm / 3600.0, 0.24, 2.1)
    var target_volume = lerpf(-21.0, -8.0, throttle) if active else -28.0
    engine_player.volume_db = lerpf(engine_player.volume_db, target_volume, 0.12)
    var wind_volume = clampf(-48.0 + speed * 0.75, -48.0, -19.0) if active else -60.0
    wind_player.volume_db = lerpf(wind_player.volume_db, wind_volume, 0.08)
    wind_player.pitch_scale = 0.75 + clampf(speed / 70.0, 0.0, 0.65)
    var skid_volume = clampf(-34.0 + skid * 28.0, -34.0, -7.0) if active and speed > 3.0 and skid > 0.25 else -60.0
    skid_player.volume_db = lerpf(skid_player.volume_db, skid_volume, 0.18)
    skid_player.pitch_scale = 0.85 + clampf(speed / 55.0, 0.0, 0.55)

func _engine_wave() -> AudioStreamWAV:
    var count = sample_rate
    var bytes = PackedByteArray()
    bytes.resize(count * 2)
    for i in range(count):
        var t = float(i) / sample_rate
        var phase = TAU * 120.0 * t
        var value = sin(phase) * 0.40 + sin(phase * 2.0) * 0.23 + sin(phase * 3.0) * 0.12 + sin(phase * 4.0) * 0.07
        value += sin(phase * 0.5) * 0.12
        value *= 0.78 + sin(phase * 0.25) * 0.08
        var sample = int(clampf(value, -1.0, 1.0) * 24000.0)
        bytes.encode_s16(i * 2, sample)
    var wave = AudioStreamWAV.new()
    wave.format = AudioStreamWAV.FORMAT_16_BITS
    wave.mix_rate = sample_rate
    wave.stereo = false
    wave.data = bytes
    wave.loop_mode = AudioStreamWAV.LOOP_FORWARD
    wave.loop_begin = 0
    wave.loop_end = count
    return wave

func _noise_wave(textured: bool) -> AudioStreamWAV:
    var count = sample_rate
    var bytes = PackedByteArray()
    bytes.resize(count * 2)
    var random = RandomNumberGenerator.new()
    random.seed = 911 if textured else 212
    var filtered = 0.0
    for i in range(count):
        var white = random.randf_range(-1.0, 1.0)
        filtered = lerpf(filtered, white, 0.32 if textured else 0.12)
        var t = float(i) / sample_rate
        var value = filtered * 0.75
        if textured:
            value += sin(TAU * 740.0 * t) * 0.06 + sin(TAU * 1130.0 * t) * 0.04
        bytes.encode_s16(i * 2, int(clampf(value, -1.0, 1.0) * 21000.0))
    var wave = AudioStreamWAV.new()
    wave.format = AudioStreamWAV.FORMAT_16_BITS
    wave.mix_rate = sample_rate
    wave.stereo = false
    wave.data = bytes
    wave.loop_mode = AudioStreamWAV.LOOP_FORWARD
    wave.loop_begin = 0
    wave.loop_end = count
    return wave

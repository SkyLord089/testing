extends Node3D
## Non-colliding visual playback of the best recorded run.

var frames: Array = []
var cursor: int = 0
var playing: bool = false

func _ready() -> void:
    name = "BestRunGhost"
    var material = StandardMaterial3D.new()
    material.albedo_color = Color(0.20, 0.82, 0.78, 0.22)
    material.emission_enabled = true
    material.emission = Color(0.08, 0.42, 0.40)
    material.emission_energy_multiplier = 0.8
    material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
    material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
    material.cull_mode = BaseMaterial3D.CULL_DISABLED
    _box(Vector3(1.74, 0.48, 4.0), Vector3.ZERO, material)
    _box(Vector3(1.28, 0.48, 1.45), Vector3(0, 0.43, 0.15), material)
    for x in [-0.82, 0.82]:
        for z in [-1.30, 1.30]:
            _box(Vector3(0.20, 0.56, 0.58), Vector3(x, -0.26, z), material)
    visible = false

func play(new_frames: Array) -> void:
    frames = new_frames
    cursor = 0
    playing = frames.size() > 1
    visible = playing

func stop() -> void:
    playing = false
    visible = false
    frames.clear()

func update_at(time: float) -> void:
    if not playing or frames.size() < 2:
        visible = false
        return
    visible = true
    if time < float(frames[0]["t"]):
        return
    while cursor < frames.size() - 2 and float(frames[cursor + 1]["t"]) < time:
        cursor += 1
    if time > float(frames[frames.size() - 1]["t"]):
        visible = false
        return
    var a = frames[cursor]
    var b = frames[cursor + 1]
    var span = maxf(0.001, float(b["t"]) - float(a["t"]))
    var weight = clampf((time - float(a["t"])) / span, 0.0, 1.0)
    var ta: Transform3D = a["tr"]
    var tb: Transform3D = b["tr"]
    var position_3d = ta.origin.lerp(tb.origin, weight)
    var qa = ta.basis.get_rotation_quaternion()
    var qb = tb.basis.get_rotation_quaternion()
    global_transform = Transform3D(Basis(qa.slerp(qb, weight)), position_3d + Vector3.UP * 0.03)

func _box(size: Vector3, pos: Vector3, material: Material) -> void:
    var mesh = BoxMesh.new()
    mesh.size = size
    var instance = MeshInstance3D.new()
    instance.mesh = mesh
    instance.material_override = material
    instance.position = pos
    instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
    add_child(instance)

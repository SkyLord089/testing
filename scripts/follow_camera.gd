extends Camera3D

var target: Node3D
var mode: int = 0
var cinematic: bool = true
var orbit_time: float = 0.0
var smoothed_look = Vector3.ZERO

func _ready() -> void:
    current = true
    near = 0.075
    far = 1800.0
    fov = 58.0

func _process(delta: float) -> void:
    if not is_instance_valid(target):
        return
    if cinematic:
        orbit_time += delta
        var focus = target.global_position + Vector3.UP * 0.15
        var angle = orbit_time * 0.075 + 0.7
        var desired = focus + Vector3(sin(angle) * 7.4, 2.45, cos(angle) * 7.4)
        global_position = global_position.lerp(desired, 1.0 - exp(-delta * 2.2))
        look_at(focus + Vector3(0, 0.12, 0), Vector3.UP)
        fov = lerpf(fov, 52.0, delta * 2.0)
        return
    var speed = target.linear_velocity.length() if target is VehicleBody3D else 0.0
    if mode == 0:
        var desired = target.global_transform * Vector3(0, 2.05, 6.35)
        var anchor = target.global_position + Vector3.UP * 0.35
        var query = PhysicsRayQueryParameters3D.create(anchor, desired, 1, [target.get_rid()])
        var hit = get_world_3d().direct_space_state.intersect_ray(query)
        if not hit.is_empty():
            desired = hit.position + hit.normal * 0.28
        global_position = global_position.lerp(desired, 1.0 - exp(-delta * 8.5))
        var look_target = target.global_transform * Vector3(0, 0.28, -3.0)
        smoothed_look = smoothed_look.lerp(look_target, 1.0 - exp(-delta * 10.0))
        if global_position.distance_to(smoothed_look) > 0.1:
            look_at(smoothed_look, Vector3.UP)
        fov = lerpf(fov, 66.0 + clampf(speed / 65.0, 0.0, 1.0) * 11.0, delta * 3.5)
    else:
        var offset = Vector3(0, 0.56, -1.02) if mode == 1 else Vector3(-0.38, 0.49, -0.12)
        global_position = target.global_transform * offset
        var forward = -target.global_basis.z
        var up = target.global_basis.y
        look_at(global_position + forward * 30.0 + up * 0.15, up)
        fov = lerpf(fov, 72.0 + clampf(speed / 70.0, 0.0, 1.0) * 8.0, delta * 4.0)

func cycle_mode() -> void:
    mode = (mode + 1) % 3
    snap()

func snap() -> void:
    if not is_instance_valid(target):
        return
    if cinematic:
        global_position = target.global_position + Vector3(5.2, 2.4, 5.2)
        look_at(target.global_position + Vector3.UP * 0.2)
    elif mode == 0:
        global_position = target.global_transform * Vector3(0, 2.05, 6.35)
        smoothed_look = target.global_transform * Vector3(0, 0.28, -3.0)
        look_at(smoothed_look)

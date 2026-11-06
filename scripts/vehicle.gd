extends VehicleBody3D
## RWD coupe built on Godot's raycast vehicle physics.

var wheels: Array[VehicleWheel3D] = []
var front_wheels: Array[VehicleWheel3D] = []
var rear_wheels: Array[VehicleWheel3D] = []
var controls_enabled: bool = false
var abs_enabled: bool = true
var tcs_enabled: bool = true
var automatic: bool = true
var wetness: float = 0.0
var damage: float = 0.0
var odometer: float = 0.0
var throttle_input: float = 0.0
var brake_input: float = 0.0
var steer_input: float = 0.0
var handbrake_input: float = 0.0
var engine_rpm: float = 900.0
var gear: int = 0
var reversing: bool = false
var drift_angle: float = 0.0
var shift_cooldown: float = 0.0
var previous_speed: float = 0.0
var last_impact: float = 0.0
var headlights_on: bool = false
var headlight_nodes: Array[SpotLight3D] = []
var tail_material: StandardMaterial3D
var paint_material: StandardMaterial3D
var gear_ratios = [3.45, 2.18, 1.55, 1.18, 0.94, 0.78]
var final_drive: float = 4.1
var max_engine_force: float = 3400.0
var audio_node: Node3D

func _ready() -> void:
    name = "Kaido_SR20"
    mass = 1280.0
    gravity_scale = 1.0
    linear_damp = 0.035
    angular_damp = 0.35
    center_of_mass_mode = RigidBody3D.CENTER_OF_MASS_MODE_CUSTOM
    center_of_mass = Vector3(0.0, -0.18, -0.12)
    continuous_cd = true
    contact_monitor = true
    max_contacts_reported = 8
    _build_collision()
    _build_wheels()
    _build_visuals()
    var AudioSynth = load("res://scripts/engine_audio.gd")
    if AudioSynth:
        audio_node = AudioSynth.new()
        add_child(audio_node)

func _build_collision() -> void:
    var collider = CollisionShape3D.new()
    var shape = BoxShape3D.new()
    shape.size = Vector3(1.70, 0.55, 3.92)
    collider.shape = shape
    collider.position = Vector3(0, 0.0, 0.02)
    add_child(collider)

func _build_wheels() -> void:
    var positions = [
        Vector3(-0.83, -0.25, 1.32), Vector3(0.83, -0.25, 1.32),
        Vector3(-0.83, -0.25, -1.32), Vector3(0.83, -0.25, -1.32)
    ]
    for i in range(4):
        var wheel = VehicleWheel3D.new()
        wheel.name = ["FrontLeft", "FrontRight", "RearLeft", "RearRight"][i]
        wheel.position = positions[i]
        wheel.use_as_steering = i < 2
        wheel.use_as_traction = i >= 2
        wheel.wheel_radius = 0.32
        wheel.wheel_rest_length = 0.12
        wheel.suspension_travel = 0.17
        wheel.suspension_stiffness = 38.0 if i < 2 else 34.0
        wheel.suspension_max_force = 8500.0
        wheel.damping_compression = 4.2
        wheel.damping_relaxation = 5.4
        wheel.wheel_friction_slip = 3.25 if i < 2 else 3.05
        wheel.wheel_roll_influence = 0.10 if i < 2 else 0.14
        add_child(wheel)
        wheels.append(wheel)
        if i < 2:
            front_wheels.append(wheel)
        else:
            rear_wheels.append(wheel)
        _build_wheel_visual(wheel)

func _physics_process(delta: float) -> void:
    if not is_inside_tree():
        return
    shift_cooldown = maxf(0.0, shift_cooldown - delta)
    last_impact = maxf(0.0, last_impact - delta)
    var forward_speed = linear_velocity.dot(global_basis.z)
    var speed = linear_velocity.length()
    odometer += speed * delta
    if controls_enabled:
        _read_controls(delta, forward_speed)
    else:
        throttle_input = move_toward(throttle_input, 0.0, delta * 3.0)
        brake_input = 0.0
        steer_input = move_toward(steer_input, 0.0, delta * 4.0)
        engine_force = 0.0
        brake = 8.0
        steering = move_toward(steering, 0.0, delta * 2.0)
    _update_driveline(delta, forward_speed)
    _update_damage(speed)
    _update_audio()
    previous_speed = speed

func _read_controls(delta: float, forward_speed: float) -> void:
    var throttle_target = Input.get_action_strength("throttle")
    var brake_target = Input.get_action_strength("brake")
    throttle_input = move_toward(throttle_input, throttle_target, delta * 3.6)
    brake_input = move_toward(brake_input, brake_target, delta * 6.0)
    var steer_target = Input.get_action_strength("steer_right") - Input.get_action_strength("steer_left")
    steer_input = move_toward(steer_input, steer_target, delta * (4.8 if absf(steer_target) > 0.01 else 6.5))
    handbrake_input = Input.get_action_strength("handbrake")
    var speed_kph = absf(forward_speed) * 3.6
    var max_steer = lerpf(deg_to_rad(33.0), deg_to_rad(9.0), clampf(speed_kph / 165.0, 0.0, 1.0))
    steering = -steer_input * max_steer
    if automatic:
        reversing = brake_target > 0.2 and forward_speed < 1.2 and throttle_target < 0.1
        if throttle_target > 0.15 and forward_speed < -0.8:
            reversing = false
    else:
        reversing = false
    if reversing:
        brake = 0.0
    else:
        var foot_brake = brake_input if forward_speed > 0.8 or throttle_target < 0.1 else 0.0
        if throttle_target > 0.1 and forward_speed < -0.5:
            foot_brake = throttle_input
        var abs_factor = 1.0
        if abs_enabled and foot_brake > 0.4 and speed_kph > 12.0:
            for wheel in wheels:
                if wheel.is_in_contact() and wheel.get_skidinfo() < 0.22:
                    abs_factor = 0.48
                    break
        brake = foot_brake * 34.0 * abs_factor
    for wheel in rear_wheels:
        wheel.brake = handbrake_input * 52.0

func _update_driveline(delta: float, forward_speed: float) -> void:
    if not controls_enabled:
        engine_rpm = lerpf(engine_rpm, 900.0, delta * 2.0)
        return
    if not automatic:
        if Input.is_action_just_pressed("shift_up"):
            gear = mini(gear + 1, gear_ratios.size() - 1)
            shift_cooldown = 0.18
        if Input.is_action_just_pressed("shift_down"):
            gear = maxi(gear - 1, 0)
            shift_cooldown = 0.18
    var ratio = gear_ratios[gear]
    var wheel_rpm = absf(forward_speed) * 60.0 / (TAU * 0.32)
    var target_rpm = 900.0 + wheel_rpm * ratio * final_drive
    if absf(forward_speed) < 2.0:
        target_rpm = maxf(target_rpm, 950.0 + throttle_input * 3600.0)
    engine_rpm = lerpf(engine_rpm, clampf(target_rpm, 850.0, 7350.0), clampf(delta * 9.0, 0.0, 1.0))
    if automatic and shift_cooldown <= 0.0 and not reversing:
        if engine_rpm > 6550.0 and gear < gear_ratios.size() - 1:
            gear += 1
            shift_cooldown = 0.32
        elif engine_rpm < 2050.0 and gear > 0:
            gear -= 1
            shift_cooldown = 0.25
    var traction_factor = 1.0
    if tcs_enabled and throttle_input > 0.35:
        for wheel in rear_wheels:
            if wheel.is_in_contact() and wheel.get_skidinfo() < 0.58:
                traction_factor = 0.52
                break
    var power_factor = 1.0 - damage * 0.0038
    var rev_limiter = 0.12 if engine_rpm > 7100.0 else 1.0
    var shift_cut = 0.25 if shift_cooldown > 0.20 else 1.0
    var direction = -1.0 if reversing else 1.0
    var requested_throttle = brake_input if reversing else throttle_input
    var force = requested_throttle * max_engine_force * (ratio / gear_ratios[0]) * power_factor * traction_factor * rev_limiter * shift_cut
    # Aerodynamic load and drag keep the top speed plausible without an artificial cap.
    if absf(forward_speed) > 65.0:
        force *= clampf(1.0 - (absf(forward_speed) - 65.0) / 30.0, 0.0, 1.0)
    engine_force = force * direction
    var velocity_flat = Vector3(linear_velocity.x, 0, linear_velocity.z)
    if velocity_flat.length() > 4.0:
        var forward_flat = global_basis.z
        forward_flat.y = 0
        drift_angle = rad_to_deg(forward_flat.normalized().angle_to(velocity_flat.normalized()))
        if drift_angle > 90.0:
            drift_angle = 180.0 - drift_angle
    else:
        drift_angle = 0.0

func _update_damage(speed: float) -> void:
    var speed_loss = previous_speed - speed
    if controls_enabled and speed_loss > 3.0 and previous_speed > 7.0:
        var impact = (speed_loss - 3.0) * 2.4
        damage = clampf(damage + impact, 0.0, 100.0)
        last_impact = 0.35

func _update_audio() -> void:
    if audio_node and audio_node.has_method("update_engine"):
        var skid = 0.0
        for wheel in rear_wheels:
            if wheel.is_in_contact():
                skid = maxf(skid, 1.0 - wheel.get_skidinfo())
        audio_node.update_engine(engine_rpm, throttle_input, linear_velocity.length(), skid, controls_enabled)

func reset_to_transform(value: Transform3D) -> void:
    global_transform = value
    linear_velocity = Vector3.ZERO
    angular_velocity = Vector3.ZERO
    engine_force = 0.0
    brake = 10.0
    steering = 0.0
    throttle_input = 0.0
    brake_input = 0.0
    steer_input = 0.0
    previous_speed = 0.0
    gear = 0
    reversing = false
    engine_rpm = 900.0

func repair() -> void:
    damage = 0.0
    last_impact = 0.0

func set_wetness(value: float) -> void:
    wetness = value
    for i in range(wheels.size()):
        var base = 3.25 if i < 2 else 3.05
        wheels[i].wheel_friction_slip = base * (1.0 - value * 0.24)

func set_headlights(enabled: bool) -> void:
    headlights_on = enabled
    for light in headlight_nodes:
        light.visible = enabled

func set_brake_lights(braking: bool) -> void:
    if tail_material:
        tail_material.emission_energy_multiplier = 4.5 if braking else 1.4

func speed_kph() -> float:
    return linear_velocity.length() * 3.6

func gear_label() -> String:
    if reversing:
        return "R"
    if speed_kph() < 2.0 and throttle_input < 0.05:
        return "N"
    return str(gear + 1)

func _build_visuals() -> void:
    paint_material = _material(Color("c94b2c"), 0.58, 0.25)
    var dark = _material(Color(0.025, 0.032, 0.035), 0.35, 0.38)
    var trim = _material(Color(0.055, 0.06, 0.062), 0.5, 0.32)
    var glass = _material(Color(0.055, 0.105, 0.135), 0.65, 0.12)
    glass.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
    glass.albedo_color.a = 0.88
    glass.cull_mode = BaseMaterial3D.CULL_DISABLED
    _box(Vector3(1.76, 0.48, 4.02), Vector3(0, 0, -0.02), paint_material)
    _box(Vector3(1.68, 0.16, 1.32), Vector3(0, 0.29, 1.28), paint_material)
    _box(Vector3(1.70, 0.20, 0.66), Vector3(0, 0.27, -1.62), paint_material)
    _box(Vector3(1.80, 0.20, 0.20), Vector3(0, -0.13, 2.02), trim)
    _box(Vector3(1.80, 0.22, 0.20), Vector3(0, -0.12, -2.03), trim)
    _box(Vector3(1.06, 0.17, 0.035), Vector3(0, 0.04, 2.135), dark)
    for x in [-0.91, 0.91]:
        _box(Vector3(0.09, 0.16, 2.65), Vector3(x, -0.19, -0.16), trim)
    _build_cabin(glass)
    # Lights and small aero details.
    var head_mat = _material(Color(0.78, 0.88, 0.91), 0.2, 0.16)
    head_mat.emission_enabled = true
    head_mat.emission = Color(0.72, 0.85, 1.0)
    head_mat.emission_energy_multiplier = 2.0
    tail_material = _material(Color(0.52, 0.018, 0.012), 0.25, 0.25)
    tail_material.emission_enabled = true
    tail_material.emission = Color(0.95, 0.018, 0.008)
    tail_material.emission_energy_multiplier = 1.4
    for x in [-0.58, 0.58]:
        _box(Vector3(0.48, 0.15, 0.045), Vector3(x, 0.16, 2.055), head_mat)
        _box(Vector3(0.46, 0.15, 0.045), Vector3(x, 0.15, -2.055), tail_material)
        var spot = SpotLight3D.new()
        spot.position = Vector3(x, 0.17, 1.98)
        spot.rotation_degrees.y = 180.0
        spot.light_color = Color(0.76, 0.86, 1.0)
        spot.light_energy = 3.8
        spot.spot_range = 52.0
        spot.spot_angle = 34.0
        spot.spot_attenuation = 1.25
        spot.shadow_enabled = false
        spot.visible = false
        add_child(spot)
        headlight_nodes.append(spot)
        _box(Vector3(0.20, 0.12, 0.27), Vector3(x * 1.57, 0.30, 0.52), paint_material)
    _box(Vector3(1.46, 0.055, 0.27), Vector3(0, 0.48, -1.78), trim)
    for x in [-0.55, 0.55]:
        _box(Vector3(0.055, 0.27, 0.08), Vector3(x, 0.34, -1.74), trim)
    var exhaust = CylinderMesh.new()
    exhaust.top_radius = 0.065
    exhaust.bottom_radius = 0.065
    exhaust.height = 0.30
    exhaust.radial_segments = 12
    var exhaust_instance = MeshInstance3D.new()
    exhaust_instance.mesh = exhaust
    exhaust_instance.material_override = _material(Color(0.26, 0.28, 0.28), 0.9, 0.24)
    exhaust_instance.rotation_degrees.x = 90
    exhaust_instance.position = Vector3(-0.56, -0.23, -2.10)
    add_child(exhaust_instance)
    # A simple interior remains visible from the cockpit camera.
    _box(Vector3(1.35, 0.16, 0.48), Vector3(0, 0.24, 0.55), dark)
    for x in [-0.39, 0.39]:
        _box(Vector3(0.42, 0.54, 0.46), Vector3(x, 0.30, -0.48), dark)
    var steering_wheel = TorusMesh.new()
    steering_wheel.inner_radius = 0.13
    steering_wheel.outer_radius = 0.17
    steering_wheel.rings = 16
    steering_wheel.ring_segments = 6
    var steering_instance = MeshInstance3D.new()
    steering_instance.mesh = steering_wheel
    steering_instance.material_override = trim
    steering_instance.position = Vector3(-0.38, 0.36, 0.34)
    steering_instance.rotation_degrees.x = 108
    add_child(steering_instance)

func _build_cabin(glass: Material) -> void:
    var st = SurfaceTool.new()
    st.begin(Mesh.PRIMITIVE_TRIANGLES)
    var a = Vector3(-0.75, 0.22, 0.84)
    var b = Vector3(0.75, 0.22, 0.84)
    var c = Vector3(0.75, 0.22, -1.15)
    var d = Vector3(-0.75, 0.22, -1.15)
    var e = Vector3(-0.62, 0.64, 0.40)
    var f = Vector3(0.62, 0.64, 0.40)
    var g = Vector3(0.62, 0.64, -0.72)
    var h = Vector3(-0.62, 0.64, -0.72)
    var faces = [[e,f,b,a], [f,g,c,b], [g,h,d,c], [h,e,a,d], [h,g,f,e]]
    for face in faces:
        _visual_quad(st, face[0], face[1], face[2], face[3])
    st.generate_normals()
    var cabin = MeshInstance3D.new()
    cabin.mesh = st.commit()
    cabin.material_override = glass
    add_child(cabin)
    _box(Vector3(1.30, 0.075, 1.18), Vector3(0, 0.655, -0.16), paint_material)
    for pair in [[a,e],[b,f],[c,g],[d,h]]:
        _beam(pair[0], pair[1], 0.065, paint_material)
    _beam(e, f, 0.06, paint_material)
    _beam(g, h, 0.06, paint_material)

func _build_wheel_visual(wheel: VehicleWheel3D) -> void:
    var tire_mesh = CylinderMesh.new()
    tire_mesh.top_radius = 0.32
    tire_mesh.bottom_radius = 0.32
    tire_mesh.height = 0.235
    tire_mesh.radial_segments = 24
    tire_mesh.rings = 2
    var tire = MeshInstance3D.new()
    tire.mesh = tire_mesh
    tire.material_override = _material(Color(0.018, 0.020, 0.021), 0.0, 0.9)
    tire.rotation_degrees.z = 90
    wheel.add_child(tire)
    var rim_mesh = CylinderMesh.new()
    rim_mesh.top_radius = 0.195
    rim_mesh.bottom_radius = 0.195
    rim_mesh.height = 0.245
    rim_mesh.radial_segments = 20
    var rim = MeshInstance3D.new()
    rim.mesh = rim_mesh
    rim.material_override = _material(Color(0.34, 0.36, 0.35), 0.86, 0.24)
    rim.rotation_degrees.z = 90
    wheel.add_child(rim)
    var spoke_mat = _material(Color(0.60, 0.62, 0.59), 0.9, 0.2)
    for i in range(5):
        var spoke = BoxMesh.new()
        spoke.size = Vector3(0.252, 0.035, 0.31)
        var instance = MeshInstance3D.new()
        instance.mesh = spoke
        instance.material_override = spoke_mat
        instance.rotation.x = TAU * float(i) / 5.0
        wheel.add_child(instance)

func _material(colour: Color, metallic: float, roughness: float) -> StandardMaterial3D:
    var mat = StandardMaterial3D.new()
    mat.albedo_color = colour
    mat.metallic = metallic
    mat.roughness = roughness
    return mat

func _box(size: Vector3, pos: Vector3, mat: Material) -> MeshInstance3D:
    var mesh = BoxMesh.new()
    mesh.size = size
    var instance = MeshInstance3D.new()
    instance.mesh = mesh
    instance.material_override = mat
    instance.position = pos
    add_child(instance)
    return instance

func _beam(a: Vector3, b: Vector3, thickness: float, mat: Material) -> void:
    var mesh = BoxMesh.new()
    mesh.size = Vector3(thickness, thickness, a.distance_to(b))
    var instance = MeshInstance3D.new()
    instance.mesh = mesh
    instance.material_override = mat
    instance.position = (a + b) * 0.5
    instance.basis = Basis.looking_at((b - a).normalized(), Vector3.UP)
    add_child(instance)

func _visual_quad(st: SurfaceTool, a: Vector3, b: Vector3, c: Vector3, d: Vector3) -> void:
    for p in [a,b,c,a,c,d]:
        st.add_vertex(p)

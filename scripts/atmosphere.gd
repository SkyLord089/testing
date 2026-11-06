extends Node3D

var world_environment: WorldEnvironment
var environment: Environment
var sky_material: ProceduralSkyMaterial
var sun: DirectionalLight3D
var rain: CPUParticles3D
var follow_target: Node3D
var time_index: int = 1
var weather_index: int = 0
var base_fog: float = 0.003
var base_sun_energy: float = 1.0

func _ready() -> void:
    world_environment = WorldEnvironment.new()
    environment = Environment.new()
    var sky = Sky.new()
    sky_material = ProceduralSkyMaterial.new()
    sky.sky_material = sky_material
    environment.background_mode = Environment.BG_SKY
    environment.sky = sky
    environment.ambient_light_source = Environment.AMBIENT_SOURCE_COLOR
    environment.tonemap_mode = Environment.TONE_MAPPER_FILMIC
    environment.tonemap_exposure = 1.05
    environment.fog_enabled = true
    environment.fog_aerial_perspective = 0.35
    world_environment.environment = environment
    add_child(world_environment)
    sun = DirectionalLight3D.new()
    sun.name = "SunAndMoon"
    sun.shadow_enabled = true
    sun.directional_shadow_max_distance = 190.0
    sun.directional_shadow_mode = DirectionalLight3D.SHADOW_PARALLEL_2_SPLITS
    sun.shadow_bias = 0.045
    add_child(sun)
    _build_rain()
    set_conditions(1, 0)

func _process(_delta: float) -> void:
    if rain and is_instance_valid(follow_target):
        rain.global_position = follow_target.global_position + Vector3.UP * 8.0

func set_conditions(new_time: int, new_weather: int) -> void:
    time_index = new_time
    weather_index = new_weather
    if not environment:
        return
    match time_index:
        0:
            sun.rotation_degrees = Vector3(-52, -32, 0)
            sun.light_color = Color(1.0, 0.94, 0.82)
            base_sun_energy = 1.3
            sky_material.sky_top_color = Color(0.17, 0.36, 0.58)
            sky_material.sky_horizon_color = Color(0.67, 0.77, 0.80)
            sky_material.ground_bottom_color = Color(0.12, 0.15, 0.14)
            sky_material.ground_horizon_color = Color(0.56, 0.64, 0.63)
            environment.ambient_light_color = Color(0.62, 0.70, 0.76)
            environment.ambient_light_energy = 0.68
            environment.fog_light_color = Color(0.58, 0.66, 0.68)
            base_fog = 0.0022
        1:
            sun.rotation_degrees = Vector3(-13, -42, 0)
            sun.light_color = Color(1.0, 0.57, 0.32)
            base_sun_energy = 1.15
            sky_material.sky_top_color = Color(0.075, 0.13, 0.23)
            sky_material.sky_horizon_color = Color(0.83, 0.39, 0.22)
            sky_material.ground_bottom_color = Color(0.065, 0.075, 0.08)
            sky_material.ground_horizon_color = Color(0.37, 0.25, 0.20)
            environment.ambient_light_color = Color(0.43, 0.48, 0.59)
            environment.ambient_light_energy = 0.40
            environment.fog_light_color = Color(0.40, 0.30, 0.27)
            base_fog = 0.0033
        _:
            sun.rotation_degrees = Vector3(-38, 125, 0)
            sun.light_color = Color(0.43, 0.58, 0.86)
            base_sun_energy = 0.26
            sky_material.sky_top_color = Color(0.008, 0.015, 0.040)
            sky_material.sky_horizon_color = Color(0.055, 0.085, 0.13)
            sky_material.ground_bottom_color = Color(0.008, 0.012, 0.017)
            sky_material.ground_horizon_color = Color(0.035, 0.05, 0.065)
            environment.ambient_light_color = Color(0.25, 0.34, 0.50)
            environment.ambient_light_energy = 0.24
            environment.fog_light_color = Color(0.035, 0.055, 0.08)
            base_fog = 0.0038
    sky_material.sky_curve = 0.16
    sky_material.sun_angle_max = 18.0
    environment.fog_sun_scatter = 0.22
    environment.fog_height = -30.0
    environment.fog_height_density = 0.08
    var rain_factor = 1.0
    if weather_index == 2:
        rain_factor = 0.58
        environment.fog_density = base_fog + 0.0045
        environment.ambient_light_energy *= 0.76
        sky_material.sky_top_color = sky_material.sky_top_color.darkened(0.36)
        sky_material.sky_horizon_color = sky_material.sky_horizon_color.darkened(0.32)
    elif weather_index == 1:
        environment.fog_density = base_fog + 0.0012
    else:
        environment.fog_density = base_fog
    sun.light_energy = base_sun_energy * rain_factor
    rain.emitting = weather_index == 2

func headlights_recommended() -> bool:
    return time_index > 0 or weather_index == 2

func wetness() -> float:
    return [0.0, 0.72, 1.0][weather_index]

func _build_rain() -> void:
    rain = CPUParticles3D.new()
    rain.name = "Rain"
    rain.amount = 520
    rain.lifetime = 0.85
    rain.preprocess = 0.4
    rain.emission_shape = CPUParticles3D.EMISSION_SHAPE_BOX
    rain.emission_box_extents = Vector3(18, 6, 18)
    rain.direction = Vector3(-0.12, -1, 0.06)
    rain.spread = 3.0
    rain.gravity = Vector3(0, -18, 0)
    rain.initial_velocity_min = 18.0
    rain.initial_velocity_max = 25.0
    rain.scale_amount_min = 0.65
    rain.scale_amount_max = 1.25
    rain.local_coords = false
    var drops = CylinderMesh.new()
    drops.top_radius = 0.009
    drops.bottom_radius = 0.009
    drops.height = 0.72
    drops.radial_segments = 3
    var material = StandardMaterial3D.new()
    material.albedo_color = Color(0.60, 0.72, 0.82, 0.38)
    material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
    material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
    drops.material = material
    rain.mesh = drops
    rain.emitting = false
    add_child(rain)

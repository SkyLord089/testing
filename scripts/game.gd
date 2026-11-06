extends Node3D
## Application controller: menu -> countdown -> driving -> results.

const Road = preload("res://scripts/road.gd")
const Vehicle = preload("res://scripts/vehicle.gd")
const Atmosphere = preload("res://scripts/atmosphere.gd")
const FollowCamera = preload("res://scripts/follow_camera.gd")
const HUD = preload("res://scripts/hud.gd")
const Race = preload("res://scripts/race.gd")
const Ghost = preload("res://scripts/ghost.gd")

var road: Node3D
var car: VehicleBody3D
var atmosphere: Node3D
var chase_camera: Camera3D
var hud: CanvasLayer
var race: Node
var ghost: Node3D
var current_route: String = "akina"
var state: String = "menu"
var state_before_pause: String = "driving"
var countdown: float = 0.0
var go_display: float = 0.0
var settings: Dictionary = {}
var previous_reverse: bool = false

func _ready() -> void:
    _setup_input()
    atmosphere = Atmosphere.new()
    add_child(atmosphere)
    race = Race.new()
    add_child(race)
    race.split_passed.connect(_on_split)
    race.race_finished.connect(_on_finish)
    _load_route(current_route)
    hud = HUD.new()
    add_child(hud)
    hud.start_requested.connect(start_run)
    hud.route_selected.connect(_on_route_selected)
    hud.settings_changed.connect(_on_settings_changed)
    hud.resume_requested.connect(resume_run)
    hud.restart_requested.connect(start_run)
    hud.menu_requested.connect(return_to_menu)
    hud.quit_requested.connect(func(): get_tree().quit())
    _apply_settings(true)
    hud.show_menu()
    print("TOUGE / APEX ready — ", road.points.size(), " road samples, ", snappedf(road.road_length, 0.1), " m")

func _load_route(route_id: String) -> void:
    current_route = route_id
    if is_instance_valid(road):
        road.queue_free()
    if is_instance_valid(car):
        car.queue_free()
    if is_instance_valid(chase_camera):
        chase_camera.queue_free()
    if is_instance_valid(ghost):
        ghost.queue_free()
    road = Road.new()
    add_child(road)
    road.build(route_id)
    car = Vehicle.new()
    car.collision_layer = 2
    car.collision_mask = 1
    add_child(car)
    car.reset_to_transform(road.start_transform(false))
    car.controls_enabled = false
    ghost = Ghost.new()
    add_child(ghost)
    chase_camera = FollowCamera.new()
    add_child(chase_camera)
    chase_camera.target = car
    chase_camera.cinematic = true
    chase_camera.snap()
    atmosphere.follow_target = car

func _physics_process(delta: float) -> void:
    if not is_instance_valid(race) or not is_instance_valid(car):
        return
    if state == "countdown":
        countdown -= delta
        if countdown > 0.0:
            hud.set_countdown(str(maxi(1, ceili(countdown))))
        else:
            state = "driving"
            car.controls_enabled = true
            race.begin()
            go_display = 0.75
            hud.set_countdown("GO!")
    elif state == "driving":
        race.physics_update(delta)
        ghost.update_at(race.elapsed)
        if go_display > 0.0:
            go_display -= delta
            if go_display <= 0.0:
                hud.set_countdown("")
    elif state == "results":
        ghost.update_at(race.elapsed)

func _process(_delta: float) -> void:
    if not is_instance_valid(hud) or not is_instance_valid(car):
        return
    if state != "menu":
        hud.update_hud(car, race, chase_camera.mode)
    car.set_brake_lights(car.brake_input > 0.15 or state == "paused")

func _unhandled_input(event: InputEvent) -> void:
    if event.is_action_pressed("pause"):
        if hud.setup_overlay.visible:
            hud.setup_overlay.visible = false
            return
        if state == "paused":
            resume_run()
        elif state == "driving" or state == "countdown":
            pause_run()
        return
    if state != "driving" and state != "countdown":
        return
    if event.is_action_pressed("camera"):
        chase_camera.cycle_mode()
    elif event.is_action_pressed("recover"):
        recover_car()
    elif event.is_action_pressed("headlights"):
        car.set_headlights(not car.headlights_on)
    elif event.is_action_pressed("photo_mode") and state == "driving":
        hud.toggle_photo_mode()

func start_run() -> void:
    settings = hud.get_settings()
    _apply_settings(false)
    state = "countdown"
    car.freeze = false
    car.repair()
    car.reset_to_transform(road.start_transform(settings["reverse"]))
    car.controls_enabled = false
    race.setup(road, car, settings["reverse"], settings["mode"], _record_key())
    ghost.play(race.ghost_frames)
    chase_camera.cinematic = false
    chase_camera.mode = 0
    chase_camera.snap()
    countdown = 3.0
    go_display = 0.0
    hud.show_driving()
    hud.set_countdown("3")
    hud.toast(road.data["name"] + " / " + ("ПОДЪЁМ" if settings["reverse"] else "СПУСК"), "Прогрей шины. Первая шпилька уже близко.", 2.5)

func pause_run() -> void:
    if state != "driving" and state != "countdown":
        return
    state_before_pause = state
    state = "paused"
    car.controls_enabled = false
    car.freeze = true
    hud.show_pause()

func resume_run() -> void:
    if state != "paused":
        return
    state = state_before_pause
    car.freeze = false
    car.controls_enabled = state == "driving"
    hud.hide_pause()

func return_to_menu() -> void:
    state = "menu"
    car.freeze = false
    car.controls_enabled = false
    car.reset_to_transform(road.start_transform(hud.direction_option.selected == 1))
    car.repair()
    race.reset()
    ghost.stop()
    chase_camera.cinematic = true
    chase_camera.snap()
    hud.show_menu()
    _apply_settings(false)

func recover_car() -> void:
    if state != "driving" and state != "countdown":
        return
    car.reset_to_transform(race.recovery_transform())
    race.add_recovery_penalty()
    chase_camera.snap()
    var penalty_text = "+5.000 сек. штрафа" if race.mode == 0 else "Продолжай разведку маршрута"
    hud.toast("ВОЗВРАТ НА ТРАССУ", penalty_text, 2.0)

func _on_route_selected(id: String) -> void:
    if id == current_route:
        return
    _load_route(id)
    _apply_settings(true)

func _on_settings_changed() -> void:
    if not is_instance_valid(hud):
        return
    var reverse = hud.direction_option.selected == 1
    _apply_settings(reverse != previous_reverse)

func _apply_settings(reposition: bool) -> void:
    settings = hud.get_settings()
    previous_reverse = settings["reverse"]
    atmosphere.set_conditions(settings["time"], settings["weather"])
    road.set_wetness(atmosphere.wetness())
    car.set_wetness(atmosphere.wetness())
    car.abs_enabled = settings["abs"]
    car.tcs_enabled = settings["tcs"]
    car.automatic = settings["automatic"]
    car.final_drive = settings["final_drive"]
    car.set_headlights(atmosphere.headlights_recommended())
    if reposition and state == "menu":
        car.reset_to_transform(road.start_transform(settings["reverse"]))
    if state == "menu":
        race.setup(road, car, settings["reverse"], settings["mode"], _record_key())
        hud.update_menu_best(race.best_time)
    hud.set_route_context(road.data, settings["reverse"], settings["mode"], settings["weather"], road.points)

func _record_key() -> String:
    var direction = "up" if settings.get("reverse", false) else "down"
    var surface = ["dry", "wet", "rain"][settings.get("weather", 0)]
    return current_route + "_" + direction + "_" + surface

func _on_split(sector: int, split_time: float) -> void:
    hud.toast("СЕКТОР %d / 3" % sector, Race.format_time(split_time), 2.0)

func _on_finish(final_time: float, new_best: bool) -> void:
    state = "results"
    car.controls_enabled = false
    ghost.stop()
    hud.set_countdown("")
    hud.show_results(final_time, race.best_time, new_best, race.splits)

func _setup_input() -> void:
    _bind("throttle", KEY_W)
    _bind("throttle", KEY_UP)
    _bind("brake", KEY_S)
    _bind("brake", KEY_DOWN)
    _bind("steer_left", KEY_A)
    _bind("steer_left", KEY_LEFT)
    _bind("steer_right", KEY_D)
    _bind("steer_right", KEY_RIGHT)
    _bind("handbrake", KEY_SPACE)
    _bind("recover", KEY_R)
    _bind("camera", KEY_C)
    _bind("headlights", KEY_F)
    _bind("pause", KEY_ESCAPE)
    _bind("shift_up", KEY_E)
    _bind("shift_down", KEY_Q)
    _bind("photo_mode", KEY_F1)

func _bind(action: String, key: Key) -> void:
    if not InputMap.has_action(action):
        InputMap.add_action(action)
    var event = InputEventKey.new()
    event.physical_keycode = key
    InputMap.action_add_event(action, event)

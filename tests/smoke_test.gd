extends SceneTree
## Run: godot --headless --path . --script res://tests/smoke_test.gd --fixed-fps 90

var failures: Array[String] = []

func _initialize() -> void:
    call_deferred("_run")

func _run() -> void:
    print("SMOKE: starting watchdog")
    var watchdog = Timer.new()
    watchdog.wait_time = 90.0
    watchdog.one_shot = true
    watchdog.timeout.connect(func():
        push_error("SMOKE TEST: watchdog timeout")
        quit(1)
    )
    root.add_child(watchdog)
    watchdog.start()
    print("SMOKE: loading main scene")
    var packed = load("res://scenes/main.tscn") as PackedScene
    _check(packed != null, "Main scene loads")
    if packed == null:
        _finish()
        return
    print("SMOKE: instancing game and generating Akina")
    var game = packed.instantiate()
    root.add_child(game)
    print("SMOKE: Akina generated")
    await process_frame
    _check(is_instance_valid(game.road), "Game controller initializes")
    if not is_instance_valid(game.road):
        game.queue_free()
        watchdog.stop()
        _finish()
        return
    _check(game.road.points.size() > 400, "Akina has a continuous sampled centre line")
    _check(game.road.road_length > 2500.0, "Akina length is plausible")
    _check(game.car.wheels.size() == 4, "Vehicle has four physical wheels")
    _check(game.hud.menu.visible, "Main menu is visible")
    print("SMOKE: generating Irohazaka")
    game._on_route_selected("irohazaka")
    await process_frame
    print("SMOKE: Irohazaka generated")
    _check(game.current_route == "irohazaka", "Route switching selects Irohazaka")
    _check(game.road.points.size() > 540, "Irohazaka generates successfully")
    _check(game.road.road_length > 2800.0, "Irohazaka length is plausible")
    print("SMOKE: starting countdown")
    game.start_run()
    for i in range(295):
        await physics_frame
    _check(game.state == "driving", "Countdown enters driving state")
    _check(game.race.active, "Race timer starts")
    _check(game.car.global_position.is_finite(), "Vehicle transform remains finite")
    game.pause_run()
    var paused_time = game.race.elapsed
    for i in range(20):
        await physics_frame
    _check(is_equal_approx(game.race.elapsed, paused_time), "Pause preserves race time")
    _check(game.car.freeze, "Pause freezes vehicle physics")
    game.hud.abs_check.button_pressed = false
    _check(is_equal_approx(game.race.elapsed, paused_time), "Setup changes do not reset the current run")
    game.resume_run()
    game.recover_car()
    _check(game.race.penalty >= 5.0, "Recovery applies time penalty")
    # Jump to the final sample to exercise finish logic without writing a record.
    game.race.best_time = 0.001
    var last = game.race.route_points.size() - 1
    game.car.global_transform = Transform3D(Basis.looking_at(game.race._tangent(last), Vector3.UP), game.race.route_points[last] + Vector3.UP)
    game.race.physics_update(0.016)
    await process_frame
    _check(game.race.completed, "Finish is detected at the end of the route")
    _check(game.state == "results", "Finish opens results screen")
    game.return_to_menu()
    await process_frame
    _check(game.state == "menu" and game.hud.menu.visible, "Results can return to main menu")
    game.queue_free()
    await process_frame
    watchdog.stop()
    _finish()

func _check(condition: bool, description: String) -> void:
    if condition:
        print("PASS: ", description)
    else:
        failures.append(description)
        push_error("FAIL: " + description)

func _finish() -> void:
    if failures.is_empty():
        print("SMOKE TEST: all checks passed")
        quit(0)
    else:
        print("SMOKE TEST: ", failures.size(), " check(s) failed")
        quit(1)

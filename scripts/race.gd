extends Node

signal split_passed(sector: int, split_time: float)
signal race_finished(final_time: float, new_best: bool)

var road: Node3D
var car: VehicleBody3D
var route_points = PackedVector3Array()
var mode: int = 0
var reverse: bool = false
var active: bool = false
var completed: bool = false
var elapsed: float = 0.0
var penalty: float = 0.0
var best_time: float = 0.0
var record_key: String = ""
var nearest: int = 0
var furthest: int = 0
var checkpoint: int = 0
var splits: Array[float] = []
var wrong_way: bool = false
var offroad: bool = false
var recording: Array = []
var ghost_frames: Array = []
var record_clock: float = 0.0
var checkpoints = [0.25, 0.50, 0.75]

func setup(road_ref: Node3D, car_ref: VehicleBody3D, reverse_route: bool, race_mode: int, condition_key: String) -> void:
    road = road_ref
    car = car_ref
    reverse = reverse_route
    mode = race_mode
    route_points = road.points.duplicate()
    if reverse:
        var reversed_points = PackedVector3Array()
        for i in range(route_points.size() - 1, -1, -1):
            reversed_points.append(route_points[i])
        route_points = reversed_points
    record_key = condition_key
    best_time = 0.0
    ghost_frames.clear()
    _load_record()
    reset()

func reset() -> void:
    active = false
    completed = false
    elapsed = 0.0
    penalty = 0.0
    nearest = 0
    furthest = 0
    checkpoint = 0
    splits.clear()
    recording.clear()
    record_clock = 0.0
    wrong_way = false
    offroad = false

func begin() -> void:
    if completed:
        return
    active = true
    if recording.is_empty() and mode == 0:
        recording.append({"t": 0.0, "tr": car.global_transform})

func physics_update(delta: float) -> void:
    if not is_instance_valid(car) or route_points.is_empty():
        return
    _update_position()
    if not active:
        return
    elapsed += delta
    if mode != 0:
        return
    record_clock += delta
    if record_clock >= 0.08:
        record_clock = 0.0
        recording.append({"t": elapsed, "tr": car.global_transform})
    furthest = maxi(furthest, nearest)
    var progress = float(furthest) / float(route_points.size() - 1)
    if checkpoint < checkpoints.size() and progress >= checkpoints[checkpoint]:
        if car.global_position.distance_to(route_points[nearest]) < 14.0:
            splits.append(elapsed + penalty)
            split_passed.emit(checkpoint + 1, elapsed + penalty)
            checkpoint += 1
    if progress >= 0.985 and car.global_position.distance_to(route_points[nearest]) < 15.0:
        _finish()

func _update_position() -> void:
    var first = maxi(0, nearest - 38)
    var last = mini(route_points.size() - 1, nearest + 38)
    if car.global_position.distance_to(route_points[nearest]) > 35.0:
        first = 0
        last = route_points.size() - 1
    var best = nearest
    var best_distance = INF
    for i in range(first, last + 1):
        var delta = route_points[i] - car.global_position
        var distance = delta.x * delta.x + delta.z * delta.z + delta.y * delta.y * 0.2
        if distance < best_distance:
            best_distance = distance
            best = i
    nearest = best
    var tangent = _tangent(nearest)
    wrong_way = car.linear_velocity.length() > 4.0 and car.linear_velocity.dot(tangent) < -2.5
    offroad = car.global_position.distance_to(route_points[nearest]) > road.road_width * 0.5 + 3.0

func _tangent(index: int) -> Vector3:
    return (route_points[mini(index + 1, route_points.size() - 1)] - route_points[maxi(index - 1, 0)]).normalized()

func progress() -> float:
    return float(furthest if mode == 0 else nearest) / float(maxi(1, route_points.size() - 1))

func recovery_transform() -> Transform3D:
    var index = clampi(nearest, 1, route_points.size() - 2)
    # +Z is the vehicle's forward axis, hence the negated tangent.
    return Transform3D(Basis.looking_at(-_tangent(index), Vector3.UP), route_points[index] + Vector3.UP * 1.0)

func add_recovery_penalty() -> void:
    if mode == 0 and active:
        penalty += 5.0

func total_time() -> float:
    return elapsed + penalty

func _finish() -> void:
    active = false
    completed = true
    recording.append({"t": elapsed, "tr": car.global_transform})
    var final = total_time()
    var is_best = best_time <= 0.0 or final < best_time
    if is_best:
        best_time = final
        _save_record()
    race_finished.emit(final, is_best)

func _load_record() -> void:
    if mode != 0 or record_key.is_empty():
        return
    var config = ConfigFile.new()
    if config.load("user://touge_apex_records.cfg") == OK:
        best_time = config.get_value("records", record_key, 0.0)
    var path = "user://" + record_key + ".ghost"
    if FileAccess.file_exists(path):
        var file = FileAccess.open(path, FileAccess.READ)
        if file:
            var payload = file.get_var()
            if payload is Dictionary and payload.has("frames"):
                ghost_frames = payload["frames"]

func _save_record() -> void:
    var config = ConfigFile.new()
    config.load("user://touge_apex_records.cfg")
    config.set_value("records", record_key, best_time)
    config.save("user://touge_apex_records.cfg")
    var file = FileAccess.open("user://" + record_key + ".ghost", FileAccess.WRITE)
    if file:
        file.store_var({"time": best_time, "frames": recording})

static func format_time(seconds: float) -> String:
    if seconds <= 0.0:
        return "--:--.---"
    var minutes = floori(seconds / 60.0)
    var secs = int(seconds) % 60
    var millis = int(fmod(seconds, 1.0) * 1000.0)
    return "%02d:%02d.%03d" % [minutes, secs, millis]

static func format_delta(seconds: float) -> String:
    var sign_text = "+" if seconds >= 0.0 else "−"
    return sign_text + "%0.3f" % absf(seconds)

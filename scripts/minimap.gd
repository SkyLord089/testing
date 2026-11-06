extends Control

var route = PackedVector3Array()
var progress: float = 0.0
var nearest: int = 0
var accent = Color("f2633f")

func _ready() -> void:
    mouse_filter = Control.MOUSE_FILTER_IGNORE
    custom_minimum_size = Vector2(205, 205)

func set_route(points: PackedVector3Array, reverse: bool) -> void:
    route = points.duplicate()
    if reverse:
        var reversed_route = PackedVector3Array()
        for i in range(route.size() - 1, -1, -1):
            reversed_route.append(route[i])
        route = reversed_route
    nearest = 0
    progress = 0.0
    queue_redraw()

func update_position(new_progress: float, new_nearest: int) -> void:
    progress = new_progress
    nearest = new_nearest
    queue_redraw()

func _draw() -> void:
    var background = StyleBoxFlat.new()
    background.bg_color = Color(0.018, 0.026, 0.033, 0.80)
    background.border_color = Color(0.40, 0.48, 0.48, 0.22)
    background.set_border_width_all(1)
    background.set_corner_radius_all(10)
    draw_style_box(background, Rect2(Vector2.ZERO, size))
    if route.size() < 2:
        return
    var min_p = Vector2(route[0].x, route[0].z)
    var max_p = min_p
    for p in route:
        min_p.x = minf(min_p.x, p.x)
        min_p.y = minf(min_p.y, p.z)
        max_p.x = maxf(max_p.x, p.x)
        max_p.y = maxf(max_p.y, p.z)
    var extent = (max_p - min_p).max(Vector2.ONE)
    var available = size - Vector2(34, 42)
    var scale_factor = minf(available.x / extent.x, available.y / extent.y)
    var map_size = extent * scale_factor
    var origin = (size - map_size) * 0.5 + Vector2(0, 5)
    var poly = PackedVector2Array()
    for i in range(0, route.size(), 3):
        poly.append(_map_point(route[i], min_p, scale_factor, origin))
    poly.append(_map_point(route[route.size() - 1], min_p, scale_factor, origin))
    draw_polyline(poly, Color(0.02, 0.025, 0.026, 0.9), 7.0, true)
    draw_polyline(poly, Color(0.68, 0.75, 0.72, 0.72), 2.2, true)
    var start = _map_point(route[0], min_p, scale_factor, origin)
    var finish = _map_point(route[route.size() - 1], min_p, scale_factor, origin)
    draw_circle(start, 4.0, Color("7ac79b"))
    draw_circle(finish, 4.0, accent)
    var car = _map_point(route[clampi(nearest, 0, route.size() - 1)], min_p, scale_factor, origin)
    draw_circle(car, 7.0, Color(accent, 0.18))
    draw_circle(car, 3.5, Color("f3f5ee"))
    var font = get_theme_default_font()
    draw_string(font, Vector2(13, 20), "ROUTE / %03d%%" % int(progress * 100.0), HORIZONTAL_ALIGNMENT_LEFT, -1, 11, Color(0.72, 0.79, 0.76))

func _map_point(p: Vector3, min_p: Vector2, factor: float, origin: Vector2) -> Vector2:
    return origin + (Vector2(p.x, p.z) - min_p) * factor

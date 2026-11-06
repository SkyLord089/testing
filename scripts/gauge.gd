extends Control

var speed: float = 0.0
var rpm: float = 900.0
var gear_text: String = "N"
var damage: float = 0.0
var accent = Color("f2633f")

func _ready() -> void:
    mouse_filter = Control.MOUSE_FILTER_IGNORE
    custom_minimum_size = Vector2(250, 250)

func update_values(new_speed: float, new_rpm: float, new_gear: String, new_damage: float) -> void:
    speed = new_speed
    rpm = new_rpm
    gear_text = new_gear
    damage = new_damage
    queue_redraw()

func _draw() -> void:
    var centre = size * 0.5
    var radius = minf(size.x, size.y) * 0.44
    draw_circle(centre, radius + 9.0, Color(0.018, 0.026, 0.033, 0.86))
    draw_arc(centre, radius, deg_to_rad(135), deg_to_rad(405), 96, Color(0.32, 0.38, 0.40, 0.42), 2.0, true)
    var rpm_weight = clampf(rpm / 7500.0, 0.0, 1.0)
    var rpm_colour = Color("ef3f35") if rpm > 6800 else accent
    if rpm_weight > 0.005:
        draw_arc(centre, radius, deg_to_rad(135), deg_to_rad(135 + 270 * rpm_weight), 96, rpm_colour, 4.0, true)
    for i in range(28):
        var weight = float(i) / 27.0
        var angle = deg_to_rad(135.0 + 270.0 * weight)
        var direction = Vector2(cos(angle), sin(angle))
        var major = i % 3 == 0
        var inner = radius - (13.0 if major else 7.0)
        draw_line(centre + direction * inner, centre + direction * (radius - 3.0), Color(0.77, 0.82, 0.80, 0.65 if major else 0.25), 1.5, true)
    var font = get_theme_default_font()
    for i in range(7):
        var weight = float(i) / 6.0
        var angle = deg_to_rad(135.0 + 270.0 * weight)
        var p = centre + Vector2(cos(angle), sin(angle)) * (radius - 26.0)
        var text = str(i * 40)
        var text_size = font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, 11)
        draw_string(font, p - Vector2(text_size.x * 0.5, -4), text, HORIZONTAL_ALIGNMENT_LEFT, -1, 11, Color(0.63, 0.69, 0.69, 0.72))
    _centred(font, str(int(speed)), centre.y + 11, 54, Color("edf2ed"))
    _centred(font, "KM/H", centre.y + 34, 12, Color(0.58, 0.65, 0.65))
    draw_line(centre + Vector2(-38, 48), centre + Vector2(38, 48), Color(0.3, 0.36, 0.36, 0.5), 1.0)
    _centred(font, gear_text, centre.y + 79, 29, accent)
    _centred(font, "GEAR", centre.y + 95, 9, Color(0.52, 0.59, 0.59))
    if damage > 35.0:
        _centred(font, "DAMAGE", centre.y - 52, 10, Color("ef6251"))

func _centred(font: Font, text: String, y: float, font_size: int, colour: Color) -> void:
    var width = font.get_string_size(text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size).x
    draw_string(font, Vector2((size.x - width) * 0.5, y), text, HORIZONTAL_ALIGNMENT_LEFT, -1, font_size, colour)

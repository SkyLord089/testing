extends CanvasLayer

signal start_requested
signal route_selected(id: String)
signal settings_changed
signal resume_requested
signal restart_requested
signal menu_requested
signal quit_requested

const RouteData = preload("res://scripts/route_data.gd")
const Gauge = preload("res://scripts/gauge.gd")
const Minimap = preload("res://scripts/minimap.gd")

var root: Control
var menu: Control
var driving_hud: Control
var pause_overlay: Control
var results_overlay: Control
var setup_overlay: Control
var selected_route: String = "akina"
var route_buttons: Dictionary = {}
var time_option: OptionButton
var weather_option: OptionButton
var mode_option: OptionButton
var direction_option: OptionButton
var abs_check: CheckButton
var tcs_check: CheckButton
var auto_check: CheckButton
var final_slider: HSlider
var final_value: Label
var best_menu_label: Label
var route_menu_label: Label
var mode_hud_label: Label
var timer_label: Label
var best_label: Label
var sector_label: Label
var damage_label: Label
var damage_bar: ProgressBar
var throttle_bar: ProgressBar
var brake_bar: ProgressBar
var drift_label: Label
var camera_label: Label
var status_label: Label
var countdown_label: Label
var gauge: Control
var minimap: Control
var toast_panel: Panel
var toast_title: Label
var toast_subtitle: Label
var toast_time: float = 0.0
var result_time: Label
var result_best: Label
var result_title: Label
var result_sectors: Label
var accent = Color("f2633f")
var ink = Color("e9efea")
var muted = Color("87938f")

func _ready() -> void:
    layer = 10
    root = Control.new()
    root.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
    root.mouse_filter = Control.MOUSE_FILTER_IGNORE
    add_child(root)
    root.theme = _make_theme()
    _build_menu()
    _build_driving_hud()
    _build_pause()
    _build_results()
    _build_setup()
    show_menu()

func _process(delta: float) -> void:
    if toast_time > 0.0:
        toast_time -= delta
        toast_panel.modulate.a = clampf(toast_time * 2.0, 0.0, 1.0)
        if toast_time <= 0.0:
            toast_panel.visible = false

func _make_theme() -> Theme:
    var theme = Theme.new()
    theme.default_font_size = 17
    theme.set_color("font_color", "Label", ink)
    theme.set_color("font_color", "Button", ink)
    theme.set_color("font_hover_color", "Button", Color.WHITE)
    theme.set_color("font_pressed_color", "Button", Color.WHITE)
    theme.set_color("font_color", "OptionButton", ink)
    theme.set_color("font_color", "CheckButton", ink)
    return theme

func _build_menu() -> void:
    menu = Control.new()
    menu.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
    menu.mouse_filter = Control.MOUSE_FILTER_IGNORE
    root.add_child(menu)
    var shade = ColorRect.new()
    shade.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
    shade.mouse_filter = Control.MOUSE_FILTER_IGNORE
    var gradient = ShaderMaterial.new()
    gradient.shader = load("res://shaders/menu_gradient.gdshader")
    shade.material = gradient
    menu.add_child(shade)
    _rect(menu, Rect2(54, 47, 42, 4), accent)
    _label(menu, "TOUGE / APEX", Vector2(54, 63), Vector2(560, 60), 43, ink)
    _label(menu, "MOUNTAIN PASS SIMULATOR     •     BUILD 01", Vector2(58, 126), Vector2(610, 24), 12, muted)
    _label(menu, "ВЫБЕРИ МАРШРУТ", Vector2(57, 182), Vector2(400, 25), 12, muted)
    _route_card("akina", Rect2(54, 218, 588, 150))
    _route_card("irohazaka", Rect2(54, 384, 588, 150))
    _label(menu, "KAIDO SR-20   /   1 280 KG   /   RWD", Vector2(58, 563), Vector2(580, 24), 12, Color("b4beb9"))
    _label(menu, "Настройки автомобиля доступны перед заездом.", Vector2(58, 592), Vector2(580, 26), 14, muted)
    _button(menu, "НАСТРОЙКА АВТО  →", Rect2(54, 633, 280, 46), open_setup)
    _button(menu, "ВЫХОД", Rect2(348, 633, 140, 46), func(): quit_requested.emit())
    _label(menu, "W / S  газ и тормоз     A / D  руль     SPACE  ручник", Vector2(56, 815), Vector2(710, 24), 12, Color("9ca7a2"))
    _label(menu, "C  камера     R  вернуться на дорогу     ESC  пауза", Vector2(56, 843), Vector2(710, 24), 12, Color("9ca7a2"))
    var session = _panel(menu, Rect2(1082, 118, 448, 664), Color(0.025, 0.035, 0.042, 0.94))
    _label(session, "НОВАЯ СЕССИЯ", Vector2(30, 26), Vector2(360, 28), 13, muted)
    route_menu_label = _label(session, "AKINA / СПУСК", Vector2(30, 62), Vector2(390, 43), 26, ink)
    _rect(session, Rect2(30, 118, 388, 1), Color(0.4, 0.47, 0.45, 0.25))
    _label(session, "РЕЖИМ", Vector2(30, 143), Vector2(170, 22), 11, muted)
    mode_option = _option(session, Rect2(30, 171, 388, 43), ["TIME ATTACK / НА ВРЕМЯ", "FREE RIDE / СВОБОДНАЯ ЕЗДА"])
    _label(session, "НАПРАВЛЕНИЕ", Vector2(30, 232), Vector2(250, 22), 11, muted)
    direction_option = _option(session, Rect2(30, 260, 388, 43), ["СПУСК / DOWNHILL", "ПОДЪЁМ / UPHILL"])
    _label(session, "ВРЕМЯ СУТОК", Vector2(30, 321), Vector2(170, 22), 11, muted)
    time_option = _option(session, Rect2(30, 349, 388, 43), ["ДЕНЬ", "ЗАКАТ", "НОЧЬ"])
    time_option.selected = 1
    _label(session, "ДОРОЖНЫЕ УСЛОВИЯ", Vector2(30, 410), Vector2(260, 22), 11, muted)
    weather_option = _option(session, Rect2(30, 438, 388, 43), ["СУХО / ЯСНО", "МОКРЫЙ АСФАЛЬТ", "ДОЖДЬ"])
    best_menu_label = _label(session, "ЛУЧШЕЕ ВРЕМЯ   --:--.---", Vector2(30, 510), Vector2(390, 26), 13, muted)
    var start = _button(session, "НАЧАТЬ ЗАЕЗД     →", Rect2(30, 567, 388, 62), func(): start_requested.emit(), true)
    start.add_theme_font_size_override("font_size", 19)
    _label(menu, "01 / ДВЕ ГОРЫ. ОДИН ИДЕАЛЬНЫЙ ЗАЕЗД.", Vector2(1110, 817), Vector2(440, 25), 11, Color("a8b1ac"))
    _select_route("akina", false)

func _route_card(id: String, rect: Rect2) -> void:
    var data = RouteData.get_route(id)
    var button = _button(menu, "", rect, func(): _select_route(id))
    button.alignment = HORIZONTAL_ALIGNMENT_LEFT
    route_buttons[id] = button
    var content = Control.new()
    content.position = Vector2.ZERO
    content.size = rect.size
    content.mouse_filter = Control.MOUSE_FILTER_IGNORE
    button.add_child(content)
    _label(content, data["name"], Vector2(24, 18), Vector2(360, 38), 27, ink)
    _label(content, data["japanese"] + "  /  " + data["region"], Vector2(26, 58), Vector2(420, 22), 11, muted)
    _label(content, data["tagline"], Vector2(26, 88), Vector2(535, 25), 14, Color("bec7c1"))
    _label(content, data["estimated"] + "   •   " + data["corners"], Vector2(26, 121), Vector2(360, 20), 10, muted)
    var badge = _panel(content, Rect2(457, 20, 105, 26), Color(0.14, 0.18, 0.18, 0.8), 5)
    var difficulty = _label(badge, data["difficulty"], Vector2.ZERO, Vector2(105, 26), 9, Color("c8d1cb"))
    difficulty.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
    difficulty.vertical_alignment = VERTICAL_ALIGNMENT_CENTER

func _build_driving_hud() -> void:
    driving_hud = Control.new()
    driving_hud.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
    driving_hud.mouse_filter = Control.MOUSE_FILTER_IGNORE
    root.add_child(driving_hud)
    var top_left = _panel(driving_hud, Rect2(26, 24, 360, 114), Color(0.018, 0.026, 0.033, 0.78))
    var route_hud = _label(top_left, "AKINA / СПУСК", Vector2(18, 13), Vector2(330, 30), 19, ink)
    route_hud.name = "RouteLabel"
    mode_hud_label = _label(top_left, "TIME ATTACK  •  DRY", Vector2(18, 47), Vector2(330, 22), 11, muted)
    sector_label = _label(top_left, "SECTORS   —  /  —  /  —", Vector2(18, 79), Vector2(330, 22), 11, Color("b8c2bc"))
    var timer_panel = _panel(driving_hud, Rect2(622, 24, 356, 109), Color(0.018, 0.026, 0.033, 0.80))
    timer_label = _label(timer_panel, "00:00.000", Vector2(0, 12), Vector2(356, 48), 34, ink)
    timer_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
    best_label = _label(timer_panel, "BEST   --:--.---", Vector2(0, 70), Vector2(356, 24), 12, muted)
    best_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
    minimap = Minimap.new()
    minimap.position = Vector2(1365, 26)
    minimap.size = Vector2(205, 205)
    driving_hud.add_child(minimap)
    gauge = Gauge.new()
    gauge.position = Vector2(1320, 620)
    gauge.size = Vector2(250, 250)
    driving_hud.add_child(gauge)
    var telemetry = _panel(driving_hud, Rect2(26, 663, 310, 198), Color(0.018, 0.026, 0.033, 0.82))
    _label(telemetry, "KAIDO SR-20", Vector2(16, 13), Vector2(260, 25), 16, ink)
    _label(telemetry, "2.0 TURBO  /  RWD  /  6 MT", Vector2(16, 40), Vector2(270, 20), 10, muted)
    _label(telemetry, "THROTTLE", Vector2(16, 72), Vector2(90, 18), 9, muted)
    throttle_bar = _bar(telemetry, Rect2(105, 78, 180, 5), Color("83bd91"))
    _label(telemetry, "BRAKE", Vector2(16, 96), Vector2(90, 18), 9, muted)
    brake_bar = _bar(telemetry, Rect2(105, 102, 180, 5), Color("e35b48"))
    _label(telemetry, "BODY", Vector2(16, 125), Vector2(90, 18), 9, muted)
    damage_bar = _bar(telemetry, Rect2(105, 131, 180, 5), accent)
    damage_bar.value = 100
    damage_label = _label(telemetry, "100%  /  ГОТОВ", Vector2(16, 155), Vector2(275, 23), 12, Color("bac5be"))
    drift_label = _label(driving_hud, "", Vector2(580, 745), Vector2(440, 43), 24, ink)
    drift_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
    camera_label = _label(driving_hud, "CHASE CAM", Vector2(675, 823), Vector2(250, 22), 10, muted)
    camera_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
    status_label = _label(driving_hud, "", Vector2(510, 155), Vector2(580, 38), 21, Color("f0b65c"))
    status_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
    countdown_label = _label(driving_hud, "", Vector2(550, 305), Vector2(500, 125), 88, ink)
    countdown_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
    _label(driving_hud, "C  КАМЕРА     F  ФАРЫ     R  ВОССТАНОВИТЬ     F1  ФОТОРЕЖИМ     ESC  ПАУЗА", Vector2(390, 869), Vector2(900, 20), 10, Color("87938f"))
    toast_panel = _panel(driving_hud, Rect2(565, 213, 470, 88), Color(0.025, 0.035, 0.041, 0.94))
    toast_title = _label(toast_panel, "", Vector2(12, 11), Vector2(446, 31), 21, ink)
    toast_title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
    toast_subtitle = _label(toast_panel, "", Vector2(12, 49), Vector2(446, 23), 12, muted)
    toast_subtitle.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
    toast_panel.visible = false

func _build_pause() -> void:
    pause_overlay = _overlay()
    var panel = _panel(pause_overlay, Rect2(545, 210, 510, 474), Color(0.025, 0.035, 0.043, 0.98))
    _label(panel, "ЗАЕЗД ПРИОСТАНОВЛЕН", Vector2(36, 35), Vector2(440, 42), 26, ink)
    _label(panel, "Двигатель ждёт. Гора — нет.", Vector2(37, 87), Vector2(430, 27), 14, muted)
    _button(panel, "ПРОДОЛЖИТЬ", Rect2(36, 147, 438, 55), func(): resume_requested.emit(), true)
    _button(panel, "НАЧАТЬ ЗАНОВО", Rect2(36, 217, 438, 50), func(): restart_requested.emit())
    _button(panel, "НАСТРОЙКИ АВТО", Rect2(36, 282, 438, 50), open_setup)
    _button(panel, "ВЕРНУТЬСЯ В МЕНЮ", Rect2(36, 347, 438, 50), func(): menu_requested.emit())
    pause_overlay.visible = false

func _build_results() -> void:
    results_overlay = _overlay()
    var panel = _panel(results_overlay, Rect2(488, 159, 624, 582), Color(0.025, 0.035, 0.043, 0.98))
    result_title = _label(panel, "ФИНИШ", Vector2(38, 30), Vector2(548, 42), 29, accent)
    _label(panel, "AKINA / TIME ATTACK", Vector2(39, 80), Vector2(548, 24), 12, muted)
    _label(panel, "ТВОЁ ВРЕМЯ", Vector2(39, 135), Vector2(548, 24), 11, muted)
    result_time = _label(panel, "00:00.000", Vector2(36, 164), Vector2(552, 66), 48, ink)
    result_best = _label(panel, "ЛУЧШЕЕ ВРЕМЯ  --:--.---", Vector2(39, 246), Vector2(548, 27), 15, Color("b9c5bd"))
    result_sectors = _label(panel, "SECTORS  — / — / —", Vector2(39, 289), Vector2(548, 25), 12, muted)
    _button(panel, "ЕЩЁ ОДИН ЗАЕЗД", Rect2(38, 367, 548, 58), func(): restart_requested.emit(), true)
    _button(panel, "ВЕРНУТЬСЯ В МЕНЮ", Rect2(38, 441, 548, 52), func(): menu_requested.emit())
    _label(panel, "Призрак лучшего заезда сохранён локально.", Vector2(39, 523), Vector2(548, 22), 11, muted)
    results_overlay.visible = false

func _build_setup() -> void:
    setup_overlay = _overlay()
    var panel = _panel(setup_overlay, Rect2(480, 145, 640, 610), Color(0.025, 0.035, 0.043, 0.99))
    _label(panel, "НАСТРОЙКА АВТО", Vector2(36, 29), Vector2(560, 43), 28, ink)
    _label(panel, "KAIDO SR-20  /  ДОРОЖНАЯ НАСТРОЙКА", Vector2(38, 79), Vector2(560, 24), 11, muted)
    abs_check = _check(panel, "ABS — антиблокировочная система", Rect2(38, 133, 560, 43), true)
    tcs_check = _check(panel, "TCS — контроль пробуксовки", Rect2(38, 190, 560, 43), true)
    auto_check = _check(panel, "Автоматическая коробка передач", Rect2(38, 247, 560, 43), true)
    _label(panel, "ГЛАВНАЯ ПЕРЕДАЧА", Vector2(38, 322), Vector2(360, 22), 11, muted)
    final_value = _label(panel, "4.1 : 1", Vector2(440, 315), Vector2(155, 30), 19, accent)
    final_value.horizontal_alignment = HORIZONTAL_ALIGNMENT_RIGHT
    final_slider = HSlider.new()
    final_slider.position = Vector2(38, 361)
    final_slider.size = Vector2(560, 30)
    final_slider.min_value = 3.5
    final_slider.max_value = 4.8
    final_slider.step = 0.1
    final_slider.value = 4.1
    final_slider.value_changed.connect(func(value):
        final_value.text = "%0.1f : 1" % value
        settings_changed.emit()
    )
    panel.add_child(final_slider)
    _label(panel, "Короткая передача: резче отклик, ниже максимальная скорость.", Vector2(38, 405), Vector2(560, 40), 12, muted)
    _label(panel, "Ручной режим: Q / E — переключение передач.", Vector2(38, 453), Vector2(560, 25), 12, Color("b8c3bc"))
    _button(panel, "ГОТОВО", Rect2(38, 516, 560, 53), func(): setup_overlay.visible = false, true)
    setup_overlay.visible = false

func show_menu() -> void:
    menu.visible = true
    driving_hud.visible = false
    pause_overlay.visible = false
    results_overlay.visible = false
    setup_overlay.visible = false

func show_driving() -> void:
    menu.visible = false
    driving_hud.visible = true
    pause_overlay.visible = false
    results_overlay.visible = false
    setup_overlay.visible = false

func show_pause() -> void:
    pause_overlay.visible = true
    setup_overlay.visible = false

func hide_pause() -> void:
    pause_overlay.visible = false
    setup_overlay.visible = false

func show_results(final_time: float, best_time: float, new_best: bool, splits: Array[float]) -> void:
    result_title.text = "НОВЫЙ РЕКОРД" if new_best else "ФИНИШ"
    result_title.modulate = Color("efb257") if new_best else ink
    result_time.text = RaceFormatter.format(final_time)
    result_best.text = "ЛУЧШЕЕ ВРЕМЯ   " + RaceFormatter.format(best_time)
    result_sectors.text = "SECTORS   " + _format_splits(splits)
    results_overlay.visible = true

func update_hud(car: VehicleBody3D, race: Node, camera_mode: int) -> void:
    if not driving_hud.visible:
        return
    gauge.update_values(car.speed_kph(), car.engine_rpm, car.gear_label(), car.damage)
    timer_label.text = RaceFormatter.format(maxf(0.001, race.total_time()))
    best_label.text = "BEST   " + RaceFormatter.format(race.best_time)
    sector_label.text = "SECTORS   " + _format_splits(race.splits)
    damage_bar.value = 100.0 - car.damage
    damage_label.text = "%02d%%  /  %s" % [int(100.0 - car.damage), "ПОВРЕЖДЁН" if car.damage > 35.0 else "ГОТОВ"]
    throttle_bar.value = car.throttle_input * 100.0
    brake_bar.value = maxf(car.brake_input, car.handbrake_input) * 100.0
    drift_label.text = "DRIFT  %02d°" % int(car.drift_angle) if car.drift_angle > 9.0 and car.speed_kph() > 25.0 else ""
    camera_label.text = ["CHASE CAM", "HOOD CAM", "COCKPIT"][camera_mode]
    minimap.update_position(race.progress(), race.nearest)
    if race.wrong_way:
        status_label.text = "РАЗВЕРНИСЬ — ТЫ ЕДЕШЬ НЕ ТУДА"
    elif race.offroad:
        status_label.text = "ВНЕ ДОРОГИ  •  R — ВОССТАНОВИТЬ"
    elif car.damage > 75.0:
        status_label.text = "КРИТИЧЕСКИЕ ПОВРЕЖДЕНИЯ"
    else:
        status_label.text = ""

func set_route_context(data: Dictionary, reverse: bool, mode: int, weather: int, points: PackedVector3Array) -> void:
    var direction = "ПОДЪЁМ" if reverse else "СПУСК"
    route_menu_label.text = data["name"] + " / " + direction
    var route_hud = driving_hud.find_child("RouteLabel", true, false)
    if route_hud:
        route_hud.text = data["name"] + " / " + direction
    mode_hud_label.text = ("TIME ATTACK" if mode == 0 else "FREE RIDE") + "  •  " + ["DRY", "WET", "RAIN"][weather]
    minimap.set_route(points, reverse)

func update_menu_best(best_time: float) -> void:
    best_menu_label.text = "ЛУЧШЕЕ ВРЕМЯ   " + RaceFormatter.format(best_time)

func set_countdown(text: String) -> void:
    countdown_label.text = text

func toast(title: String, subtitle: String = "", duration: float = 2.6) -> void:
    toast_title.text = title
    toast_subtitle.text = subtitle
    toast_time = duration
    toast_panel.modulate.a = 1.0
    toast_panel.visible = true

func open_setup() -> void:
    setup_overlay.visible = true

func toggle_photo_mode() -> void:
    driving_hud.visible = not driving_hud.visible

func get_settings() -> Dictionary:
    return {
        "time": time_option.selected,
        "weather": weather_option.selected,
        "mode": mode_option.selected,
        "reverse": direction_option.selected == 1,
        "abs": abs_check.button_pressed,
        "tcs": tcs_check.button_pressed,
        "automatic": auto_check.button_pressed,
        "final_drive": final_slider.value
    }

func _select_route(id: String, emit_signal: bool = true) -> void:
    selected_route = id
    for route_id in route_buttons:
        var selected = route_id == id
        var normal = _style(Color(0.045, 0.060, 0.067, 0.94) if selected else Color(0.025, 0.035, 0.042, 0.82), accent if selected else Color(0.35, 0.41, 0.40, 0.24), 8, 1)
        route_buttons[route_id].add_theme_stylebox_override("normal", normal)
    var data = RouteData.get_route(id)
    route_menu_label.text = data["name"] + " / " + ("ПОДЪЁМ" if direction_option.selected == 1 else "СПУСК")
    if emit_signal:
        route_selected.emit(id)

func _format_splits(splits: Array[float]) -> String:
    var values: Array[String] = []
    for i in range(3):
        if i < splits.size():
            values.append("%0.2f" % splits[i])
        else:
            values.append("—")
    return "  /  ".join(values)

func _overlay() -> Control:
    var overlay = Control.new()
    overlay.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
    root.add_child(overlay)
    var shade = ColorRect.new()
    shade.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
    shade.color = Color(0.008, 0.012, 0.017, 0.68)
    overlay.add_child(shade)
    return overlay

func _panel(parent: Control, rect: Rect2, colour: Color, radius: int = 9) -> Panel:
    var panel = Panel.new()
    panel.position = rect.position
    panel.size = rect.size
    panel.mouse_filter = Control.MOUSE_FILTER_IGNORE
    panel.add_theme_stylebox_override("panel", _style(colour, Color(0.4, 0.48, 0.45, 0.20), radius, 1))
    parent.add_child(panel)
    return panel

func _rect(parent: Control, rect: Rect2, colour: Color) -> void:
    var control = ColorRect.new()
    control.position = rect.position
    control.size = rect.size
    control.color = colour
    control.mouse_filter = Control.MOUSE_FILTER_IGNORE
    parent.add_child(control)

func _label(parent: Control, text: String, pos: Vector2, label_size: Vector2, font_size: int, colour: Color) -> Label:
    var label = Label.new()
    label.text = text
    label.position = pos
    label.size = label_size
    label.add_theme_font_size_override("font_size", font_size)
    label.add_theme_color_override("font_color", colour)
    label.mouse_filter = Control.MOUSE_FILTER_IGNORE
    parent.add_child(label)
    return label

func _button(parent: Control, text: String, rect: Rect2, callback: Callable, primary: bool = false) -> Button:
    var button = Button.new()
    button.text = text
    button.position = rect.position
    button.size = rect.size
    button.mouse_default_cursor_shape = Control.CURSOR_POINTING_HAND
    button.add_theme_stylebox_override("normal", _style(accent if primary else Color(0.055, 0.072, 0.078, 0.94), accent if primary else Color(0.38, 0.46, 0.43, 0.28), 7, 1))
    button.add_theme_stylebox_override("hover", _style(accent.lightened(0.12) if primary else Color(0.09, 0.115, 0.12, 0.98), accent, 7, 1))
    button.add_theme_stylebox_override("pressed", _style(accent.darkened(0.15) if primary else Color(0.035, 0.047, 0.052, 1.0), accent, 7, 1))
    button.add_theme_stylebox_override("focus", _style(Color(0,0,0,0), Color(accent, 0.6), 7, 2))
    button.pressed.connect(callback)
    parent.add_child(button)
    return button

func _option(parent: Control, rect: Rect2, items: Array) -> OptionButton:
    var option = OptionButton.new()
    option.position = rect.position
    option.size = rect.size
    for item in items:
        option.add_item(item)
    option.add_theme_stylebox_override("normal", _style(Color(0.06, 0.077, 0.082, 1), Color(0.35, 0.43, 0.40, 0.3), 6, 1))
    option.add_theme_stylebox_override("hover", _style(Color(0.09, 0.11, 0.115, 1), accent, 6, 1))
    option.add_theme_stylebox_override("pressed", _style(Color(0.09, 0.11, 0.115, 1), accent, 6, 1))
    option.item_selected.connect(func(_index): settings_changed.emit())
    parent.add_child(option)
    return option

func _check(parent: Control, text: String, rect: Rect2, value: bool) -> CheckButton:
    var check = CheckButton.new()
    check.text = text
    check.position = rect.position
    check.size = rect.size
    check.button_pressed = value
    check.toggled.connect(func(_value): settings_changed.emit())
    parent.add_child(check)
    return check

func _bar(parent: Control, rect: Rect2, colour: Color) -> ProgressBar:
    var bar = ProgressBar.new()
    bar.position = rect.position
    bar.size = rect.size
    bar.show_percentage = false
    bar.mouse_filter = Control.MOUSE_FILTER_IGNORE
    bar.add_theme_stylebox_override("background", _style(Color(0.18, 0.22, 0.22, 0.65), Color.TRANSPARENT, 2, 0))
    bar.add_theme_stylebox_override("fill", _style(colour, Color.TRANSPARENT, 2, 0))
    parent.add_child(bar)
    return bar

func _style(background: Color, border: Color, radius: int, border_width: int) -> StyleBoxFlat:
    var style = StyleBoxFlat.new()
    style.bg_color = background
    style.border_color = border
    style.set_border_width_all(border_width)
    style.set_corner_radius_all(radius)
    style.content_margin_left = 16
    style.content_margin_right = 16
    return style

# Avoid a global class cache dependency just for time formatting.
class RaceFormatter:
    static func format(seconds: float) -> String:
        if seconds <= 0.0:
            return "--:--.---"
        var minutes = floori(seconds / 60.0)
        var secs = int(seconds) % 60
        var millis = int(fmod(seconds, 1.0) * 1000.0)
        return "%02d:%02d.%03d" % [minutes, secs, millis]

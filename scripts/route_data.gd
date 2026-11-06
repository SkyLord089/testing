extends RefCounted
## Hand-designed centre lines. These are artistic reconstructions, not survey data.

static func get_route(id: String) -> Dictionary:
    if id == "irohazaka":
        return _irohazaka()
    return _akina()

static func _base() -> Dictionary:
    return {
        "id": "akina",
        "name": "AKINA",
        "japanese": "秋名山",
        "region": "GUNMA / JAPAN",
        "tagline": "Быстрые связки и поздние торможения",
        "description": "Широкий горный спуск, слепые вершины и длинная серия технических шпилек.",
        "difficulty": "ТЕХНИЧНАЯ",
        "road_width": 7.2,
        "seed": 8613,
        "foliage": "cedar",
        "accent": Color("e85b38"),
        "estimated": "2.6 KM",
        "corners": "34 TURNS"
    }

static func _akina() -> Dictionary:
    var r = _base()
    r["control_points"] = [
        Vector3(0, 280, 0), Vector3(0, 276, -70), Vector3(35, 270, -145),
        Vector3(110, 264, -190), Vector3(180, 255, -180), Vector3(220, 247, -125),
        Vector3(205, 238, -60), Vector3(150, 230, -20), Vector3(75, 222, -25),
        Vector3(25, 213, -70), Vector3(5, 204, -135), Vector3(45, 195, -200),
        Vector3(120, 186, -240), Vector3(200, 177, -250), Vector3(260, 168, -220),
        Vector3(280, 158, -160), Vector3(250, 149, -100), Vector3(180, 140, -70),
        Vector3(100, 131, -85), Vector3(55, 122, -130), Vector3(70, 113, -195),
        Vector3(130, 104, -250), Vector3(215, 95, -285), Vector3(295, 86, -280),
        Vector3(345, 77, -230), Vector3(340, 68, -165), Vector3(285, 59, -115),
        Vector3(210, 50, -100), Vector3(140, 41, -130), Vector3(110, 32, -190),
        Vector3(140, 23, -260), Vector3(210, 15, -320), Vector3(300, 8, -350),
        Vector3(390, 2, -340), Vector3(460, 0, -300)
    ]
    return r

static func _irohazaka() -> Dictionary:
    var r = _base()
    r["id"] = "irohazaka"
    r["name"] = "IROHAZAKA"
    r["japanese"] = "いろは坂"
    r["region"] = "NIKKO / JAPAN"
    r["tagline"] = "Шпилька за шпилькой. Ритм важнее скорости"
    r["description"] = "Крутой лесной серпантин с тесными разворотами, клейким асфальтом и резкими сменами ритма."
    r["difficulty"] = "ЭКСПЕРТ"
    r["road_width"] = 6.6
    r["seed"] = 4819
    r["foliage"] = "autumn"
    r["accent"] = Color("e7a342")
    r["estimated"] = "2.9 KM"
    r["corners"] = "20 HAIRPINS"
    r["control_points"] = [
        Vector3(-250,310,100), Vector3(-180,305,60), Vector3(-80,300,50),
        Vector3(20,294,80), Vector3(100,287,120), Vector3(160,280,105),
        Vector3(185,273,55), Vector3(160,266,10), Vector3(100,259,-5),
        Vector3(45,252,15), Vector3(20,245,55), Vector3(35,238,100),
        Vector3(90,231,130), Vector3(160,224,150), Vector3(225,217,135),
        Vector3(260,210,90), Vector3(250,203,45), Vector3(200,196,20),
        Vector3(140,189,30), Vector3(105,182,70), Vector3(115,175,115),
        Vector3(165,168,145), Vector3(235,161,165), Vector3(305,154,150),
        Vector3(340,147,105), Vector3(330,140,55), Vector3(280,133,25),
        Vector3(220,126,30), Vector3(185,119,70), Vector3(195,112,115),
        Vector3(245,105,145), Vector3(315,98,165), Vector3(385,91,150),
        Vector3(420,84,105), Vector3(410,77,55), Vector3(360,70,25),
        Vector3(300,63,30), Vector3(265,56,70), Vector3(275,49,115),
        Vector3(325,42,145), Vector3(395,35,165), Vector3(465,28,150),
        Vector3(500,21,105), Vector3(490,14,55), Vector3(440,7,25),
        Vector3(380,2,30), Vector3(320,0,55)
    ]
    return r

static func catmull_rom(points: Array, subdivisions: int = 12) -> PackedVector3Array:
    var result = PackedVector3Array()
    if points.size() < 2:
        return result
    for i in range(points.size() - 1):
        var p0 = points[maxi(0, i - 1)]
        var p1 = points[i]
        var p2 = points[i + 1]
        var p3 = points[mini(points.size() - 1, i + 2)]
        for j in range(subdivisions):
            var t = float(j) / float(subdivisions)
            var t2 = t * t
            var t3 = t2 * t
            var p = 0.5 * ((2.0 * p1) + (-p0 + p2) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 + (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3)
            result.append(p)
    result.append(points[points.size() - 1])
    return result

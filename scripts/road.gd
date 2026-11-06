extends Node3D
## Procedural road, carved terrain, collisions, guardrails and vegetation.

const RouteData = preload("res://scripts/route_data.gd")

var data: Dictionary
var points = PackedVector3Array()
var cumulative = PackedFloat32Array()
var road_length: float = 0.0
var road_width: float = 7.0
var road_material: ShaderMaterial
var terrain_material: ShaderMaterial
var noise = FastNoiseLite.new()
var rng = RandomNumberGenerator.new()

func build(route_id: String) -> void:
    data = RouteData.get_route(route_id)
    name = data["name"] + "_Road"
    road_width = data["road_width"]
    rng.seed = data["seed"]
    noise.seed = data["seed"]
    noise.frequency = 1.0
    noise.fractal_octaves = 4
    points = RouteData.catmull_rom(data["control_points"], 12)
    _measure()
    _build_road()
    _build_markings()
    _build_terrain()
    _build_guardrails()
    _build_vegetation()
    _build_details()

func _measure() -> void:
    cumulative.append(0.0)
    road_length = 0.0
    for i in range(1, points.size()):
        road_length += points[i - 1].distance_to(points[i])
        cumulative.append(road_length)

func tangent_at(index: int) -> Vector3:
    var a = maxi(0, index - 1)
    var b = mini(points.size() - 1, index + 1)
    return (points[b] - points[a]).normalized()

func side_at(index: int) -> Vector3:
    return tangent_at(index).cross(Vector3.UP).normalized()

func start_transform(reverse: bool = false) -> Transform3D:
    var index = points.size() - 3 if reverse else 2
    var direction = tangent_at(index) * (-1.0 if reverse else 1.0)
    var origin = points[index] + Vector3.UP * 1.15
    return Transform3D(Basis.looking_at(direction, Vector3.UP), origin)

func nearest_index(position_3d: Vector3, around: int = -1, radius: int = 30) -> int:
    var first = 0
    var last = points.size() - 1
    if around >= 0:
        first = maxi(0, around - radius)
        last = mini(points.size() - 1, around + radius)
    var best = first
    var best_distance = INF
    for i in range(first, last + 1):
        var delta = points[i] - position_3d
        var d = delta.x * delta.x + delta.z * delta.z + delta.y * delta.y * 0.15
        if d < best_distance:
            best_distance = d
            best = i
    return best

func set_wetness(value: float) -> void:
    if road_material:
        road_material.set_shader_parameter("wetness", value)
    if terrain_material:
        terrain_material.set_shader_parameter("wetness", value)

func _road_vertex(index: int, offset: float, lift: float = 0.0) -> Vector3:
    var p = points[index] + side_at(index) * offset
    p.y += 0.045 * (1.0 - clampf(absf(offset) / (road_width * 0.5), 0.0, 1.0)) + lift
    return p

func _add_quad(st: SurfaceTool, a: Vector3, b: Vector3, c: Vector3, d: Vector3, normal: Vector3, uv_scale: Vector2 = Vector2.ONE, color: Color = Color.WHITE) -> void:
    var vertices = [a, b, c, a, c, d]
    var uvs = [Vector2.ZERO, Vector2(0, uv_scale.y), uv_scale, Vector2(uv_scale.x, 0), Vector2.ZERO, uv_scale]
    # UVs follow the actual quad corners rather than triangle-local coordinates.
    uvs = [Vector2(0, 0), Vector2(0, uv_scale.y), Vector2(uv_scale.x, uv_scale.y), Vector2(0, 0), Vector2(uv_scale.x, uv_scale.y), Vector2(uv_scale.x, 0)]
    for k in range(6):
        st.set_normal(normal)
        st.set_uv(uvs[k])
        st.set_color(color)
        st.add_vertex(vertices[k])

func _build_road() -> void:
    var st = SurfaceTool.new()
    st.begin(Mesh.PRIMITIVE_TRIANGLES)
    var offsets = [-road_width * 0.5, -road_width * 0.25, 0.0, road_width * 0.25, road_width * 0.5]
    for i in range(points.size() - 1):
        var normal = side_at(i).cross(tangent_at(i)).normalized()
        for j in range(offsets.size() - 1):
            var a = _road_vertex(i, offsets[j])
            var b = _road_vertex(i + 1, offsets[j])
            var c = _road_vertex(i + 1, offsets[j + 1])
            var d = _road_vertex(i, offsets[j + 1])
            _add_quad(st, a, b, c, d, normal, Vector2(0.25, points[i].distance_to(points[i + 1]) / 5.0))
    road_material = ShaderMaterial.new()
    road_material.shader = load("res://shaders/road.gdshader")
    var mesh = st.commit()
    var instance = MeshInstance3D.new()
    instance.name = "Asphalt"
    instance.mesh = mesh
    instance.material_override = road_material
    add_child(instance)
    _add_mesh_collision(mesh, instance, true)

func _build_markings() -> void:
    var edge_material = StandardMaterial3D.new()
    edge_material.albedo_color = Color(0.72, 0.73, 0.66)
    edge_material.roughness = 0.84
    edge_material.cull_mode = BaseMaterial3D.CULL_DISABLED
    var centre_material = StandardMaterial3D.new()
    centre_material.albedo_color = Color(0.78, 0.61, 0.25)
    centre_material.roughness = 0.8
    centre_material.cull_mode = BaseMaterial3D.CULL_DISABLED
    _ribbon(-road_width * 0.5 + 0.20, 0.11, edge_material, false)
    _ribbon(road_width * 0.5 - 0.20, 0.11, edge_material, false)
    _ribbon(0.0, 0.10, centre_material, true)

func _ribbon(offset: float, strip_width: float, material: Material, dashed: bool) -> void:
    var st = SurfaceTool.new()
    st.begin(Mesh.PRIMITIVE_TRIANGLES)
    for i in range(points.size() - 1):
        if dashed and int(cumulative[i] / 6.0) % 2 == 1:
            continue
        var normal = side_at(i).cross(tangent_at(i)).normalized()
        var a = _road_vertex(i, offset - strip_width * 0.5, 0.018)
        var b = _road_vertex(i + 1, offset - strip_width * 0.5, 0.018)
        var c = _road_vertex(i + 1, offset + strip_width * 0.5, 0.018)
        var d = _road_vertex(i, offset + strip_width * 0.5, 0.018)
        _add_quad(st, a, b, c, d, normal)
    var instance = MeshInstance3D.new()
    instance.mesh = st.commit()
    instance.material_override = material
    add_child(instance)

func _terrain_height_at(x: float, z: float) -> float:
    var closest = 0
    var best = INF
    for i in range(0, points.size(), 3):
        var dx = x - points[i].x
        var dz = z - points[i].z
        var d = dx * dx + dz * dz
        if d < best:
            best = d
            closest = i
    var distance = sqrt(best)
    var p = points[closest]
    var signed_side = Vector3(x - p.x, 0, z - p.z).dot(side_at(closest))
    var n = noise.get_noise_2d(x * 0.027, z * 0.027) * 4.5 + noise.get_noise_2d(x * 0.009, z * 0.009) * 8.0
    if distance < road_width * 0.5 + 1.8:
        return p.y - 0.34
    var away = distance - road_width * 0.5 - 1.8
    var rough = clampf(away / 18.0, 0.0, 1.0)
    if signed_side < 0.0:
        return p.y + away * 0.46 + n * rough
    return p.y - away * 0.53 + n * rough

func _build_terrain() -> void:
    var min_p = points[0]
    var max_p = points[0]
    for p in points:
        min_p.x = minf(min_p.x, p.x)
        min_p.y = minf(min_p.y, p.y)
        min_p.z = minf(min_p.z, p.z)
        max_p.x = maxf(max_p.x, p.x)
        max_p.y = maxf(max_p.y, p.y)
        max_p.z = maxf(max_p.z, p.z)
    var margin = 150.0
    var step = 12.0
    var nx = int((max_p.x - min_p.x + margin * 2.0) / step) + 1
    var nz = int((max_p.z - min_p.z + margin * 2.0) / step) + 1
    var grid = PackedVector3Array()
    grid.resize(nx * nz)
    for iz in range(nz):
        for ix in range(nx):
            var x = min_p.x - margin + ix * step
            var z = min_p.z - margin + iz * step
            grid[iz * nx + ix] = Vector3(x, _terrain_height_at(x, z), z)
    var st = SurfaceTool.new()
    st.begin(Mesh.PRIMITIVE_TRIANGLES)
    for iz in range(nz - 1):
        for ix in range(nx - 1):
            var a = grid[iz * nx + ix]
            var b = grid[iz * nx + ix + 1]
            var c = grid[(iz + 1) * nx + ix + 1]
            var d = grid[(iz + 1) * nx + ix]
            var tint = 0.83 + noise.get_noise_2d(a.x * 0.02, a.z * 0.02) * 0.17
            var color = Color(tint, tint, tint)
            st.set_color(color)
            st.add_vertex(a)
            st.add_vertex(d)
            st.add_vertex(b)
            st.set_color(color)
            st.add_vertex(b)
            st.add_vertex(d)
            st.add_vertex(c)
    st.generate_normals()
    terrain_material = ShaderMaterial.new()
    terrain_material.shader = load("res://shaders/terrain.gdshader")
    var mesh = st.commit()
    var instance = MeshInstance3D.new()
    instance.name = "CarvedMountain"
    instance.mesh = mesh
    instance.material_override = terrain_material
    add_child(instance)
    _add_mesh_collision(mesh, instance, true)
    _build_distant_mountains(min_p, max_p)

func _build_distant_mountains(min_p: Vector3, max_p: Vector3) -> void:
    var centre = (min_p + max_p) * 0.5
    centre.y = min_p.y - 100.0
    var material = StandardMaterial3D.new()
    material.albedo_color = Color(0.095, 0.125, 0.115)
    material.roughness = 1.0
    for i in range(11):
        var angle = TAU * float(i) / 11.0
        var radius = 640.0 + rng.randf_range(-60.0, 130.0)
        var mesh = CylinderMesh.new()
        mesh.top_radius = 0.0
        mesh.bottom_radius = rng.randf_range(180.0, 290.0)
        mesh.height = rng.randf_range(270.0, 480.0)
        mesh.radial_segments = 9
        mesh.rings = 3
        var instance = MeshInstance3D.new()
        instance.mesh = mesh
        instance.material_override = material
        instance.position = centre + Vector3(cos(angle) * radius, mesh.height * 0.5, sin(angle) * radius)
        instance.rotation.y = rng.randf() * TAU
        add_child(instance)

func _build_guardrails() -> void:
    var rails: Array[Transform3D] = []
    var posts: Array[Transform3D] = []
    var reflectors: Array[Transform3D] = []
    var wall_st = SurfaceTool.new()
    wall_st.begin(Mesh.PRIMITIVE_TRIANGLES)
    for i in range(0, points.size() - 2, 2):
        for sign_side in [-1.0, 1.0]:
            var offset = sign_side * (road_width * 0.5 + 0.72)
            var a = _road_vertex(i, offset, 0.58)
            var b = _road_vertex(i + 2, offset, 0.58)
            var direction = (b - a).normalized()
            var local_basis = Basis.looking_at(direction, Vector3.UP)
            var rail_basis = local_basis * Basis.from_scale(Vector3(0.12, 0.20, a.distance_to(b)))
            rails.append(Transform3D(rail_basis, (a + b) * 0.5))
            posts.append(Transform3D(Basis.from_scale(Vector3(0.13, 0.82, 0.13)), a - Vector3.UP * 0.34))
            if i % 12 == 0:
                reflectors.append(Transform3D(local_basis * Basis.from_scale(Vector3(0.15, 0.10, 0.035)), a + Vector3.UP * 0.02 - direction * 0.08))
            var lower_a = a - Vector3.UP * 0.48
            var lower_b = b - Vector3.UP * 0.48
            var upper_a = a + Vector3.UP * 0.28
            var upper_b = b + Vector3.UP * 0.28
            _add_quad(wall_st, lower_a, lower_b, upper_b, upper_a, Vector3.UP)
    var rail_material = StandardMaterial3D.new()
    rail_material.albedo_color = Color(0.43, 0.47, 0.46)
    rail_material.metallic = 0.72
    rail_material.roughness = 0.38
    _make_multimesh("GuardrailBeams", _unit_box(), rail_material, rails)
    var post_material = StandardMaterial3D.new()
    post_material.albedo_color = Color(0.24, 0.27, 0.26)
    post_material.metallic = 0.6
    _make_multimesh("GuardrailPosts", _unit_box(), post_material, posts)
    var reflector_material = StandardMaterial3D.new()
    reflector_material.albedo_color = Color(0.95, 0.15, 0.055)
    reflector_material.emission_enabled = true
    reflector_material.emission = Color(0.7, 0.035, 0.01)
    reflector_material.emission_energy_multiplier = 1.5
    _make_multimesh("Reflectors", _unit_box(), reflector_material, reflectors)
    var wall_mesh = wall_st.commit()
    var body = StaticBody3D.new()
    body.name = "GuardrailCollision"
    var collider = CollisionShape3D.new()
    var shape = wall_mesh.create_trimesh_shape()
    shape.backface_collision = true
    collider.shape = shape
    body.add_child(collider)
    add_child(body)

func _build_vegetation() -> void:
    var trunks: Array[Transform3D] = []
    var canopy_low: Array[Transform3D] = []
    var canopy_mid: Array[Transform3D] = []
    var canopy_top: Array[Transform3D] = []
    var colours: Array[Color] = []
    var rocks: Array[Transform3D] = []
    var autumn = data["foliage"] == "autumn"
    for i in range(4, points.size() - 4, 3):
        for sign_side in [-1.0, 1.0]:
            var count = 2 if rng.randf() > 0.35 else 1
            for n in range(count):
                var offset = rng.randf_range(11.0, 67.0)
                var base = points[i] + side_at(i) * sign_side * offset
                base.x += rng.randf_range(-5.0, 5.0)
                base.z += rng.randf_range(-5.0, 5.0)
                base.y = _terrain_height_at(base.x, base.z)
                var size = rng.randf_range(0.72, 1.42)
                var tree_basis = Basis(Vector3.UP, rng.randf() * TAU) * Basis.from_scale(Vector3.ONE * size)
                trunks.append(Transform3D(tree_basis, base + Vector3.UP * 1.65 * size))
                if autumn:
                    canopy_low.append(Transform3D(tree_basis * Basis.from_scale(Vector3(2.7, 2.25, 2.7)), base + Vector3.UP * 4.6 * size))
                    var palette = [Color("a43e22"), Color("d07225"), Color("d5a23c"), Color("717736"), Color("8f3024")]
                    colours.append(palette[rng.randi_range(0, palette.size() - 1)])
                else:
                    canopy_low.append(Transform3D(tree_basis, base + Vector3.UP * 3.25 * size))
                    canopy_mid.append(Transform3D(tree_basis, base + Vector3.UP * 5.15 * size))
                    canopy_top.append(Transform3D(tree_basis, base + Vector3.UP * 6.65 * size))
                    colours.append(Color(0.72 + rng.randf() * 0.22, 0.79 + rng.randf() * 0.17, 0.70 + rng.randf() * 0.2))
            if rng.randf() > 0.67:
                var rock_pos = points[i] + side_at(i) * sign_side * rng.randf_range(7.0, 16.0)
                rock_pos.y = _terrain_height_at(rock_pos.x, rock_pos.z)
                var rock_basis = Basis(Vector3.UP, rng.randf() * TAU) * Basis.from_scale(Vector3(rng.randf_range(0.5, 1.7), rng.randf_range(0.4, 1.1), rng.randf_range(0.6, 1.5)))
                rocks.append(Transform3D(rock_basis, rock_pos + Vector3.UP * 0.3))
    var trunk_mesh = CylinderMesh.new()
    trunk_mesh.top_radius = 0.11
    trunk_mesh.bottom_radius = 0.19
    trunk_mesh.height = 3.3
    trunk_mesh.radial_segments = 6
    var bark = StandardMaterial3D.new()
    bark.albedo_color = Color(0.19, 0.125, 0.075)
    bark.roughness = 1.0
    _make_multimesh("TreeTrunks", trunk_mesh, bark, trunks)
    var leaves = StandardMaterial3D.new()
    leaves.albedo_color = Color(0.12, 0.24, 0.11) if not autumn else Color.WHITE
    leaves.roughness = 0.94
    leaves.vertex_color_use_as_albedo = true
    if autumn:
        var crown = SphereMesh.new()
        crown.radius = 1.0
        crown.height = 2.0
        crown.radial_segments = 8
        crown.rings = 4
        _make_multimesh("AutumnCrowns", crown, leaves, canopy_low, colours)
    else:
        for layer in range(3):
            var cone = CylinderMesh.new()
            cone.top_radius = 0.0
            cone.bottom_radius = 1.75 - layer * 0.32
            cone.height = 3.6 - layer * 0.25
            cone.radial_segments = 8
            var transforms = canopy_low if layer == 0 else (canopy_mid if layer == 1 else canopy_top)
            _make_multimesh("CedarLayer" + str(layer), cone, leaves, transforms, colours)
    var rock_mesh = SphereMesh.new()
    rock_mesh.radius = 1.0
    rock_mesh.height = 1.5
    rock_mesh.radial_segments = 7
    rock_mesh.rings = 3
    var rock_mat = StandardMaterial3D.new()
    rock_mat.albedo_color = Color(0.25, 0.26, 0.23)
    rock_mat.roughness = 1.0
    _make_multimesh("RoadsideRocks", rock_mesh, rock_mat, rocks)

func _build_details() -> void:
    _gantry(2, "TOUGE / APEX   •   START")
    _gantry(points.size() - 3, "FINISH   •   " + data["name"])
    var yellow = StandardMaterial3D.new()
    yellow.albedo_color = Color(0.94, 0.63, 0.12)
    yellow.roughness = 0.7
    var dark = StandardMaterial3D.new()
    dark.albedo_color = Color(0.055, 0.06, 0.055)
    var last_sign = -100.0
    for i in range(12, points.size() - 12, 8):
        var before = tangent_at(i - 6)
        var after = tangent_at(i + 6)
        var angle = before.angle_to(after)
        if angle < 0.48 or cumulative[i] - last_sign < 32.0:
            continue
        last_sign = cumulative[i]
        var turn = before.cross(after).y
        var outer = -signf(turn)
        var p = _road_vertex(i, outer * (road_width * 0.5 + 1.35))
        var sign_basis = Basis.looking_at(tangent_at(i), Vector3.UP)
        _box(Vector3(0.09, 1.35, 0.09), p + Vector3.UP * 0.67, dark)
        var board = _box(Vector3(1.75, 0.65, 0.09), p + Vector3.UP * 1.32, yellow)
        board.basis = sign_basis
        var label = Label3D.new()
        label.text = ">>" if turn > 0.0 else "<<"
        label.font_size = 72
        label.pixel_size = 0.009
        label.modulate = Color(0.04, 0.045, 0.04)
        label.outline_size = 0
        label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
        label.no_depth_test = false
        label.position = p + Vector3.UP * 1.32
        add_child(label)

func _gantry(index: int, text: String) -> void:
    var p = points[index]
    var tangent = tangent_at(index)
    var side = side_at(index)
    var gantry_basis = Basis.looking_at(tangent, Vector3.UP)
    var metal = StandardMaterial3D.new()
    metal.albedo_color = Color(0.07, 0.085, 0.09)
    metal.metallic = 0.65
    metal.roughness = 0.45
    for s in [-1.0, 1.0]:
        var post = _box(Vector3(0.16, 4.3, 0.16), p + side * s * (road_width * 0.5 + 0.85) + Vector3.UP * 2.15, metal)
        post.basis = gantry_basis
    var beam = _box(Vector3(road_width + 2.0, 0.56, 0.28), p + Vector3.UP * 4.12, metal)
    beam.basis = gantry_basis
    var label = Label3D.new()
    label.text = text
    label.font_size = 64
    label.pixel_size = 0.009
    label.modulate = Color(0.94, 0.96, 0.92)
    label.outline_size = 8
    label.outline_modulate = Color(0.025, 0.03, 0.03)
    label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
    label.position = p + Vector3.UP * 4.15
    add_child(label)

func _box(size: Vector3, position_3d: Vector3, material: Material) -> MeshInstance3D:
    var mesh = BoxMesh.new()
    mesh.size = size
    var instance = MeshInstance3D.new()
    instance.mesh = mesh
    instance.material_override = material
    instance.position = position_3d
    add_child(instance)
    return instance

func _unit_box() -> BoxMesh:
    var mesh = BoxMesh.new()
    mesh.size = Vector3.ONE
    return mesh

func _make_multimesh(label: String, mesh: Mesh, material: Material, transforms: Array[Transform3D], colours: Array[Color] = []) -> void:
    if transforms.is_empty():
        return
    var instance = MultiMeshInstance3D.new()
    instance.name = label
    instance.material_override = material
    var mm = MultiMesh.new()
    mm.transform_format = MultiMesh.TRANSFORM_3D
    if not colours.is_empty():
        mm.use_colors = true
    mm.mesh = mesh
    mm.instance_count = transforms.size()
    for i in range(transforms.size()):
        mm.set_instance_transform(i, transforms[i])
        if not colours.is_empty():
            mm.set_instance_color(i, colours[mini(i, colours.size() - 1)])
    instance.multimesh = mm
    add_child(instance)

func _add_mesh_collision(mesh: Mesh, parent_instance: Node, backface: bool) -> void:
    var body = StaticBody3D.new()
    body.name = "Collision"
    var physics_material = PhysicsMaterial.new()
    physics_material.friction = 1.0
    physics_material.rough = true
    body.physics_material_override = physics_material
    var collider = CollisionShape3D.new()
    var shape = mesh.create_trimesh_shape()
    shape.backface_collision = backface
    collider.shape = shape
    body.add_child(collider)
    parent_instance.add_child(body)

shader_type spatial;
render_mode cull_disabled;

uniform float wetness : hint_range(0.0, 1.0) = 0.0;
uniform vec3 asphalt_tint : source_color = vec3(0.19, 0.205, 0.21);
varying vec3 world_pos;

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}
float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), f.x),
               mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0)), f.x), f.y);
}
void vertex() {
    world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
}
void fragment() {
    float grain = noise(world_pos.xz * 32.0) * 0.055 + noise(world_pos.xz * 3.0) * 0.045;
    float repaired = smoothstep(0.56, 0.72, noise(world_pos.xz * 0.12));
    vec3 dry = asphalt_tint * (0.78 + grain) * (1.0 - repaired * 0.12);
    ALBEDO = mix(dry, dry * 0.46, wetness);
    ROUGHNESS = mix(0.88, 0.19, wetness) + grain * 0.3;
    METALLIC = wetness * 0.16;
    SPECULAR = mix(0.28, 0.72, wetness);
}

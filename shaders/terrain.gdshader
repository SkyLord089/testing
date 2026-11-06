shader_type spatial;
render_mode cull_disabled;

uniform float wetness : hint_range(0.0, 1.0) = 0.0;
varying vec3 world_pos;
varying vec3 world_normal;

float hash(vec2 p) { return fract(sin(dot(p, vec2(41.3, 289.1))) * 43758.54); }
float noise(vec2 p) {
    vec2 i = floor(p); vec2 f = fract(p); f = f*f*(3.0-2.0*f);
    return mix(mix(hash(i), hash(i+vec2(1,0)), f.x), mix(hash(i+vec2(0,1)), hash(i+vec2(1,1)), f.x), f.y);
}
void vertex() {
    world_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz;
    world_normal = normalize((MODEL_MATRIX * vec4(NORMAL, 0.0)).xyz);
}
void fragment() {
    float n = noise(world_pos.xz * 0.09) * 0.6 + noise(world_pos.xz * 0.8) * 0.4;
    float rock = 1.0 - smoothstep(0.52, 0.83, abs(world_normal.y));
    vec3 moss = mix(vec3(0.075,0.105,0.067), vec3(0.16,0.19,0.105), n);
    vec3 stone = mix(vec3(0.17,0.175,0.16), vec3(0.28,0.27,0.23), n);
    ALBEDO = mix(moss, stone, rock) * COLOR.rgb * (1.0 - wetness * 0.25);
    ROUGHNESS = mix(0.96, 0.65, wetness);
}

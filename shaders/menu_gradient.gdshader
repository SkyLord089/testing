shader_type canvas_item;
void fragment() {
    float left = 1.0 - smoothstep(0.18, 0.72, UV.x);
    float bottom = smoothstep(0.55, 1.0, UV.y) * 0.38;
    float alpha = max(left * 0.94, bottom);
    COLOR = vec4(vec3(0.018, 0.026, 0.034), alpha);
}

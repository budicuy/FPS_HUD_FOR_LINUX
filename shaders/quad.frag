#version 450

layout(set = 0, binding = 0) uniform sampler2D u_hud_tex;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;

void main() {
    vec4 sample_col = texture(u_hud_tex, v_uv);
    if (sample_col.a <= 0.001) {
        discard;
    }
    o_color = sample_col;
}

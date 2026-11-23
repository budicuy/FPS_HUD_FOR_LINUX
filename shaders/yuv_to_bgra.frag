#version 450

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D tex_y;
layout(set = 0, binding = 1) uniform sampler2D tex_uv_or_u;
layout(set = 0, binding = 2) uniform sampler2D tex_v;

layout(push_constant) uniform Push {
    uint format_mode; // 0 = NV12, 1 = YUV420p
    uint color_space; // 0 = Bt709Limited, 1 = Bt709Full, 2 = Bt601Limited, 3 = Bt601Full
} pc;

void main() {
    float y_raw = texture(tex_y, v_uv).r;
    float u_raw;
    float v_raw;
    if (pc.format_mode == 0u) {
        vec2 uv = texture(tex_uv_or_u, v_uv).rg;
        u_raw = uv.x;
        v_raw = uv.y;
    } else {
        u_raw = texture(tex_uv_or_u, v_uv).r;
        v_raw = texture(tex_v, v_uv).r;
    }

    float y;
    float u;
    float v;
    bool is_full = (pc.color_space == 1u || pc.color_space == 3u);
    if (is_full) {
        y = y_raw;
        u = u_raw - (128.0 / 255.0);
        v = v_raw - (128.0 / 255.0);
    } else {
        y = (y_raw - (16.0 / 255.0)) * (255.0 / 219.0);
        u = (u_raw - (128.0 / 255.0)) * (255.0 / 224.0);
        v = (v_raw - (128.0 / 255.0)) * (255.0 / 224.0);
    }

    vec3 rgb;
    if (pc.color_space <= 1u) {
        // BT.709
        rgb.r = y + 1.5748 * v;
        rgb.g = y - 0.187324 * u - 0.468124 * v;
        rgb.b = y + 1.8556 * u;
    } else {
        // BT.601
        rgb.r = y + 1.402 * v;
        rgb.g = y - 0.344136 * u - 0.714136 * v;
        rgb.b = y + 1.772 * u;
    }

    out_color = vec4(clamp(rgb, 0.0, 1.0), 1.0);
}

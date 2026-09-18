#version 450
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(set = 1, binding = 0) uniform sampler2D vis;

layout(push_constant) uniform PC {
    vec2 inv_size;
    vec2 tex_size;
    uint pass;
    uint wrap;
    uint point;
    uint use_vis;
} pc;

layout(location = 0) in vec4 v_color;
layout(location = 1) in vec3 v_stq;
layout(location = 0) out vec4 out_color;

float wrap1(float tc, uint mode, float size) {
    if (mode == 0u) return tc - floor(tc / size) * size;
    return clamp(tc, 0.0, size - 1.0);
}

void main() {
    float frac = pc.use_vis != 0u ? texelFetch(vis, ivec2(0), 0).r : 1.0;
    float v = floor(frac * 255.0 / 2.0 + 0.001) / 128.0;
    vec2 tc = (v_stq.z != 0.0 ? v_stq.xy / v_stq.z : v_stq.xy) * pc.tex_size;
    vec4 t;
    if (pc.point != 0u) {
        ivec2 size = textureSize(tex, 0);
        vec2 w = vec2(wrap1(tc.x, pc.wrap & 3u, pc.tex_size.x),
                      wrap1(tc.y, (pc.wrap >> 2) & 3u, pc.tex_size.y));
        t = texelFetch(tex, clamp(ivec2(floor(w)), ivec2(0), size - 1), 0);
    } else {
        tc -= 0.5;
        if ((pc.wrap & 3u) != 0u) tc.x = clamp(tc.x, 0.0, pc.tex_size.x - 1.0);
        if (((pc.wrap >> 2) & 3u) != 0u) tc.y = clamp(tc.y, 0.0, pc.tex_size.y - 1.0);
        t = texture(tex, (tc + 0.5) / pc.tex_size);
    }
    vec3 rgb = pc.pass == 2u ? t.rgb * v : t.rgb;
    rgb = min(rgb * v_color.rgb, vec3(1.0));
    float a = clamp(v * v_color.a, 0.0, 1.0);
    out_color = vec4(rgb, a);
}

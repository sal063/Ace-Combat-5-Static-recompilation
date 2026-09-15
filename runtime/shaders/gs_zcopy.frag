#version 450
layout(location = 1) in vec3 v_stq;
layout(location = 0) out vec4 out_color;

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(push_constant) uniform PC {
    vec2  inv_size;
    vec2  tex_size;
    uvec4 flags;
    vec4  fogcol;
    uvec4 clamp_uv;
    vec2  img_size;
    float scale;
    float point_size;
    float tex_scale;
    float pad0;
} pc;

void main() {
    bool fst = ((pc.flags.x >> 2) & 1u) != 0u;
    vec2 tc = fst ? v_stq.xy : (v_stq.xy / max(v_stq.z, 1e-9)) * pc.tex_size;
    tc = clamp(tc, vec2(0.0), max(pc.tex_size - 1.0, vec2(0.0)));
    float d = texture(tex, (tc + 0.5) / pc.img_size).r;
    gl_FragDepth = d;
    out_color = vec4(d, 0.0, 0.0, 1.0);
}

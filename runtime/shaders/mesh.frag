#version 450
layout(location = 0) in vec4 v_color;
layout(location = 1) in vec3 v_stq;
layout(location = 2) in float v_fog;
layout(location = 3) flat in uint v_round_uv;

layout(location = 0, index = 0) out vec4 out_color;
layout(location = 0, index = 1) out vec4 out_blend;

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
    vec4 c = v_color;
    uint f = pc.flags.x;
    uint tfx = f & 3u;
    bool ate = ((f >> 3) & 1u) != 0u;
    uint atst = (f >> 4) & 7u;
    float aref = float((f >> 8) & 0xFFu) / 128.0;
    bool fge = ((f >> 16) & 1u) != 0u;
    bool tcc = ((f >> 17) & 1u) != 0u;
    bool afkeep = ((f >> 18) & 1u) != 0u;
    bool point = ((f >> 19) & 1u) != 0u;
    bool afonly = ((f >> 20) & 1u) != 0u;
    bool fba = ((f >> 21) & 1u) != 0u;

    if (pc.flags.y != 0u) {
        vec2 tc = (v_stq.xy / max(v_stq.z, 1e-9)) * pc.tex_size;
        uint wms = pc.flags.z & 3u, wmt = (pc.flags.z >> 2) & 3u;
        bool region_image = ((pc.flags.z >> 8) & 1u) != 0u;
        vec2 org = vec2(0.0);
        if (region_image) {
            if (wms == 2u) org.x = float(pc.clamp_uv.x);
            if (wmt == 2u) org.y = float(pc.clamp_uv.z);
        }
        vec2 uv = (tc - org) / pc.img_size;
        vec2 lo = 0.5 / pc.img_size;
        vec2 hi = (pc.tex_size - 0.5) / pc.img_size;
        if (wms == 2u) {
            lo.x = (float(pc.clamp_uv.x) - org.x + 0.5) / pc.img_size.x;
            hi.x = (float(pc.clamp_uv.y) - org.x + 0.5) / pc.img_size.x;
        }
        if (wmt == 2u) {
            lo.y = (float(pc.clamp_uv.z) - org.y + 0.5) / pc.img_size.y;
            hi.y = (float(pc.clamp_uv.w) - org.y + 0.5) / pc.img_size.y;
        }
        if (wms == 1u || wms == 2u) uv.x = clamp(uv.x, lo.x, hi.x);
        if (wmt == 1u || wmt == 2u) uv.y = clamp(uv.y, lo.y, hi.y);
        vec4 t;
        if (point) {
            ivec2 size = textureSize(tex, 0);
            ivec2 p = ivec2(floor(uv * vec2(size)));
            p = ((p % size) + size) % size;
            t = texelFetch(tex, p, 0);
        } else {
            t = texture(tex, uv);
        }
        float af = c.a;
        vec3 rgb;
        float a;
        if (tfx == 1u) {
            rgb = t.rgb;
            a   = tcc ? t.a : af;
        } else if (tfx == 0u) {
            rgb = t.rgb * c.rgb;
            a   = tcc ? t.a * af : af;
        } else if (tfx == 2u) {
            rgb = t.rgb * c.rgb + af * (128.0 / 255.0);
            a   = tcc ? t.a + af : af;
        } else {
            rgb = t.rgb * c.rgb + af * (128.0 / 255.0);
            a   = tcc ? t.a : af;
        }
        c = vec4(rgb, a);
    } else {
        c.rgb *= 128.0 / 255.0;
    }

    if (fge) c.rgb = mix(pc.fogcol.rgb, c.rgb, clamp(v_fog, 0.0, 1.0));

    if (ate) {
        bool pass;
        if      (atst == 0u) pass = false;
        else if (atst == 1u) pass = true;
        else if (atst == 2u) pass = c.a <  aref;
        else if (atst == 3u) pass = c.a <= aref;
        else if (atst == 4u) pass = abs(c.a - aref) < (0.5 / 128.0);
        else if (atst == 5u) pass = c.a >= aref;
        else if (atst == 6u) pass = c.a >  aref;
        else                 pass = abs(c.a - aref) >= (0.5 / 128.0);
        if (afonly ? pass : (!pass && afkeep)) discard;
    }

    out_blend = vec4(0.0, 0.0, 0.0, clamp(c.a, 0.0, 1.0));
    if (fba) c.a = max(c.a, 1.0);
    out_color = clamp(c, vec4(0.0), vec4(2.0));
}

#version 450

layout(location = 0) in vec4  in_pos;
layout(location = 1) in ivec4 in_nrm;
layout(location = 2) in ivec4 in_col;
layout(location = 3) in vec4  in_uv;

layout(location = 0) out vec4 v_color;
layout(location = 1) out vec3 v_stq;
layout(location = 2) out float v_fog;
layout(location = 3) flat out uint v_round_uv;

out gl_PerVertex {
    vec4 gl_Position;
    float gl_PointSize;
    float gl_ClipDistance[1];
};

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
    uint  mesh_block;
    uint  mesh_pad;
} pc;

struct Block {
    vec4  m[4];
    vec4  c[4];
    vec4  l[3];
    vec4  r[4];
    vec4  e[3];
    float fog0, fog1, zscale, zmax;
    float xoff, yoff, spec_alpha, pad0;
    uint  mode, pass, clip, pad1;
    uint  prog, pad2, pad3, pad4;
    vec4  p[4];
};

layout(std430, set = 2, binding = 0) readonly buffer Blocks { Block b[]; };

void main() {
    Block k = b[pc.mesh_block];
    vec3 p = in_pos.xyz;
    vec4 s = k.m[0] * p.x + k.m[1] * p.y + k.m[2] * p.z + k.m[3];
    float w = s.w;

    float X = (s.x + (0.5 - k.xoff) * w) * pc.inv_size.x - w;
    float Y = (s.y + (0.5 - k.yoff) * w) * pc.inv_size.y - w;
    float zlim = min(16777215.0, k.zmax);
    float Z = min(max(s.z * k.zscale, 0.0), zlim * max(w, 0.0)) / k.zmax;
    gl_Position = vec4(X, Y, Z, w);

    vec4 cc = k.c[0] * p.x + k.c[1] * p.y + k.c[2] * p.z + k.c[3];
    gl_ClipDistance[0] = k.clip != 0u ? cc.z + cc.w : 1.0;

    float fog = clamp(k.fog0 + w * k.fog1, 0.0, 255.0);
    v_fog = fog / 255.0;
    v_round_uv = 0u;
    gl_PointSize = pc.point_size;

    if (k.prog == 5u) {
        gl_Position = vec4(in_pos.x * pc.inv_size.x - 1.0,
                           in_pos.y * pc.inv_size.y - 1.0, in_pos.z, 1.0);
        gl_ClipDistance[0] = 1.0;
        v_stq = in_uv.xyz;
        v_color = vec4(in_col) / 128.0;
        v_fog = in_uv.w;
        v_round_uv = uint(in_nrm.x);
        return;
    }

    if (k.prog == 3u) {
        float iw = 1.0 / w;
        vec3 c = s.xyz * iw;
        vec2 corner = c.xy + vec2(in_nrm.xy) * k.p[0].xy * in_uv.z * iw;
        float zg = clamp(floor(floor(c.z * 16.0) / 8.0), k.p[0].z, zlim);
        gl_Position = c.z > 0.0
            ? vec4((corner.x + 0.5 - k.xoff) * pc.inv_size.x - 1.0,
                   (corner.y + 0.5 - k.yoff) * pc.inv_size.y - 1.0, zg / k.zmax, 1.0)
            : vec4(2.0, 2.0, 2.0, 1.0);
        gl_ClipDistance[0] = 1.0;
        v_stq = vec3(in_uv.xy, 1.0);
        v_color = vec4(in_col & 255) / 128.0;
        v_fog = 1.0;
        return;
    }

    if (k.prog == 4u) {
        v_stq = in_uv.xyz;
        v_color = vec4(in_col & 255) / 128.0;
        return;
    }

    if (k.prog == 2u) {
        if (k.pass == 0u) {
            v_stq = vec3(in_uv.xy, 1.0);
            v_color = vec4(in_col & 255) / 128.0;
        } else {
            v_stq = vec3(in_uv.xy * 2.0, 1.0);
            v_color = vec4(vec3(clamp((k.p[0].z - cc.w) * k.p[0].w * 128.0, 0.0, 128.0)),
                           128.0) / 128.0;
        }
        return;
    }

    if (k.prog == 1u) {
        vec2 uv;
        vec4 col;
        if (k.pass == 0u) {
            uv = in_uv.xy + in_uv.zw;
            col = vec4(in_col & 255);
        } else if (k.pass == 1u) {
            uv = in_uv.xy * 16.0;
            col = vec4(vec3(clamp((k.p[0].z - cc.w) * k.p[0].w * 128.0, 0.0, 128.0)),
                       k.p[3].x);
        } else if (k.pass == 2u) {
            uv = in_uv.xy * 16.0 + k.p[1].xy;
            col = vec4(vec3(clamp((k.p[1].z - cc.w) * k.p[1].w * 128.0, 0.0, 128.0)),
                       fog * 0.5);
        } else {
            uv = (p.xz - k.p[2].xz) * k.p[2].w;
            col = vec4(fog * 0.5);
        }
        v_stq = vec3(uv, 1.0);
        v_color = col / 128.0;
        return;
    }

    if (k.pass == 0u) {
        v_stq = vec3(in_uv.xy, 1.0);
        if (k.mode == 0u) {
            v_color = vec4(in_col & 255) / 128.0;
        } else {
            vec3 n = vec3(in_nrm.xyz) / 4096.0;
            vec4 l = vec4(k.l[0].xyz * n.x + k.l[1].xyz * n.y + k.l[2].xyz * n.z, 1.0);
            l = l * l * l;
            l.xyz *= 2.0;
            l = max(l, 0.0);
            vec4 li = max(k.r[0] * l.x + k.r[1] * l.y + k.r[2] * l.z + k.r[3] * l.w, 0.0);
            vec3 c = min(vec3(in_col.xyz) * li.xyz, vec3(255.0));
            v_color = vec4(c, 128.0) / 128.0;
        }
    } else {
        vec3 n = vec3(in_nrm.xyz) / 4096.0;
        vec3 nb = k.e[0].xyz * n.x + k.e[1].xyz * n.y + k.e[2].xyz * n.z;
        v_stq = vec3((nb.x + 1.0) * 0.5, (nb.z + 1.0) * 0.5, 1.0);
        float spec = min(max(nb.y, 0.0) * 1280.0, 128.0);
        v_color = vec4(vec3(spec), k.spec_alpha) / 128.0;
    }
}

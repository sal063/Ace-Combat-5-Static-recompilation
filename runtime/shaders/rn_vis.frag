#version 450
layout(set = 0, binding = 0) uniform sampler2D depth_img;
layout(set = 1, binding = 0) uniform sampler2D color_img;

layout(push_constant) uniform PC {
    vec4  box;
    float sun_z;
    float use_alpha;
} pc;

layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;

void main() {
    ivec2 size = textureSize(depth_img, 0);
    ivec2 p0 = clamp(ivec2(floor(pc.box.xy)), ivec2(0), size);
    ivec2 p1 = clamp(ivec2(floor(pc.box.zw + 0.5)), ivec2(0), size);
    ivec2 n = max(p1 - p0, ivec2(0));
    int nx = min(n.x, 64), ny = min(n.y, 64);
    float sum = 0.0;
    for (int j = 0; j < ny; j++) {
        for (int i = 0; i < nx; i++) {
            ivec2 q = p0 + ivec2((vec2(i, j) + 0.5) * vec2(n) / vec2(nx, ny));
            float d = texelFetch(depth_img, q, 0).r;
            float w = pc.sun_z >= d ? 1.0 : 0.0;
            if (pc.use_alpha > 0.5)
                w *= clamp(1.0 - 2.0 * texelFetch(color_img, q, 0).a, 0.0, 1.0);
            sum += w;
        }
    }
    float frac = (nx > 0 && ny > 0) ? sum / float(nx * ny) : 0.0;
    out_color = vec4(frac, frac, frac, 1.0);
}

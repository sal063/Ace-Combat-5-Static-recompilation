#version 450
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 out_color;
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform PC {
    vec4 map;
    vec4 bound;
    vec4 opts;
    vec4 grade;
} pc;

vec2 texel;
vec2 lo, hi;

vec3 tap(vec2 uv) { return texture(src, clamp(uv, lo, hi)).rgb; }

vec3 filtered(vec2 uv, vec2 dtex) {
    vec2 size = vec2(textureSize(src, 0));
    if (pc.opts.x < 0.5) {
        ivec2 p = ivec2(floor(clamp(uv, lo, hi) * size));
        return texelFetch(src, p, 0).rgb;
    }
    if (pc.opts.x > 1.5) {
        vec2 t = uv * size - 0.5;
        vec2 scale = max(1.0 / max(dtex, vec2(1e-6)), vec2(1.0));
        vec2 f = fract(t);
        vec2 region = 0.5 - 0.5 / scale;
        vec2 d = f - 0.5;
        f = (d - clamp(d, -region, region)) * scale + 0.5;
        return tap((floor(t) + f + 0.5) / size);
    }
    return tap(uv);
}

vec3 downsample(vec2 uv, vec2 dtex, vec2 one) {
    ivec2 n = ivec2(clamp(ceil(dtex - 0.01), vec2(1.0), vec2(4.0)));
    vec2 step = dtex / vec2(n) * one;
    vec2 first = uv - 0.5 * dtex * one + 0.5 * step;
    vec3 sum = vec3(0.0);
    for (int y = 0; y < n.y; y++)
        for (int x = 0; x < n.x; x++)
            sum += tap(first + vec2(float(x), float(y)) * step);
    return sum / float(n.x * n.y);
}

vec3 fxaa(vec2 uv, vec3 rgbM) {
    const vec3 L = vec3(0.299, 0.587, 0.114);
    vec3 rgbNW = tap(uv + vec2(-1.0, -1.0) * texel);
    vec3 rgbNE = tap(uv + vec2( 1.0, -1.0) * texel);
    vec3 rgbSW = tap(uv + vec2(-1.0,  1.0) * texel);
    vec3 rgbSE = tap(uv + vec2( 1.0,  1.0) * texel);
    float lNW = dot(rgbNW, L), lNE = dot(rgbNE, L);
    float lSW = dot(rgbSW, L), lSE = dot(rgbSE, L), lM = dot(rgbM, L);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));
    vec2 dir = vec2(-((lNW + lNE) - (lSW + lSE)), (lNW + lSW) - (lNE + lSE));
    float reduce = max((lNW + lNE + lSW + lSE) * 0.03125, 1.0 / 128.0);
    float rcp = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * rcp, vec2(-8.0), vec2(8.0)) * texel;
    vec3 a = 0.5 * (tap(uv + dir * (1.0 / 3.0 - 0.5)) + tap(uv + dir * (2.0 / 3.0 - 0.5)));
    vec3 b = a * 0.5 + 0.25 * (tap(uv - dir * 0.5) + tap(uv + dir * 0.5));
    float lB = dot(b, L);
    return (lB < lMin || lB > lMax) ? a : b;
}

vec3 sharpen(vec2 uv, vec3 c, float amount) {
    vec3 n = tap(uv + vec2(0.0, -texel.y));
    vec3 s = tap(uv + vec2(0.0,  texel.y));
    vec3 e = tap(uv + vec2( texel.x, 0.0));
    vec3 w = tap(uv + vec2(-texel.x, 0.0));
    vec3 mn = min(c, min(min(n, s), min(e, w)));
    vec3 mx = max(c, max(max(n, s), max(e, w)));
    vec3 blur = (n + s + e + w) * 0.25;
    return clamp(c + (c - blur) * (amount * 2.0), mn, mx);
}

void main() {
    vec2 uv = v_uv * pc.map.xy + pc.map.zw;
    vec2 size = vec2(textureSize(src, 0));
    vec2 dtex = fwidth(uv * size);
    bool shrink = max(dtex.x, dtex.y) > 1.01;
    if (any(lessThan(v_uv, pc.bound.xy)) || any(greaterThan(v_uv, pc.bound.zw))) {
        out_color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    if (pc.grade.w < 0.5 && pc.opts.x > 0.5 && pc.opts.x < 1.5 && !shrink) {
        out_color = vec4(texture(src, uv).rgb, 1.0);
        return;
    }
    texel = 1.0 / size;
    lo = (pc.bound.xy * pc.map.xy + pc.map.zw) + 0.5 * texel;
    hi = (pc.bound.zw * pc.map.xy + pc.map.zw) - 0.5 * texel;
    vec3 c = shrink && pc.opts.x > 0.5 ? downsample(uv, dtex, texel)
                                       : filtered(uv, dtex);
    if (shrink) texel *= max(dtex, vec2(1.0));
    if (pc.opts.y > 0.5) c = fxaa(uv, c);
    if (pc.opts.z > 0.001) c = sharpen(uv, c, pc.opts.z);
    c += pc.grade.x;
    c = (c - 0.5) * pc.grade.y + 0.5;
    if (abs(pc.grade.z - 1.0) > 0.001) c = pow(max(c, vec3(0.0)), vec3(1.0 / pc.grade.z));
    if (abs(pc.opts.w - 1.0) > 0.001) c = mix(vec3(dot(c, vec3(0.299, 0.587, 0.114))), c, pc.opts.w);
    out_color = vec4(clamp(c, 0.0, 1.0), 1.0);
}

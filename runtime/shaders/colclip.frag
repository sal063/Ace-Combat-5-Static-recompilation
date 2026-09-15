#version 450
layout(location = 0) out vec4 out_color;
layout(set = 0, binding = 0) uniform sampler2D src;

void main() {
    vec4 c = texelFetch(src, ivec2(gl_FragCoord.xy), 0);
    vec3 v = mod(floor(c.rgb * 255.0 + 0.5), 256.0);
    out_color = vec4(v / 255.0, clamp(c.a, 0.0, 1.0));
}

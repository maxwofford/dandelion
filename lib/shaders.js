// gloam shader sources — shared between browser and CLI
// these are the same GLSL 300 es shaders from index.html

const bg_vs = `#version 300 es
layout(location=0) in vec2 a_pos;
out vec2 v_uv;
void main() {
  v_uv = a_pos * 0.5 + 0.5;
  gl_Position = vec4(a_pos, 0.0, 1.0);
}`;

const bg_fs = `#version 300 es
precision highp float;
in vec2 v_uv;
out vec4 o_color;
float hash(vec2 p) { return fract(sin(dot(p, vec2(127.1,311.7))) * 43758.5453); }
void main() {
  vec3 top = vec3(0.02, 0.02, 0.09);
  vec3 mid = vec3(0.06, 0.03, 0.07);
  vec3 bot = vec3(0.05, 0.02, 0.02);
  float y = v_uv.y;
  vec3 col = y > 0.5 ? mix(mid, top, (y-0.5)*2.0) : mix(bot, mid, y*2.0);
  col += (hash(v_uv * 200.0) - 0.5) * 0.012;
  o_color = vec4(col, 1.0);
}`;

const node_vs = `#version 300 es
layout(location=0) in vec2 a_pos;
layout(location=1) in vec2 a_offset;
layout(location=2) in float a_radius;
layout(location=3) in vec4 a_color;
layout(location=4) in float a_age;
uniform mat4 u_mvp;
out vec2 v_uv;
out vec4 v_color;
out float v_age;
void main() {
  v_uv = a_pos;
  v_color = a_color;
  v_age = a_age;
  vec2 world = a_offset + a_pos * a_radius * 2.0;
  gl_Position = u_mvp * vec4(world, 0.0, 1.0);
}`;

const node_fs = `#version 300 es
precision highp float;
in vec2 v_uv;
in vec4 v_color;
in float v_age;
out vec4 o_color;
void main() {
  float d = length(v_uv);
  float circle = smoothstep(0.55, 0.42, d);
  float halo = exp(-d * 3.0) * 0.2;
  float birth = smoothstep(0.0, 1.0, v_age);
  float fade = 1.0 - smoothstep(0.8, 1.0, d);
  float intensity = (circle + halo) * birth * fade;
  vec3 col = mix(v_color.rgb, vec3(1.0, 0.97, 0.92), circle * 0.2);
  o_color = vec4(col * intensity, 1.0);
}`;

const edge_vs = `#version 300 es
layout(location=0) in vec2 a_pos;
layout(location=1) in vec2 a_p0;
layout(location=2) in vec2 a_p1;
layout(location=3) in vec3 a_color;
layout(location=4) in float a_alpha;
uniform mat4 u_mvp;
out vec2 v_uv;
out vec3 v_color;
out float v_alpha;
void main() {
  v_uv = a_pos;
  v_color = a_color;
  v_alpha = a_alpha;
  vec2 d = a_p1 - a_p0;
  float len = length(d);
  vec2 dir = len > 0.001 ? d / len : vec2(1.0, 0.0);
  vec2 perp = vec2(-dir.y, dir.x);
  float gw = 18.0;
  vec2 center = (a_p0 + a_p1) * 0.5;
  vec2 world = center + d * 0.5 * a_pos.x + perp * gw * a_pos.y;
  gl_Position = u_mvp * vec4(world, 0.0, 1.0);
}`;

const edge_fs = `#version 300 es
precision highp float;
in vec2 v_uv;
in vec3 v_color;
in float v_alpha;
out vec4 o_color;
void main() {
  float d = abs(v_uv.y);
  float line = smoothstep(0.18, 0.04, d);
  float glow = exp(-d * 2.5) * 0.25;
  float intensity = (line + glow) * v_alpha;
  o_color = vec4(v_color * intensity, 1.0);
}`;

const blur_fs = `#version 300 es
precision highp float;
in vec2 v_uv;
uniform sampler2D u_tex;
uniform vec2 u_dir;
out vec4 o_color;
void main() {
  vec4 sum = vec4(0.0);
  float w[5] = float[5](0.227, 0.1945, 0.1216, 0.0541, 0.0162);
  sum += texture(u_tex, v_uv) * w[0];
  for (int i = 1; i < 5; i++) {
    vec2 off = u_dir * float(i);
    sum += texture(u_tex, v_uv + off) * w[i];
    sum += texture(u_tex, v_uv - off) * w[i];
  }
  o_color = sum;
}`;

const blit_fs = `#version 300 es
precision highp float;
in vec2 v_uv;
uniform sampler2D u_tex;
uniform float u_intensity;
out vec4 o_color;
void main() {
  o_color = texture(u_tex, v_uv) * u_intensity;
}`;

if (typeof module !== 'undefined' && module.exports) {
  module.exports = { bg_vs, bg_fs, node_vs, node_fs, edge_vs, edge_fs, blur_fs, blit_fs };
}

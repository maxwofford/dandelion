// gloam-gl: headless OpenGL renderer for gloam
// macOS: CGL + OpenGL 3.2 core (translates GLES 300 es shaders to 330 core)
// linux: EGL + GLES 3.0 (native)
//
// build:
//   macOS:  cc -shared -o lib/gloam-gl.dylib lib/gloam-gl.c -framework OpenGL
//   linux:  cc -shared -o lib/gloam-gl.so lib/gloam-gl.c -lEGL -lGLESv2

#ifdef __APPLE__
  #define GL_SILENCE_DEPRECATION
  #include <OpenGL/OpenGL.h>
  #include <OpenGL/gl3.h>
  #define GLOAM_MACOS 1
#else
  #include <EGL/egl.h>
  #include <GLES3/gl3.h>
  #include <fcntl.h>
  #define GLOAM_LINUX 1
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- state ---
static int g_width, g_height;
#ifdef GLOAM_MACOS
static CGLContextObj g_ctx;
#else
static EGLDisplay g_dpy;
static EGLContext g_ctx;
static EGLSurface g_srf;
#endif

static GLuint g_bg_prog, g_node_prog, g_edge_prog, g_blur_prog, g_blit_prog;
static GLuint g_quad_vao, g_quad_buf;
static GLuint g_node_vao, g_node_inst_buf;
static GLuint g_edge_vao, g_edge_inst_buf;

typedef struct { GLuint fbo, tex; int w, h; } FBO;
static FBO g_scene, g_blur1, g_blur2;

// PBO ring for async readback
static GLuint g_pbo[2];
static int g_pbo_idx = 0;       // which PBO to start reading into next
static int g_pbo_ready = -1;    // which PBO has data ready to map (-1 = none)
static GLsync g_fence = NULL;   // fence for async readback completion

// --- shader compilation ---

// translate GLES 300 es shader to GL 330 core (macOS)
static char* translate_shader(const char* src) {
#ifdef GLOAM_MACOS
  // replace "#version 300 es" with "#version 330 core"
  // remove "precision highp float;" lines
  const char* old_ver = "#version 300 es";
  const char* new_ver = "#version 330 core";

  size_t len = strlen(src) + 256;
  char* out = malloc(len);
  const char* p = src;
  char* o = out;

  // version line
  if (strncmp(p, old_ver, strlen(old_ver)) == 0) {
    o += sprintf(o, "%s", new_ver);
    p += strlen(old_ver);
  }

  // copy rest, skipping precision qualifiers
  while (*p) {
    if (strncmp(p, "precision highp float;", 22) == 0) { p += 22; continue; }
    if (strncmp(p, "precision mediump float;", 24) == 0) { p += 24; continue; }
    *o++ = *p++;
  }
  *o = '\0';
  return out;
#else
  return strdup(src);
#endif
}

static GLuint compile_shader(GLenum type, const char* src) {
  char* translated = translate_shader(src);
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, (const char**)&translated, NULL);
  glCompileShader(s);
  free(translated);

  GLint ok;
  glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetShaderInfoLog(s, 512, NULL, log);
    fprintf(stderr, "gloam-gl: shader error: %s\n", log);
    return 0;
  }
  return s;
}

static GLuint link_program(const char* vs_src, const char* fs_src) {
  GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
  GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
  if (!vs || !fs) return 0;

  GLuint p = glCreateProgram();
  glAttachShader(p, vs);
  glAttachShader(p, fs);
  glLinkProgram(p);

  GLint ok;
  glGetProgramiv(p, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[512];
    glGetProgramInfoLog(p, 512, NULL, log);
    fprintf(stderr, "gloam-gl: link error: %s\n", log);
    return 0;
  }
  glDeleteShader(vs);
  glDeleteShader(fs);
  return p;
}

// --- FBO ---
static FBO make_fbo(int w, int h) {
  FBO f = { 0, 0, w, h };
  glGenTextures(1, &f.tex);
  glBindTexture(GL_TEXTURE_2D, f.tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glGenFramebuffers(1, &f.fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, f.fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f.tex, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  return f;
}

// --- setup VAOs ---
static void setup_vaos(void) {
  float quad[] = { -1,-1, 1,-1, -1,1, 1,1 };

  // bg/blit/blur quad
  glGenVertexArrays(1, &g_quad_vao);
  glBindVertexArray(g_quad_vao);
  glGenBuffers(1, &g_quad_buf);
  glBindBuffer(GL_ARRAY_BUFFER, g_quad_buf);
  glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

  // node VAO
  glGenVertexArrays(1, &g_node_vao);
  glBindVertexArray(g_node_vao);
  GLuint nq;
  glGenBuffers(1, &nq);
  glBindBuffer(GL_ARRAY_BUFFER, nq);
  glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

  glGenBuffers(1, &g_node_inst_buf);
  glBindBuffer(GL_ARRAY_BUFFER, g_node_inst_buf);
  // stride: offset.xy(8) + radius(4) + color.rgba(16) + age(4) = 32
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 32, (void*)0);
  glVertexAttribDivisor(1, 1);
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 32, (void*)8);
  glVertexAttribDivisor(2, 1);
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, 32, (void*)12);
  glVertexAttribDivisor(3, 1);
  glEnableVertexAttribArray(4);
  glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, 32, (void*)28);
  glVertexAttribDivisor(4, 1);

  // edge VAO
  glGenVertexArrays(1, &g_edge_vao);
  glBindVertexArray(g_edge_vao);
  GLuint eq;
  glGenBuffers(1, &eq);
  glBindBuffer(GL_ARRAY_BUFFER, eq);
  glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);

  glGenBuffers(1, &g_edge_inst_buf);
  glBindBuffer(GL_ARRAY_BUFFER, g_edge_inst_buf);
  // stride: p0.xy(8) + p1.xy(8) + color.rgb(12) + alpha(4) = 32
  glEnableVertexAttribArray(1);
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 32, (void*)0);
  glVertexAttribDivisor(1, 1);
  glEnableVertexAttribArray(2);
  glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 32, (void*)8);
  glVertexAttribDivisor(2, 1);
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, 32, (void*)16);
  glVertexAttribDivisor(3, 1);
  glEnableVertexAttribArray(4);
  glVertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, 32, (void*)28);
  glVertexAttribDivisor(4, 1);

  glBindVertexArray(0);
}

// ═══════════════════════════════════════════
// PUBLIC API (called via Bun FFI)
// ═══════════════════════════════════════════

int gl_init(int width, int height) {
  g_width = width;
  g_height = height;

#ifdef GLOAM_MACOS
  CGLPixelFormatAttribute attrs[] = {
    kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
    kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
    kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8,
    kCGLPFAAccelerated,
    (CGLPixelFormatAttribute)0
  };
  CGLPixelFormatObj pix;
  GLint npix;
  CGLError err = CGLChoosePixelFormat(attrs, &pix, &npix);
  if (err) { fprintf(stderr, "gloam-gl: CGLChoosePixelFormat failed: %d\n", err); return -1; }
  err = CGLCreateContext(pix, NULL, &g_ctx);
  CGLDestroyPixelFormat(pix);
  if (err) { fprintf(stderr, "gloam-gl: CGLCreateContext failed: %d\n", err); return -1; }
  CGLSetCurrentContext(g_ctx);
#else
  g_dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (g_dpy == EGL_NO_DISPLAY) { fprintf(stderr, "gloam-gl: no EGL display\n"); return -1; }
  eglInitialize(g_dpy, NULL, NULL);
  eglBindAPI(EGL_OPENGL_ES_API);

  EGLint cfg_attrs[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE };
  EGLConfig cfg;
  EGLint ncfg;
  eglChooseConfig(g_dpy, cfg_attrs, &cfg, 1, &ncfg);

  EGLint ctx_attrs[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE };
  g_ctx = eglCreateContext(g_dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);

  EGLint srf_attrs[] = { EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE };
  g_srf = eglCreatePbufferSurface(g_dpy, cfg, srf_attrs);
  eglMakeCurrent(g_dpy, g_srf, g_srf, g_ctx);
#endif

  fprintf(stderr, "gloam-gl: initialized %dx%d — %s\n", width, height, glGetString(GL_VERSION));

  setup_vaos();
  g_scene = make_fbo(width, height);
  int bw = width / 4, bh = height / 4;
  g_blur1 = make_fbo(bw, bh);
  g_blur2 = make_fbo(bw, bh);

  // PBOs for async pixel readback
  glGenBuffers(2, g_pbo);
  size_t pbo_size = width * height * 4;
  for (int i = 0; i < 2; i++) {
    glBindBuffer(GL_PIXEL_PACK_BUFFER, g_pbo[i]);
    glBufferData(GL_PIXEL_PACK_BUFFER, pbo_size, NULL, GL_STREAM_READ);
  }
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  g_pbo_idx = 0;
  g_pbo_ready = -1;

  return 0;
}

int gl_compile_shaders(
  const char* bg_vs, const char* bg_fs,
  const char* node_vs, const char* node_fs,
  const char* edge_vs, const char* edge_fs,
  const char* blur_fs, const char* blit_fs
) {
  g_bg_prog   = link_program(bg_vs, bg_fs);
  g_node_prog = link_program(node_vs, node_fs);
  g_edge_prog = link_program(edge_vs, edge_fs);
  g_blur_prog = link_program(bg_vs, blur_fs);  // reuses bg vertex shader
  g_blit_prog = link_program(bg_vs, blit_fs);
  if (!g_bg_prog || !g_node_prog || !g_edge_prog || !g_blur_prog || !g_blit_prog) return -1;
  return 0;
}

void gl_render_frame(
  const float* node_data, int node_count,
  const float* edge_data, int edge_count,
  const float* mvp
) {
  int w = g_width, h = g_height;

  // pass 1: scene → FBO
  glBindFramebuffer(GL_FRAMEBUFFER, g_scene.fbo);
  glViewport(0, 0, w, h);
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);

  // background
  glDisable(GL_BLEND);
  glUseProgram(g_bg_prog);
  glBindVertexArray(g_quad_vao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  if (node_count > 0 || edge_count > 0) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE);

    // edges
    if (edge_count > 0) {
      glUseProgram(g_edge_prog);
      glUniformMatrix4fv(glGetUniformLocation(g_edge_prog, "u_mvp"), 1, GL_FALSE, mvp);
      glBindBuffer(GL_ARRAY_BUFFER, g_edge_inst_buf);
      glBufferData(GL_ARRAY_BUFFER, edge_count * 32, edge_data, GL_DYNAMIC_DRAW);
      glBindVertexArray(g_edge_vao);
      glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, edge_count);
    }

    // nodes
    if (node_count > 0) {
      glUseProgram(g_node_prog);
      glUniformMatrix4fv(glGetUniformLocation(g_node_prog, "u_mvp"), 1, GL_FALSE, mvp);
      glBindBuffer(GL_ARRAY_BUFFER, g_node_inst_buf);
      glBufferData(GL_ARRAY_BUFFER, node_count * 32, node_data, GL_DYNAMIC_DRAW);
      glBindVertexArray(g_node_vao);
      glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, node_count);
    }
  }

  // pass 2: downsample to bloom
  int bw = w / 4, bh = h / 4;
  glBindFramebuffer(GL_FRAMEBUFFER, g_blur1.fbo);
  glViewport(0, 0, bw, bh);
  glDisable(GL_BLEND);
  glUseProgram(g_blit_prog);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, g_scene.tex);
  glUniform1i(glGetUniformLocation(g_blit_prog, "u_tex"), 0);
  glUniform1f(glGetUniformLocation(g_blit_prog, "u_intensity"), 1.0f);
  glBindVertexArray(g_quad_vao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  // pass 3: horizontal blur
  glBindFramebuffer(GL_FRAMEBUFFER, g_blur2.fbo);
  glUseProgram(g_blur_prog);
  glBindTexture(GL_TEXTURE_2D, g_blur1.tex);
  glUniform1i(glGetUniformLocation(g_blur_prog, "u_tex"), 0);
  glUniform2f(glGetUniformLocation(g_blur_prog, "u_dir"), 1.0f / bw, 0);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  // pass 4: vertical blur
  glBindFramebuffer(GL_FRAMEBUFFER, g_blur1.fbo);
  glBindTexture(GL_TEXTURE_2D, g_blur2.tex);
  glUniform2f(glGetUniformLocation(g_blur_prog, "u_dir"), 0, 1.0f / bh);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  // pass 5: composite to scene FBO (we read from scene FBO, not backbuffer)
  // rebind scene FBO as output — but we also read from it, so use a temp
  // actually: blit scene to backbuffer-like target, then composite bloom
  // for headless: render final composite into scene FBO itself
  // trick: copy scene tex to blur2 first, then composite back into scene
  glBindFramebuffer(GL_FRAMEBUFFER, g_blur2.fbo);
  glViewport(0, 0, bw, bh);
  glUseProgram(g_blit_prog);
  glBindTexture(GL_TEXTURE_2D, g_scene.tex);
  glUniform1f(glGetUniformLocation(g_blit_prog, "u_intensity"), 1.0f);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

  // final composite: scene + bloom → scene FBO
  glBindFramebuffer(GL_FRAMEBUFFER, g_scene.fbo);
  glViewport(0, 0, w, h);
  glDisable(GL_BLEND);
  // re-blit scene (it's already there, so just add bloom on top)
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE);
  glUseProgram(g_blit_prog);
  glBindTexture(GL_TEXTURE_2D, g_blur1.tex);
  glUniform1f(glGetUniformLocation(g_blit_prog, "u_intensity"), 0.25f);
  glBindVertexArray(g_quad_vao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

}

// synchronous read (for single PNG export)
void gl_read_pixels(unsigned char* buffer) {
  glBindFramebuffer(GL_FRAMEBUFFER, g_scene.fbo);
  glReadPixels(0, 0, g_width, g_height, GL_RGBA, GL_UNSIGNED_BYTE, buffer);
}

// async read step 1: start DMA from scene FBO into PBO 0 (non-blocking)
void gl_read_pixels_start(void) {
  glBindFramebuffer(GL_FRAMEBUFFER, g_scene.fbo);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, g_pbo[0]);
  glReadPixels(0, 0, g_width, g_height, GL_RGBA, GL_UNSIGNED_BYTE, 0);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  if (g_fence) glDeleteSync(g_fence);
  g_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
  glFlush(); // ensure commands are submitted to GPU
  g_pbo_ready = 0;
}

// async read step 2: wait for fence, map PBO, copy to buffer
int gl_read_pixels_finish(unsigned char* buffer) {
  if (g_pbo_ready < 0) return -1;
  if (g_fence) {
    GLenum result = glClientWaitSync(g_fence, GL_SYNC_FLUSH_COMMANDS_BIT, 500000000); // 500ms timeout
    glDeleteSync(g_fence);
    g_fence = NULL;
    if (result == GL_WAIT_FAILED) return -1;
  }
  glBindBuffer(GL_PIXEL_PACK_BUFFER, g_pbo[0]);
  void* mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, g_width * g_height * 4, GL_MAP_READ_BIT);
  if (!mapped) {
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    return -1;
  }
  memcpy(buffer, mapped, g_width * g_height * 4);
  glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
  glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
  g_pbo_ready = -1;
  return 0;
}

void gl_destroy(void) {
#ifdef GLOAM_MACOS
  CGLSetCurrentContext(NULL);
  CGLDestroyContext(g_ctx);
#else
  eglMakeCurrent(g_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroySurface(g_dpy, g_srf);
  eglDestroyContext(g_dpy, g_ctx);
  eglTerminate(g_dpy);
#endif
}

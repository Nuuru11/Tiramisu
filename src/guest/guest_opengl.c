// tiramisu/src/guest/guest_opengl.c
// PE64 guest exercising OpenGL 1.x fixed-function, GLSL 330 core VBO/VAO/shaders,
// WGL extension lookup, and gdi32!SwapBuffers over Tiramisu's opengl32 bridge.
typedef unsigned int u32;
typedef int i32;
typedef unsigned char u8;
typedef unsigned long long u64;
typedef long long i64;

#ifdef _WIN32
#define GSTD __stdcall
#else
#define GSTD
#endif

int _fltused = 1;

__attribute__((dllimport)) void* GSTD wglCreateContext(void* hdc);
__attribute__((dllimport)) int GSTD wglDeleteContext(void* hglrc);
__attribute__((dllimport)) int GSTD wglMakeCurrent(void* hdc, void* hglrc);
__attribute__((dllimport)) void* GSTD wglGetCurrentContext(void);
__attribute__((dllimport)) void* GSTD wglGetProcAddress(const char* name);
__attribute__((dllimport)) void GSTD glClearColor(float r, float g, float b, float a);
__attribute__((dllimport)) void GSTD glClear(u32 mask);
__attribute__((dllimport)) void GSTD glViewport(int x, int y, int w, int h);
__attribute__((dllimport)) void GSTD glMatrixMode(u32 mode);
__attribute__((dllimport)) void GSTD glLoadIdentity(void);
__attribute__((dllimport)) void GSTD glOrtho(double l, double r, double b, double t, double n, double f);
__attribute__((dllimport)) void GSTD glBegin(u32 mode);
__attribute__((dllimport)) void GSTD glEnd(void);
__attribute__((dllimport)) void GSTD glVertex2f(float x, float y);
__attribute__((dllimport)) void GSTD glColor4f(float r, float g, float b, float a);
__attribute__((dllimport)) void GSTD glTexCoord2f(float s, float t);
__attribute__((dllimport)) void GSTD glGenTextures(int n, u32* textures);
__attribute__((dllimport)) void GSTD glBindTexture(u32 target, u32 tex);
__attribute__((dllimport)) void GSTD glTexImage2D(u32 target, int lvl, int intfmt, int w, int h, int b, u32 fmt, u32 type, const void* px);
__attribute__((dllimport)) void GSTD glDrawArrays(u32 mode, int first, int count);
__attribute__((dllimport)) u32 GSTD glGetError(void);
__attribute__((dllimport)) const u8* GSTD glGetString(u32 name);
__attribute__((dllimport)) int GSTD SwapBuffers(void* hdc);

static int cstr_has(const u8* s, const char* sub) {
  if (!s || !sub) return 0;
  for (u32 i = 0; s[i]; ++i) {
    u32 j = 0;
    while (sub[j] && s[i + j] == (u8)sub[j]) ++j;
    if (!sub[j]) return 1;
  }
  return 0;
}

u32 GSTD guest_opengl_main(u64 a, u64 b, u64 c) {
  (void)a; (void)b; (void)c;
  u32 bits = 0;

  void* hdc = (void*)0x00401000ull;
  void* ctx = wglCreateContext(hdc);
  if (ctx && wglMakeCurrent(hdc, ctx) && wglGetCurrentContext() == ctx) {
    bits |= 0x01u;
  }

  const u8* ver = glGetString(0x1F02u /*GL_VERSION*/);
  if (cstr_has(ver, "Tiramisu")) bits |= 0x02u;

  // Fixed-function matrix + immediate quad
  glViewport(0, 0, 1280, 720);
  glMatrixMode(0x1701u); // GL_PROJECTION
  glLoadIdentity();
  glOrtho(0.0, 1280.0, 720.0, 0.0, -1.0, 1.0);
  glClearColor(0.1f, 0.2f, 0.3f, 1.0f);
  glClear(0x4100u); // COLOR | DEPTH

  u32 tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(0x0DE1u, tex);
  u32 px[4] = {0xFFFFFFFFu, 0xFF0000FFu, 0xFF00FF00u, 0xFFFF0000u};
  glTexImage2D(0x0DE1u, 0, 0x8058u, 2, 2, 0, 0x1908u, 0x1401u, px);

  glBegin(0x0007u); // GL_QUADS
  glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
  glTexCoord2f(0.0f, 0.0f); glVertex2f(10.0f, 10.0f);
  glTexCoord2f(1.0f, 0.0f); glVertex2f(200.0f, 10.0f);
  glTexCoord2f(1.0f, 1.0f); glVertex2f(200.0f, 200.0f);
  glTexCoord2f(0.0f, 1.0f); glVertex2f(10.0f, 200.0f);
  glEnd();
  if (tex) bits |= 0x04u;

  // Core GLSL 330 path via wglGetProcAddress
  typedef u32 (GSTD *pfnCreateShader)(u32);
  typedef void (GSTD *pfnShaderSource)(u32, int, const char* const*, const int*);
  typedef void (GSTD *pfnCompileShader)(u32);
  typedef void (GSTD *pfnGetShaderiv)(u32, u32, int*);
  typedef u32 (GSTD *pfnCreateProgram)(void);
  typedef void (GSTD *pfnAttachShader)(u32, u32);
  typedef void (GSTD *pfnLinkProgram)(u32);
  typedef void (GSTD *pfnGetProgramiv)(u32, u32, int*);
  typedef void (GSTD *pfnUseProgram)(u32);
  typedef int (GSTD *pfnGetUniformLocation)(u32, const char*);
  typedef void (GSTD *pfnUniform4f)(int, float, float, float, float);
  typedef void (GSTD *pfnGenBuf)(int, u32*);
  typedef void (GSTD *pfnBindBuf)(u32, u32);
  typedef void (GSTD *pfnBindVAO)(u32);
  typedef void (GSTD *pfnBufData)(u32, i64, const void*, u32);
  typedef void (GSTD *pfnEnableAttr)(u32);
  typedef void (GSTD *pfnAttrPtr)(u32, int, u32, u8, int, const void*);

  pfnCreateShader fnCreateShader = (pfnCreateShader)wglGetProcAddress("glCreateShader");
  pfnShaderSource fnShaderSource = (pfnShaderSource)wglGetProcAddress("glShaderSource");
  pfnCompileShader fnCompileShader = (pfnCompileShader)wglGetProcAddress("glCompileShader");
  pfnGetShaderiv fnGetShaderiv = (pfnGetShaderiv)wglGetProcAddress("glGetShaderiv");
  pfnCreateProgram fnCreateProgram = (pfnCreateProgram)wglGetProcAddress("glCreateProgram");
  pfnAttachShader fnAttachShader = (pfnAttachShader)wglGetProcAddress("glAttachShader");
  pfnLinkProgram fnLinkProgram = (pfnLinkProgram)wglGetProcAddress("glLinkProgram");
  pfnGetProgramiv fnGetProgramiv = (pfnGetProgramiv)wglGetProcAddress("glGetProgramiv");
  pfnUseProgram fnUseProgram = (pfnUseProgram)wglGetProcAddress("glUseProgram");
  pfnGetUniformLocation fnGetUniform = (pfnGetUniformLocation)wglGetProcAddress("glGetUniformLocation");
  pfnUniform4f fnUniform4f = (pfnUniform4f)wglGetProcAddress("glUniform4f");
  pfnGenBuf fnGenBuffers = (pfnGenBuf)wglGetProcAddress("glGenBuffers");
  pfnBindBuf fnBindBuffer = (pfnBindBuf)wglGetProcAddress("glBindBuffer");
  pfnBufData fnBufferData = (pfnBufData)wglGetProcAddress("glBufferData");
  pfnGenBuf fnGenVAOs = (pfnGenBuf)wglGetProcAddress("glGenVertexArrays");
  pfnBindVAO fnBindVAO = (pfnBindVAO)wglGetProcAddress("glBindVertexArray");
  pfnEnableAttr fnEnableAttr = (pfnEnableAttr)wglGetProcAddress("glEnableVertexAttribArray");
  pfnAttrPtr fnAttrPtr = (pfnAttrPtr)wglGetProcAddress("glVertexAttribPointer");

  if (fnCreateShader && fnShaderSource && fnCompileShader && fnCreateProgram &&
      fnAttachShader && fnLinkProgram && fnUseProgram && fnGenBuffers && fnGenVAOs) {
    const char* vs_src =
        "#version 330 core\n"
        "layout(location=0) in vec3 aPos;\n"
        "out vec2 vUV;\n"
        "void main() { vUV = aPos.xy * 0.5 + 0.5; gl_Position = vec4(aPos, 1.0); }\n";
    const char* ps_src =
        "#version 330 core\n"
        "in vec2 vUV;\n"
        "uniform vec4 uTint;\n"
        "out vec4 FragColor;\n"
        "void main() { FragColor = uTint * vec4(vUV, 1.0, 1.0); }\n";
    u32 vs = fnCreateShader(0x8B31u);
    fnShaderSource(vs, 1, &vs_src, 0);
    fnCompileShader(vs);
    int ok_vs = 0;
    fnGetShaderiv(vs, 0x8B81u, &ok_vs);

    u32 ps = fnCreateShader(0x8B30u);
    fnShaderSource(ps, 1, &ps_src, 0);
    fnCompileShader(ps);
    int ok_ps = 0;
    fnGetShaderiv(ps, 0x8B81u, &ok_ps);

    u32 prog = fnCreateProgram();
    fnAttachShader(prog, vs);
    fnAttachShader(prog, ps);
    fnLinkProgram(prog);
    int linked = 0;
    fnGetProgramiv(prog, 0x8B82u, &linked);
    if (ok_vs && ok_ps && linked) bits |= 0x08u;

    fnUseProgram(prog);
    int u_tint = fnGetUniform(prog, "uTint");
    if (u_tint >= 0) {
      fnUniform4f(u_tint, 1.0f, 0.5f, 0.25f, 1.0f);
      bits |= 0x10u;
    }

    u32 vao = 0, vbo = 0;
    fnGenVAOs(1, &vao);
    fnBindVAO(vao);
    fnGenBuffers(1, &vbo);
    fnBindBuffer(0x8892u, vbo);
    float verts[9] = {0.0f, 0.5f, 0.0f, -0.5f, -0.5f, 0.0f, 0.5f, -0.5f, 0.0f};
    fnBufferData(0x8892u, sizeof(verts), verts, 0x88E4u);
    fnEnableAttr(0);
    fnAttrPtr(0, 3, 0x1406u, 0, 12, 0);
    glDrawArrays(0x0004u, 0, 3);
    bits |= 0x20u;
  }

  if (SwapBuffers(hdc)) bits |= 0x40u;
  if (glGetError() == 0) bits |= 0x80u;
  wglMakeCurrent(0, 0);
  wglDeleteContext(ctx);
  return bits;
}

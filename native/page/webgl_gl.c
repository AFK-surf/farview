/* Farview Wave G1: Real GPU WebGL 1.0 & WebGL 2.0 native OpenGL 4.1 Core backend.
 *
 * Platform linking red line:
 *   - macOS (__APPLE__): zero external deps; uses __asm__(".linker_option \"-framework\", \"OpenGL\"\n")
 *     and CGL OpenGL 4.1 Core Profile (Metal-backed on Apple Silicon).
 *   - Linux (!__APPLE__): zero hard link to -lGL/-lEGL; dynamically loads libEGL.so.1 + libGL.so.1
 *     (or libOSMesa.so.8) via dlopen/dlsym, degrading gracefully when no GL driver is installed.
 */

#include "webgl_gl.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifdef __APPLE__
#define GL_SILENCE_DEPRECATION
__asm__(".linker_option \"-framework\", \"OpenGL\"\n");
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#else
/* Linux / Windows dynamic loader (no -lGL / -lEGL link-time dependency) */
#ifdef _WIN32
#include "../win_compat.h"
#else
#include <dlfcn.h>
#endif
typedef unsigned int GLenum;
typedef unsigned char GLboolean;
typedef unsigned int GLbitfield;
typedef signed char GLbyte;
typedef short GLshort;
typedef int GLint;
typedef int GLsizei;
typedef unsigned char GLubyte;
typedef unsigned short GLushort;
typedef unsigned int GLuint;
typedef float GLfloat;
typedef double GLdouble;
typedef ptrdiff_t GLintptr;
typedef ptrdiff_t GLsizeiptr;
typedef char GLchar;

#define GL_FALSE 0
#define GL_TRUE  1
#define GL_NO_ERROR 0
#define GL_INVALID_ENUM 0x0500
#define GL_INVALID_VALUE 0x0501
#define GL_INVALID_OPERATION 0x0502
#define GL_OUT_OF_MEMORY 0x0505
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_SHORT 0x1403
#define GL_UNSIGNED_INT 0x1405
#define GL_FLOAT 0x1406
#define GL_HALF_FLOAT 0x140B
#define GL_DEPTH_COMPONENT 0x1902
#define GL_RED 0x1903
#define GL_RGB 0x1907
#define GL_RGBA 0x1908
#define GL_RG 0x8227
#define GL_R8 0x8229
#define GL_RG8 0x822B
#define GL_RGB8 0x8051
#define GL_RGBA8 0x8058
#define GL_SRGB 0x8C40
#define GL_SRGB8 0x8C41
#define GL_SRGB8_ALPHA8 0x8C43
#define GL_RGBA32F 0x8814
#define GL_RGB32F 0x8815
#define GL_RGBA16F 0x881A
#define GL_RGB16F 0x881B
#define GL_DEPTH_COMPONENT16 0x81A5
#define GL_DEPTH_COMPONENT24 0x81A6
#define GL_DEPTH_STENCIL 0x84F9
#define GL_DEPTH24_STENCIL8 0x88F0
#define GL_STENCIL_INDEX8 0x8D48
#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_TEXTURE_WRAP_S 0x2802
#define GL_TEXTURE_WRAP_T 0x2803
#define GL_TEXTURE_BASE_LEVEL 0x813C
#define GL_TEXTURE_MAX_LEVEL 0x813D
#define GL_LINEAR 0x2601
#define GL_CLAMP_TO_EDGE 0x812F
#define GL_FRAMEBUFFER 0x8D40
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#define GL_RENDERBUFFER 0x8D41
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_DEPTH_STENCIL_ATTACHMENT 0x821A
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_VERTEX_ARRAY_BINDING 0x85B5
#define GL_UNPACK_ALIGNMENT 0x0CF5
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_PROGRAM_POINT_SIZE 0x8642
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_DEPTH_BUFFER_BIT 0x0100
#define GL_STENCIL_BUFFER_BIT 0x0400
#define GL_INFO_LOG_LENGTH 0x8B84
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_VIEWPORT 0x0BA2
#define GL_SCISSOR_BOX 0x0C10
#define GL_COLOR_CLEAR_VALUE 0x0C22
#define GL_MAX_VIEWPORT_DIMS 0x0D3A
#define GL_ALIASED_LINE_WIDTH_RANGE 0x846E
#define GL_ALIASED_POINT_SIZE_RANGE 0x846D
#define GL_DEPTH_RANGE 0x0B70
#define GL_BLEND_COLOR 0x8005
#define GL_COLOR_WRITEMASK 0x0C23
#define GL_PIXEL_PACK_BUFFER 0x88EB
#define GL_STREAM_READ 0x88E1
#define GL_MAP_READ_BIT 0x0001
#endif

/* WebGL-specific enums not present in desktop GL 4.1 Core headers */
#define FV_GL_LUMINANCE 0x1909
#define FV_GL_LUMINANCE_ALPHA 0x190A
#define FV_GL_ALPHA 0x1906
#define FV_GL_RGBA4 0x8056
#define FV_GL_RGB5_A1 0x8057
#define FV_GL_RGB565 0x8D62
#define FV_GL_HALF_FLOAT_OES 0x8D61
#define FV_GL_UNPACK_FLIP_Y_WEBGL 0x9240
#define FV_GL_UNPACK_PREMULTIPLY_ALPHA_WEBGL 0x9241
#define FV_GL_UNPACK_COLORSPACE_CONVERSION_WEBGL 0x9243
#define FV_GL_DEPTH_STENCIL_ATTACHMENT_WEBGL 0x821A
#define FV_GL_ALIASED_POINT_SIZE_RANGE 0x846D

typedef struct {
  int32_t in_use;
  int32_t is_webgl2;
  int32_t width;
  int32_t height;
  int32_t dirty;
#ifdef __APPLE__
  CGLContextObj cgl_ctx;
#elif defined(_WIN32)
  HWND wgl_hwnd;
  HDC wgl_hdc;
  HGLRC wgl_ctx;
#else
  void *egl_dpy;
  void *egl_ctx;
  void *egl_surf;
#endif
  GLuint default_fbo;
  GLuint color_tex;
  GLuint depth_stencil_rbo;
  GLuint default_vao;
  GLuint blit_fbo;
  GLuint blit_tex;
  int32_t blit_w;
  int32_t blit_h;
  GLuint pbo[2];
  int32_t pbo_w;
  int32_t pbo_h;
  int32_t pbo_idx;
  int32_t pbo_primed;
  GLuint bound_draw_fbo; /* 0 means default_fbo */
  GLuint bound_read_fbo; /* 0 means default_fbo */
  GLuint bound_vao;      /* 0 means default_vao */
  int32_t unpack_flip_y;
  int32_t unpack_premultiply_alpha;
  int32_t unpack_alignment;
  int32_t pack_alignment;
} FvWebGLCtx;

static FvWebGLCtx g_ctxs[FV_WEBGL_MAX_CONTEXTS];
static int32_t g_current_slot = -1;
static int32_t g_atexit_registered = 0;

static int32_t clamp_dim(int32_t d) {
  if (d < 1) return 1;
  if (d > FV_WEBGL_MAX_DIM) return FV_WEBGL_MAX_DIM;
  return d;
}

static FvWebGLCtx *get_ctx(int32_t ctx_id) {
  if (ctx_id < 1 || ctx_id > FV_WEBGL_MAX_CONTEXTS) return NULL;
  FvWebGLCtx *c = &g_ctxs[ctx_id - 1];
  if (!c->in_use) return NULL;
  return c;
}

#ifndef __APPLE__
/* Linux EGL + GL dynamic function table */
typedef void *EGLDisplay;
typedef void *EGLConfig;
typedef void *EGLContext;
typedef void *EGLSurface;
typedef int32_t EGLint;
typedef unsigned int EGLenum;
typedef unsigned int EGLBoolean;

#define EGL_DEFAULT_DISPLAY ((void *)0)
#define EGL_NO_DISPLAY ((void *)0)
#define EGL_NO_CONTEXT ((void *)0)
#define EGL_NO_SURFACE ((void *)0)
#define EGL_NONE 0x3038
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_BIT 0x0008
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_OPENGL_API 0x30A2
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#define EGL_CONTEXT_OPENGL_PROFILE_MASK 0x30FD
#define EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT 0x00000001
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD

static int g_linux_gl_inited = 0;
static int g_linux_gl_ok = 0;
static void *g_lib_egl = NULL;
static void *g_lib_gl = NULL;

static EGLDisplay (*p_eglGetDisplay)(void *) = NULL;
static EGLDisplay (*p_eglGetPlatformDisplay)(EGLenum, void *, const intptr_t *) = NULL;
static EGLBoolean (*p_eglInitialize)(EGLDisplay, EGLint *, EGLint *) = NULL;
static EGLBoolean (*p_eglBindAPI)(EGLenum) = NULL;
static EGLBoolean (*p_eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *) = NULL;
static EGLSurface (*p_eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *) = NULL;
static EGLContext (*p_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) = NULL;
static EGLBoolean (*p_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = NULL;
static EGLBoolean (*p_eglDestroyContext)(EGLDisplay, EGLContext) = NULL;
static EGLBoolean (*p_eglDestroySurface)(EGLDisplay, EGLSurface) = NULL;
static void *(*p_eglGetProcAddress)(const char *) = NULL;

#define FV_GL_FUNCS(X) \
  X(GLuint, glCreateShader, (GLenum)) \
  X(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
  X(void, glCompileShader, (GLuint)) \
  X(void, glGetShaderiv, (GLuint, GLenum, GLint *)) \
  X(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
  X(void, glDeleteShader, (GLuint)) \
  X(GLuint, glCreateProgram, (void)) \
  X(void, glAttachShader, (GLuint, GLuint)) \
  X(void, glDetachShader, (GLuint, GLuint)) \
  X(void, glLinkProgram, (GLuint)) \
  X(void, glValidateProgram, (GLuint)) \
  X(void, glGetProgramiv, (GLuint, GLenum, GLint *)) \
  X(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
  X(void, glUseProgram, (GLuint)) \
  X(void, glDeleteProgram, (GLuint)) \
  X(GLint, glGetAttribLocation, (GLuint, const GLchar *)) \
  X(void, glBindAttribLocation, (GLuint, GLuint, const GLchar *)) \
  X(GLint, glGetUniformLocation, (GLuint, const GLchar *)) \
  X(GLuint, glGetUniformBlockIndex, (GLuint, const GLchar *)) \
  X(void, glUniformBlockBinding, (GLuint, GLuint, GLuint)) \
  X(void, glBindBufferBase, (GLenum, GLuint, GLuint)) \
  X(void, glBindBufferRange, (GLenum, GLuint, GLuint, GLintptr, GLsizeiptr)) \
  X(void, glGetActiveAttrib, (GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *)) \
  X(void, glGetActiveUniform, (GLuint, GLuint, GLsizei, GLsizei *, GLint *, GLenum *, GLchar *)) \
  X(void, glGenBuffers, (GLsizei, GLuint *)) \
  X(void, glBindBuffer, (GLenum, GLuint)) \
  X(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum)) \
  X(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void *)) \
  X(void, glDeleteBuffers, (GLsizei, const GLuint *)) \
  X(void, glGenVertexArrays, (GLsizei, GLuint *)) \
  X(void, glBindVertexArray, (GLuint)) \
  X(void, glDeleteVertexArrays, (GLsizei, const GLuint *)) \
  X(void, glEnableVertexAttribArray, (GLuint)) \
  X(void, glDisableVertexAttribArray, (GLuint)) \
  X(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) \
  X(void, glVertexAttribIPointer, (GLuint, GLint, GLenum, GLsizei, const void *)) \
  X(void, glVertexAttribDivisor, (GLuint, GLuint)) \
  X(void, glVertexAttrib1fv, (GLuint, const GLfloat *)) \
  X(void, glVertexAttrib2fv, (GLuint, const GLfloat *)) \
  X(void, glVertexAttrib3fv, (GLuint, const GLfloat *)) \
  X(void, glVertexAttrib4fv, (GLuint, const GLfloat *)) \
  X(void, glUniform1fv, (GLint, GLsizei, const GLfloat *)) \
  X(void, glUniform2fv, (GLint, GLsizei, const GLfloat *)) \
  X(void, glUniform3fv, (GLint, GLsizei, const GLfloat *)) \
  X(void, glUniform4fv, (GLint, GLsizei, const GLfloat *)) \
  X(void, glUniform1iv, (GLint, GLsizei, const GLint *)) \
  X(void, glUniform2iv, (GLint, GLsizei, const GLint *)) \
  X(void, glUniform3iv, (GLint, GLsizei, const GLint *)) \
  X(void, glUniform4iv, (GLint, GLsizei, const GLint *)) \
  X(void, glUniform1uiv, (GLint, GLsizei, const GLuint *)) \
  X(void, glUniform2uiv, (GLint, GLsizei, const GLuint *)) \
  X(void, glUniform3uiv, (GLint, GLsizei, const GLuint *)) \
  X(void, glUniform4uiv, (GLint, GLsizei, const GLuint *)) \
  X(void, glUniformMatrix2fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
  X(void, glUniformMatrix3fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
  X(void, glUniformMatrix4fv, (GLint, GLsizei, GLboolean, const GLfloat *)) \
  X(void, glGenTextures, (GLsizei, GLuint *)) \
  X(void, glBindTexture, (GLenum, GLuint)) \
  X(void, glActiveTexture, (GLenum)) \
  X(void, glDeleteTextures, (GLsizei, const GLuint *)) \
  X(void, glPixelStorei, (GLenum, GLint)) \
  X(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
  X(void, glTexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *)) \
  X(void, glTexParameteri, (GLenum, GLenum, GLint)) \
  X(void, glTexParameterf, (GLenum, GLenum, GLfloat)) \
  X(void, glGenerateMipmap, (GLenum)) \
  X(void, glGenSamplers, (GLsizei, GLuint *)) \
  X(void, glDeleteSamplers, (GLsizei, const GLuint *)) \
  X(void, glBindSampler, (GLuint, GLuint)) \
  X(void, glSamplerParameteri, (GLuint, GLenum, GLint)) \
  X(void, glSamplerParameterf, (GLuint, GLenum, GLfloat)) \
  X(void, glClearBufferfv, (GLenum, GLint, const GLfloat *)) \
  X(void, glClearBufferiv, (GLenum, GLint, const GLint *)) \
  X(void, glClearBufferuiv, (GLenum, GLint, const GLuint *)) \
  X(void, glClearBufferfi, (GLenum, GLint, GLfloat, GLint)) \
  X(void, glGenFramebuffers, (GLsizei, GLuint *)) \
  X(void, glBindFramebuffer, (GLenum, GLuint)) \
  X(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) \
  X(void, glFramebufferRenderbuffer, (GLenum, GLenum, GLenum, GLuint)) \
  X(GLenum, glCheckFramebufferStatus, (GLenum)) \
  X(void, glDeleteFramebuffers, (GLsizei, const GLuint *)) \
  X(void, glDrawBuffers, (GLsizei, const GLenum *)) \
  X(void, glReadBuffer, (GLenum)) \
  X(void, glBlitFramebuffer, (GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)) \
  X(void, glGenRenderbuffers, (GLsizei, GLuint *)) \
  X(void, glBindRenderbuffer, (GLenum, GLuint)) \
  X(void, glRenderbufferStorage, (GLenum, GLenum, GLsizei, GLsizei)) \
  X(void, glRenderbufferStorageMultisample, (GLenum, GLsizei, GLenum, GLsizei, GLsizei)) \
  X(void, glDeleteRenderbuffers, (GLsizei, const GLuint *)) \
  X(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) \
  X(void, glScissor, (GLint, GLint, GLsizei, GLsizei)) \
  X(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
  X(void, glClearDepth, (GLdouble)) \
  X(void, glClearStencil, (GLint)) \
  X(void, glClear, (GLbitfield)) \
  X(void, glEnable, (GLenum)) \
  X(void, glDisable, (GLenum)) \
  X(GLboolean, glIsEnabled, (GLenum)) \
  X(void, glDepthFunc, (GLenum)) \
  X(void, glDepthMask, (GLboolean)) \
  X(void, glDepthRange, (GLdouble, GLdouble)) \
  X(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) \
  X(void, glCullFace, (GLenum)) \
  X(void, glFrontFace, (GLenum)) \
  X(void, glBlendFunc, (GLenum, GLenum)) \
  X(void, glBlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum)) \
  X(void, glBlendEquation, (GLenum)) \
  X(void, glBlendEquationSeparate, (GLenum, GLenum)) \
  X(void, glBlendColor, (GLfloat, GLfloat, GLfloat, GLfloat)) \
  X(void, glLineWidth, (GLfloat)) \
  X(void, glPolygonOffset, (GLfloat, GLfloat)) \
  X(void, glStencilFunc, (GLenum, GLint, GLuint)) \
  X(void, glStencilFuncSeparate, (GLenum, GLenum, GLint, GLuint)) \
  X(void, glStencilOp, (GLenum, GLenum, GLenum)) \
  X(void, glStencilOpSeparate, (GLenum, GLenum, GLenum, GLenum)) \
  X(void, glStencilMask, (GLuint)) \
  X(void, glStencilMaskSeparate, (GLenum, GLuint)) \
  X(void, glDrawArrays, (GLenum, GLint, GLsizei)) \
  X(void, glDrawElements, (GLenum, GLsizei, GLenum, const void *)) \
  X(void, glDrawArraysInstanced, (GLenum, GLint, GLsizei, GLsizei)) \
  X(void, glDrawElementsInstanced, (GLenum, GLsizei, GLenum, const void *, GLsizei)) \
  X(void, glFinish, (void)) \
  X(void, glFlush, (void)) \
  X(GLenum, glGetError, (void)) \
  X(const GLubyte *, glGetString, (GLenum)) \
  X(void, glGetIntegerv, (GLenum, GLint *)) \
  X(void, glGetFloatv, (GLenum, GLfloat *)) \
  X(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
  X(void *, glMapBufferRange, (GLenum, GLintptr, GLsizeiptr, GLbitfield)) \
  X(GLboolean, glUnmapBuffer, (GLenum))

#define DECL_GL_PTR(ret, name, args) static ret (*p_##name) args = NULL;
FV_GL_FUNCS(DECL_GL_PTR)
#undef DECL_GL_PTR

#define glCreateShader p_glCreateShader
#define glShaderSource p_glShaderSource
#define glCompileShader p_glCompileShader
#define glGetShaderiv p_glGetShaderiv
#define glGetShaderInfoLog p_glGetShaderInfoLog
#define glDeleteShader p_glDeleteShader
#define glCreateProgram p_glCreateProgram
#define glAttachShader p_glAttachShader
#define glDetachShader p_glDetachShader
#define glLinkProgram p_glLinkProgram
#define glValidateProgram p_glValidateProgram
#define glGetProgramiv p_glGetProgramiv
#define glGetProgramInfoLog p_glGetProgramInfoLog
#define glUseProgram p_glUseProgram
#define glDeleteProgram p_glDeleteProgram
#define glGetAttribLocation p_glGetAttribLocation
#define glBindAttribLocation p_glBindAttribLocation
#define glGetUniformLocation p_glGetUniformLocation
#define glGetUniformBlockIndex p_glGetUniformBlockIndex
#define glUniformBlockBinding p_glUniformBlockBinding
#define glBindBufferBase p_glBindBufferBase
#define glBindBufferRange p_glBindBufferRange
#define glGetActiveAttrib p_glGetActiveAttrib
#define glGetActiveUniform p_glGetActiveUniform
#define glGenBuffers p_glGenBuffers
#define glBindBuffer p_glBindBuffer
#define glBufferData p_glBufferData
#define glBufferSubData p_glBufferSubData
#define glDeleteBuffers p_glDeleteBuffers
#define glGenVertexArrays p_glGenVertexArrays
#define glBindVertexArray p_glBindVertexArray
#define glDeleteVertexArrays p_glDeleteVertexArrays
#define glEnableVertexAttribArray p_glEnableVertexAttribArray
#define glDisableVertexAttribArray p_glDisableVertexAttribArray
#define glVertexAttribPointer p_glVertexAttribPointer
#define glVertexAttribIPointer p_glVertexAttribIPointer
#define glVertexAttribDivisor p_glVertexAttribDivisor
#define glVertexAttrib1fv p_glVertexAttrib1fv
#define glVertexAttrib2fv p_glVertexAttrib2fv
#define glVertexAttrib3fv p_glVertexAttrib3fv
#define glVertexAttrib4fv p_glVertexAttrib4fv
#define glUniform1fv p_glUniform1fv
#define glUniform2fv p_glUniform2fv
#define glUniform3fv p_glUniform3fv
#define glUniform4fv p_glUniform4fv
#define glUniform1iv p_glUniform1iv
#define glUniform2iv p_glUniform2iv
#define glUniform3iv p_glUniform3iv
#define glUniform4iv p_glUniform4iv
#define glUniform1uiv p_glUniform1uiv
#define glUniform2uiv p_glUniform2uiv
#define glUniform3uiv p_glUniform3uiv
#define glUniform4uiv p_glUniform4uiv
#define glUniformMatrix2fv p_glUniformMatrix2fv
#define glUniformMatrix3fv p_glUniformMatrix3fv
#define glUniformMatrix4fv p_glUniformMatrix4fv
#define glGenTextures p_glGenTextures
#define glBindTexture p_glBindTexture
#define glActiveTexture p_glActiveTexture
#define glDeleteTextures p_glDeleteTextures
#define glPixelStorei p_glPixelStorei
#define glTexImage2D p_glTexImage2D
#define glTexSubImage2D p_glTexSubImage2D
#define glTexParameteri p_glTexParameteri
#define glTexParameterf p_glTexParameterf
#define glGenerateMipmap p_glGenerateMipmap
#define glGenSamplers p_glGenSamplers
#define glDeleteSamplers p_glDeleteSamplers
#define glBindSampler p_glBindSampler
#define glSamplerParameteri p_glSamplerParameteri
#define glSamplerParameterf p_glSamplerParameterf
#define glClearBufferfv p_glClearBufferfv
#define glClearBufferiv p_glClearBufferiv
#define glClearBufferuiv p_glClearBufferuiv
#define glClearBufferfi p_glClearBufferfi
#define glGenFramebuffers p_glGenFramebuffers
#define glBindFramebuffer p_glBindFramebuffer
#define glFramebufferTexture2D p_glFramebufferTexture2D
#define glFramebufferRenderbuffer p_glFramebufferRenderbuffer
#define glCheckFramebufferStatus p_glCheckFramebufferStatus
#define glDeleteFramebuffers p_glDeleteFramebuffers
#define glDrawBuffers p_glDrawBuffers
#define glReadBuffer p_glReadBuffer
#define glBlitFramebuffer p_glBlitFramebuffer
#define glGenRenderbuffers p_glGenRenderbuffers
#define glBindRenderbuffer p_glBindRenderbuffer
#define glRenderbufferStorage p_glRenderbufferStorage
#define glRenderbufferStorageMultisample p_glRenderbufferStorageMultisample
#define glDeleteRenderbuffers p_glDeleteRenderbuffers
#define glViewport p_glViewport
#define glScissor p_glScissor
#define glClearColor p_glClearColor
#define glClearDepth p_glClearDepth
#define glClearStencil p_glClearStencil
#define glClear p_glClear
#define glEnable p_glEnable
#define glDisable p_glDisable
#define glIsEnabled p_glIsEnabled
#define glDepthFunc p_glDepthFunc
#define glDepthMask p_glDepthMask
#define glDepthRange p_glDepthRange
#define glColorMask p_glColorMask
#define glCullFace p_glCullFace
#define glFrontFace p_glFrontFace
#define glBlendFunc p_glBlendFunc
#define glBlendFuncSeparate p_glBlendFuncSeparate
#define glBlendEquation p_glBlendEquation
#define glBlendEquationSeparate p_glBlendEquationSeparate
#define glBlendColor p_glBlendColor
#define glLineWidth p_glLineWidth
#define glPolygonOffset p_glPolygonOffset
#define glStencilFunc p_glStencilFunc
#define glStencilFuncSeparate p_glStencilFuncSeparate
#define glStencilOp p_glStencilOp
#define glStencilOpSeparate p_glStencilOpSeparate
#define glStencilMask p_glStencilMask
#define glStencilMaskSeparate p_glStencilMaskSeparate
#define glDrawArrays p_glDrawArrays
#define glDrawElements p_glDrawElements
#define glDrawArraysInstanced p_glDrawArraysInstanced
#define glDrawElementsInstanced p_glDrawElementsInstanced
#define glFinish p_glFinish
#define glFlush p_glFlush
#define glGetError p_glGetError
#define glGetString p_glGetString
#define glGetIntegerv p_glGetIntegerv
#define glGetFloatv p_glGetFloatv
#define glReadPixels p_glReadPixels
#define glMapBufferRange p_glMapBufferRange
#define glUnmapBuffer p_glUnmapBuffer

#ifdef _WIN32
#define WGL_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB 0x2092
#define WGL_CONTEXT_PROFILE_MASK_ARB 0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001

typedef HGLRC (WINAPI *PFNWGLCREATECONTEXTPROC)(HDC);
typedef BOOL (WINAPI *PFNWGLDELETECONTEXTPROC)(HGLRC);
typedef BOOL (WINAPI *PFNWGLMAKECURRENTPROC)(HDC, HGLRC);
typedef PROC (WINAPI *PFNWGLGETPROCADDRESSPROC)(LPCSTR);
typedef HGLRC (WINAPI *PFNWGLCREATECONTEXTATTRIBSARBPROC)(HDC, HGLRC, const int *);

static int g_win_gl_inited = 0;
static int g_win_gl_ok = 0;
static HMODULE g_win_gl_mod = NULL;
static PFNWGLCREATECONTEXTPROC p_wglCreateContext = NULL;
static PFNWGLDELETECONTEXTPROC p_wglDeleteContext = NULL;
static PFNWGLMAKECURRENTPROC p_wglMakeCurrent = NULL;
static PFNWGLGETPROCADDRESSPROC p_wglGetProcAddress = NULL;
static PFNWGLCREATECONTEXTATTRIBSARBPROC p_wglCreateContextAttribsARB = NULL;

static void *win_gl_sym(const char *name) {
  void *p = NULL;
  if (p_wglGetProcAddress) {
    p = (void *)p_wglGetProcAddress(name);
    if (p == (void *)1 || p == (void *)2 || p == (void *)3 || p == (void *)-1) p = NULL;
  }
  if (!p && g_win_gl_mod) p = (void *)GetProcAddress(g_win_gl_mod, name);
  return p;
}

static int win_wgl_create_surface_ctx(HWND *out_hwnd, HDC *out_hdc, HGLRC *out_ctx) {
  HWND hwnd = CreateWindowExA(0, "STATIC", "", WS_POPUP, 0, 0, 1, 1, NULL, NULL, NULL, NULL);
  if (!hwnd) return 0;
  HDC hdc = GetDC(hwnd);
  if (!hdc) {
    DestroyWindow(hwnd);
    return 0;
  }
  PIXELFORMATDESCRIPTOR pfd;
  memset(&pfd, 0, sizeof(pfd));
  pfd.nSize = sizeof(pfd);
  pfd.nVersion = 1;
  pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
  pfd.iPixelType = PFD_TYPE_RGBA;
  pfd.cColorBits = 32;
  pfd.cDepthBits = 24;
  pfd.cStencilBits = 8;
  pfd.iLayerType = PFD_MAIN_PLANE;
  int pf = ChoosePixelFormat(hdc, &pfd);
  if (!pf || !SetPixelFormat(hdc, pf, &pfd)) {
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    return 0;
  }
  HGLRC boot = p_wglCreateContext(hdc);
  if (!boot) {
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    return 0;
  }
  if (!p_wglMakeCurrent(hdc, boot)) {
    p_wglDeleteContext(boot);
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    return 0;
  }
  if (!p_wglCreateContextAttribsARB) {
    p_wglCreateContextAttribsARB =
        (PFNWGLCREATECONTEXTATTRIBSARBPROC)p_wglGetProcAddress("wglCreateContextAttribsARB");
  }
  HGLRC core = NULL;
  if (p_wglCreateContextAttribsARB) {
    const int attrs41[] = {
      WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
      WGL_CONTEXT_MINOR_VERSION_ARB, 1,
      WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
      0
    };
    core = p_wglCreateContextAttribsARB(hdc, NULL, attrs41);
    if (!core) {
      const int attrs33[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 3,
        WGL_CONTEXT_MINOR_VERSION_ARB, 3,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
        0
      };
      core = p_wglCreateContextAttribsARB(hdc, NULL, attrs33);
    }
  }
  HGLRC final_ctx = boot;
  if (core) {
    if (p_wglMakeCurrent(hdc, core)) {
      p_wglDeleteContext(boot);
      final_ctx = core;
    } else {
      p_wglDeleteContext(core);
    }
  }
  *out_hwnd = hwnd;
  *out_hdc = hdc;
  *out_ctx = final_ctx;
  return 1;
}

static int win_gl_init(void) {
  if (g_win_gl_inited) return g_win_gl_ok;
  g_win_gl_inited = 1;
  g_win_gl_mod = LoadLibraryA("opengl32.dll");
  if (!g_win_gl_mod) return 0;
  p_wglCreateContext = (PFNWGLCREATECONTEXTPROC)GetProcAddress(g_win_gl_mod, "wglCreateContext");
  p_wglDeleteContext = (PFNWGLDELETECONTEXTPROC)GetProcAddress(g_win_gl_mod, "wglDeleteContext");
  p_wglMakeCurrent = (PFNWGLMAKECURRENTPROC)GetProcAddress(g_win_gl_mod, "wglMakeCurrent");
  p_wglGetProcAddress = (PFNWGLGETPROCADDRESSPROC)GetProcAddress(g_win_gl_mod, "wglGetProcAddress");
  if (!p_wglCreateContext || !p_wglDeleteContext || !p_wglMakeCurrent || !p_wglGetProcAddress) {
    return 0;
  }
  HWND hwnd = NULL;
  HDC hdc = NULL;
  HGLRC ctx = NULL;
  if (!win_wgl_create_surface_ctx(&hwnd, &hdc, &ctx)) return 0;
  int missing = 0;
#define LOAD_GL_PTR(ret, name, args) \
  p_##name = (ret (*) args)win_gl_sym(#name); \
  if (!p_##name) missing = 1;
  FV_GL_FUNCS(LOAD_GL_PTR)
#undef LOAD_GL_PTR
  p_wglMakeCurrent(NULL, NULL);
  p_wglDeleteContext(ctx);
  ReleaseDC(hwnd, hdc);
  DestroyWindow(hwnd);
  g_win_gl_ok = missing ? 0 : 1;
  return g_win_gl_ok;
}
#else
static void *linux_gl_sym(const char *name) {
  void *p = NULL;
  if (p_eglGetProcAddress) p = p_eglGetProcAddress(name);
  if (!p && g_lib_gl) p = dlsym(g_lib_gl, name);
  return p;
}

static int linux_gl_init(void) {
  if (g_linux_gl_inited) return g_linux_gl_ok;
  g_linux_gl_inited = 1;
  g_lib_egl = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
  if (!g_lib_egl) return 0;
  g_lib_gl = dlopen("libGL.so.1", RTLD_LAZY | RTLD_LOCAL);
  if (!g_lib_gl) g_lib_gl = dlopen("libOpenGL.so.0", RTLD_LAZY | RTLD_LOCAL);
  p_eglGetDisplay = dlsym(g_lib_egl, "eglGetDisplay");
  p_eglGetPlatformDisplay = dlsym(g_lib_egl, "eglGetPlatformDisplay");
  p_eglInitialize = dlsym(g_lib_egl, "eglInitialize");
  p_eglBindAPI = dlsym(g_lib_egl, "eglBindAPI");
  p_eglChooseConfig = dlsym(g_lib_egl, "eglChooseConfig");
  p_eglCreatePbufferSurface = dlsym(g_lib_egl, "eglCreatePbufferSurface");
  p_eglCreateContext = dlsym(g_lib_egl, "eglCreateContext");
  p_eglMakeCurrent = dlsym(g_lib_egl, "eglMakeCurrent");
  p_eglDestroyContext = dlsym(g_lib_egl, "eglDestroyContext");
  p_eglDestroySurface = dlsym(g_lib_egl, "eglDestroySurface");
  p_eglGetProcAddress = dlsym(g_lib_egl, "eglGetProcAddress");
  if (!p_eglGetDisplay || !p_eglInitialize || !p_eglBindAPI || !p_eglChooseConfig ||
      !p_eglCreatePbufferSurface || !p_eglCreateContext || !p_eglMakeCurrent ||
      !p_eglDestroyContext || !p_eglDestroySurface) {
    return 0;
  }
  int missing = 0;
#define LOAD_GL_PTR(ret, name, args) \
  p_##name = (ret (*) args)linux_gl_sym(#name); \
  if (!p_##name) missing = 1;
  FV_GL_FUNCS(LOAD_GL_PTR)
#undef LOAD_GL_PTR
  g_linux_gl_ok = missing ? 0 : 1;
  return g_linux_gl_ok;
}
#endif
#endif

static int32_t make_current(int32_t ctx_id, FvWebGLCtx **out_ctx) {
  FvWebGLCtx *c = get_ctx(ctx_id);
  if (!c) return 0;
  int32_t slot = ctx_id - 1;
  if (g_current_slot != slot) {
#ifdef __APPLE__
    if (CGLSetCurrentContext(c->cgl_ctx) != kCGLNoError) return 0;
#elif defined(_WIN32)
    if (!p_wglMakeCurrent(c->wgl_hdc, c->wgl_ctx)) return 0;
#else
    if (!p_eglMakeCurrent(c->egl_dpy, c->egl_surf, c->egl_surf, c->egl_ctx)) return 0;
#endif
    g_current_slot = slot;
  }
  if (out_ctx) *out_ctx = c;
  return 1;
}

static void fv_webgl_shutdown_all(void) {
  for (int32_t i = 0; i < FV_WEBGL_MAX_CONTEXTS; i++) {
    if (g_ctxs[i].in_use) {
      fv_webgl_destroy(i + 1);
    }
  }
}

int32_t fv_webgl_available(void) {
#ifdef __APPLE__
  return 1;
#elif defined(_WIN32)
  return win_gl_init();
#else
  return linux_gl_init();
#endif
}

int32_t fv_webgl_active_count(void) {
  int32_t n = 0;
  for (int32_t i = 0; i < FV_WEBGL_MAX_CONTEXTS; i++) {
    if (g_ctxs[i].in_use) n++;
  }
  return n;
}

static int32_t setup_default_fbo(FvWebGLCtx *c, int32_t w, int32_t h) {
  glBindTexture(GL_TEXTURE_2D, c->color_tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glBindTexture(GL_TEXTURE_2D, 0);

  glBindRenderbuffer(GL_RENDERBUFFER, c->depth_stencil_rbo);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
  glBindRenderbuffer(GL_RENDERBUFFER, 0);

  glBindFramebuffer(GL_FRAMEBUFFER, c->default_fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->color_tex, 0);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, c->depth_stencil_rbo);

  GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  return status == GL_FRAMEBUFFER_COMPLETE ? 1 : 0;
}

int32_t fv_webgl_create(int32_t width, int32_t height, int32_t is_webgl2) {
  if (!fv_webgl_available()) return -1;
  int32_t slot = -1;
  for (int32_t i = 0; i < FV_WEBGL_MAX_CONTEXTS; i++) {
    if (!g_ctxs[i].in_use) {
      slot = i;
      break;
    }
  }
  if (slot < 0) return -1;

  int32_t w = clamp_dim(width);
  int32_t h = clamp_dim(height);
  FvWebGLCtx *c = &g_ctxs[slot];
  memset(c, 0, sizeof(*c));

#ifdef __APPLE__
  CGLPixelFormatAttribute attrs[] = {
    kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
    kCGLPFAAccelerated,
    kCGLPFAAllowOfflineRenderers,
    (CGLPixelFormatAttribute)0
  };
  CGLPixelFormatObj pix = NULL;
  GLint npix = 0;
  CGLError err = CGLChoosePixelFormat(attrs, &pix, &npix);
  if (err != kCGLNoError || !pix) {
    CGLPixelFormatAttribute sw_attrs[] = {
      kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
      (CGLPixelFormatAttribute)0
    };
    err = CGLChoosePixelFormat(sw_attrs, &pix, &npix);
    if (err != kCGLNoError || !pix) return -1;
  }
  err = CGLCreateContext(pix, NULL, &c->cgl_ctx);
  CGLDestroyPixelFormat(pix);
  if (err != kCGLNoError || !c->cgl_ctx) return -1;
  if (CGLSetCurrentContext(c->cgl_ctx) != kCGLNoError) {
    CGLDestroyContext(c->cgl_ctx);
    c->cgl_ctx = NULL;
    return -1;
  }
#elif defined(_WIN32)
  if (!win_wgl_create_surface_ctx(&c->wgl_hwnd, &c->wgl_hdc, &c->wgl_ctx)) {
    return -1;
  }
#else
  EGLDisplay dpy = p_eglGetDisplay(EGL_DEFAULT_DISPLAY);
  EGLint maj = 0, min = 0;
  int dpy_ok = (dpy != EGL_NO_DISPLAY) && p_eglInitialize(dpy, &maj, &min);
  if (!dpy_ok && p_eglGetPlatformDisplay) {
    dpy = p_eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    dpy_ok = (dpy != EGL_NO_DISPLAY) && p_eglInitialize(dpy, &maj, &min);
  }
  if (!dpy_ok) return -1;
  if (!p_eglBindAPI(EGL_OPENGL_API)) return -1;
  EGLint cfg_attrs[] = {
    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
    EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
    EGL_NONE
  };
  EGLConfig cfg = NULL;
  EGLint ncfg = 0;
  if (!p_eglChooseConfig(dpy, cfg_attrs, &cfg, 1, &ncfg) || ncfg < 1) {
    EGLint min_attrs[] = {
      EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
      EGL_NONE
    };
    if (!p_eglChooseConfig(dpy, min_attrs, &cfg, 1, &ncfg) || ncfg < 1) return -1;
  }
  EGLint pb_attrs[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
  EGLSurface surf = p_eglCreatePbufferSurface(dpy, cfg, pb_attrs);
  if (surf == EGL_NO_SURFACE) return -1;
  EGLint ctx_attrs[] = {
    EGL_CONTEXT_MAJOR_VERSION, 4,
    EGL_CONTEXT_MINOR_VERSION, 1,
    EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
    EGL_NONE
  };
  EGLContext ectx = p_eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs);
  if (ectx == EGL_NO_CONTEXT) {
    /* Fallback to default 3.3+ core if 4.1 unavailable */
    EGLint ctx_attrs3[] = {
      EGL_CONTEXT_MAJOR_VERSION, 3,
      EGL_CONTEXT_MINOR_VERSION, 3,
      EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
      EGL_NONE
    };
    ectx = p_eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attrs3);
  }
  if (ectx == EGL_NO_CONTEXT) {
    p_eglDestroySurface(dpy, surf);
    return -1;
  }
  if (!p_eglMakeCurrent(dpy, surf, surf, ectx)) {
    p_eglDestroyContext(dpy, ectx);
    p_eglDestroySurface(dpy, surf);
    return -1;
  }
  c->egl_dpy = dpy;
  c->egl_surf = surf;
  c->egl_ctx = ectx;
#endif

  g_current_slot = slot;
  c->in_use = 1;
  c->is_webgl2 = is_webgl2 ? 1 : 0;
  c->width = w;
  c->height = h;
  c->dirty = 0;
  c->unpack_alignment = 4;
  c->pack_alignment = 4;

  glGenFramebuffers(1, &c->default_fbo);
  glGenTextures(1, &c->color_tex);
  glGenRenderbuffers(1, &c->depth_stencil_rbo);
  glGenVertexArrays(1, &c->default_vao);

  if (!setup_default_fbo(c, w, h)) {
    fv_webgl_destroy(slot + 1);
    return -1;
  }

  glBindVertexArray(c->default_vao);
  glEnable(GL_PROGRAM_POINT_SIZE);
  glViewport(0, 0, w, h);
  glScissor(0, 0, w, h);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClearDepth(1.0);
  glClearStencil(0);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
  /* Clear any init error */
  while (glGetError() != GL_NO_ERROR) {}

  if (!g_atexit_registered) {
    g_atexit_registered = 1;
    atexit(fv_webgl_shutdown_all);
  }
  return slot + 1;
}

void fv_webgl_destroy(int32_t ctx_id) {
  FvWebGLCtx *c = get_ctx(ctx_id);
  if (!c) return;
  if (make_current(ctx_id, NULL)) {
    if (c->default_vao) glDeleteVertexArrays(1, &c->default_vao);
    if (c->default_fbo) glDeleteFramebuffers(1, &c->default_fbo);
    if (c->color_tex) glDeleteTextures(1, &c->color_tex);
    if (c->depth_stencil_rbo) glDeleteRenderbuffers(1, &c->depth_stencil_rbo);
    if (c->blit_fbo) glDeleteFramebuffers(1, &c->blit_fbo);
    if (c->blit_tex) glDeleteTextures(1, &c->blit_tex);
    if (c->pbo[0] || c->pbo[1]) glDeleteBuffers(2, c->pbo);
#ifdef __APPLE__
    CGLSetCurrentContext(NULL);
#elif defined(_WIN32)
    p_wglMakeCurrent(NULL, NULL);
#else
    p_eglMakeCurrent(c->egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
#endif
    g_current_slot = -1;
  }
#ifdef __APPLE__
  if (c->cgl_ctx) {
    CGLDestroyContext(c->cgl_ctx);
    c->cgl_ctx = NULL;
  }
#elif defined(_WIN32)
  if (c->wgl_ctx) p_wglDeleteContext(c->wgl_ctx);
  if (c->wgl_hwnd && c->wgl_hdc) ReleaseDC(c->wgl_hwnd, c->wgl_hdc);
  if (c->wgl_hwnd) DestroyWindow(c->wgl_hwnd);
  c->wgl_ctx = NULL;
  c->wgl_hdc = NULL;
  c->wgl_hwnd = NULL;
#else
  if (c->egl_ctx) p_eglDestroyContext(c->egl_dpy, c->egl_ctx);
  if (c->egl_surf) p_eglDestroySurface(c->egl_dpy, c->egl_surf);
  c->egl_ctx = NULL;
  c->egl_surf = NULL;
  c->egl_dpy = NULL;
#endif
  memset(c, 0, sizeof(*c));
}

int32_t fv_webgl_resize(int32_t ctx_id, int32_t width, int32_t height) {
  FvWebGLCtx *c = NULL;
  if (!make_current(ctx_id, &c)) return 0;
  int32_t w = clamp_dim(width);
  int32_t h = clamp_dim(height);
  if (w == c->width && h == c->height) return 1;
  c->width = w;
  c->height = h;
  int32_t ok = setup_default_fbo(c, w, h);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
  /* Restore previously bound FBOs */
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, c->bound_draw_fbo ? c->bound_draw_fbo : c->default_fbo);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, c->bound_read_fbo ? c->bound_read_fbo : c->default_fbo);
  c->dirty = 1;
  return ok;
}

int32_t fv_webgl_width(int32_t ctx_id) {
  FvWebGLCtx *c = get_ctx(ctx_id);
  return c ? c->width : 0;
}

int32_t fv_webgl_height(int32_t ctx_id) {
  FvWebGLCtx *c = get_ctx(ctx_id);
  return c ? c->height : 0;
}

int32_t fv_webgl_is_dirty(int32_t ctx_id) {
  FvWebGLCtx *c = get_ctx(ctx_id);
  return c ? c->dirty : 0;
}

void fv_webgl_clear_dirty(int32_t ctx_id) {
  FvWebGLCtx *c = get_ctx(ctx_id);
  if (c) c->dirty = 0;
}

/* --- Shaders & Programs --- */

uint32_t fv_webgl_create_shader(int32_t ctx_id, uint32_t type) {
  if (!make_current(ctx_id, NULL)) return 0;
  return (uint32_t)glCreateShader((GLenum)type);
}

void fv_webgl_shader_source(int32_t ctx_id, uint32_t shader, const char *src, int32_t len) {
  if (!make_current(ctx_id, NULL) || !src) return;
  const GLchar *s = (const GLchar *)src;
  GLint l = (GLint)len;
  glShaderSource((GLuint)shader, 1, &s, &l);
}

void fv_webgl_compile_shader(int32_t ctx_id, uint32_t shader) {
  if (!make_current(ctx_id, NULL)) return;
  glCompileShader((GLuint)shader);
}

int32_t fv_webgl_get_shader_iv(int32_t ctx_id, uint32_t shader, uint32_t pname) {
  if (!make_current(ctx_id, NULL)) return 0;
  GLint v = 0;
  glGetShaderiv((GLuint)shader, (GLenum)pname, &v);
  return (int32_t)v;
}

int32_t fv_webgl_get_shader_info_log(int32_t ctx_id, uint32_t shader, char *out_buf, int32_t max_len) {
  if (!make_current(ctx_id, NULL) || !out_buf || max_len <= 0) return 0;
  GLsizei len = 0;
  glGetShaderInfoLog((GLuint)shader, (GLsizei)max_len, &len, (GLchar *)out_buf);
  if (len < 0) len = 0;
  if (len >= max_len) len = max_len - 1;
  out_buf[len] = '\0';
  return (int32_t)len;
}

void fv_webgl_delete_shader(int32_t ctx_id, uint32_t shader) {
  if (!make_current(ctx_id, NULL)) return;
  glDeleteShader((GLuint)shader);
}

uint32_t fv_webgl_create_program(int32_t ctx_id) {
  if (!make_current(ctx_id, NULL)) return 0;
  return (uint32_t)glCreateProgram();
}

void fv_webgl_attach_shader(int32_t ctx_id, uint32_t program, uint32_t shader) {
  if (!make_current(ctx_id, NULL)) return;
  glAttachShader((GLuint)program, (GLuint)shader);
}

void fv_webgl_detach_shader(int32_t ctx_id, uint32_t program, uint32_t shader) {
  if (!make_current(ctx_id, NULL)) return;
  glDetachShader((GLuint)program, (GLuint)shader);
}

void fv_webgl_link_program(int32_t ctx_id, uint32_t program) {
  if (!make_current(ctx_id, NULL)) return;
  glLinkProgram((GLuint)program);
}

void fv_webgl_validate_program(int32_t ctx_id, uint32_t program) {
  if (!make_current(ctx_id, NULL)) return;
  glValidateProgram((GLuint)program);
}

int32_t fv_webgl_get_program_iv(int32_t ctx_id, uint32_t program, uint32_t pname) {
  if (!make_current(ctx_id, NULL)) return 0;
  GLint v = 0;
  glGetProgramiv((GLuint)program, (GLenum)pname, &v);
  return (int32_t)v;
}

int32_t fv_webgl_get_program_info_log(int32_t ctx_id, uint32_t program, char *out_buf, int32_t max_len) {
  if (!make_current(ctx_id, NULL) || !out_buf || max_len <= 0) return 0;
  GLsizei len = 0;
  glGetProgramInfoLog((GLuint)program, (GLsizei)max_len, &len, (GLchar *)out_buf);
  if (len < 0) len = 0;
  if (len >= max_len) len = max_len - 1;
  out_buf[len] = '\0';
  return (int32_t)len;
}

void fv_webgl_use_program(int32_t ctx_id, uint32_t program) {
  if (!make_current(ctx_id, NULL)) return;
  glUseProgram((GLuint)program);
}

void fv_webgl_delete_program(int32_t ctx_id, uint32_t program) {
  if (!make_current(ctx_id, NULL)) return;
  glDeleteProgram((GLuint)program);
}

int32_t fv_webgl_get_attrib_location(int32_t ctx_id, uint32_t program, const char *name) {
  if (!make_current(ctx_id, NULL) || !name) return -1;
  return (int32_t)glGetAttribLocation((GLuint)program, (const GLchar *)name);
}

void fv_webgl_bind_attrib_location(int32_t ctx_id, uint32_t program, uint32_t index, const char *name) {
  if (!make_current(ctx_id, NULL) || !name) return;
  glBindAttribLocation((GLuint)program, (GLuint)index, (const GLchar *)name);
}

int32_t fv_webgl_get_uniform_location(int32_t ctx_id, uint32_t program, const char *name) {
  if (!make_current(ctx_id, NULL) || !name) return -1;
  return (int32_t)glGetUniformLocation((GLuint)program, (const GLchar *)name);
}

uint32_t fv_webgl_get_uniform_block_index(int32_t ctx_id, uint32_t program, const char *name) {
  if (!make_current(ctx_id, NULL) || !name) return 0xFFFFFFFFu;
  return (uint32_t)glGetUniformBlockIndex((GLuint)program, (const GLchar *)name);
}

void fv_webgl_uniform_block_binding(int32_t ctx_id, uint32_t program, uint32_t block_index, uint32_t block_binding) {
  if (!make_current(ctx_id, NULL)) return;
  glUniformBlockBinding((GLuint)program, (GLuint)block_index, (GLuint)block_binding);
}

void fv_webgl_bind_buffer_base(int32_t ctx_id, uint32_t target, uint32_t index, uint32_t buffer) {
  if (!make_current(ctx_id, NULL)) return;
  glBindBufferBase((GLenum)target, (GLuint)index, (GLuint)buffer);
}

void fv_webgl_bind_buffer_range(int32_t ctx_id, uint32_t target, uint32_t index, uint32_t buffer,
                                int32_t offset, int32_t size) {
  if (!make_current(ctx_id, NULL) || offset < 0 || size < 0) return;
  glBindBufferRange((GLenum)target, (GLuint)index, (GLuint)buffer, (GLintptr)offset, (GLsizeiptr)size);
}

int32_t fv_webgl_get_active_attrib(int32_t ctx_id, uint32_t program, uint32_t index,
                                   int32_t *out_size, uint32_t *out_type, char *out_name, int32_t max_len) {
  if (!make_current(ctx_id, NULL) || !out_name || max_len <= 0) return -1;
  GLsizei len = 0;
  GLint sz = 0;
  GLenum ty = 0;
  glGetActiveAttrib((GLuint)program, (GLuint)index, (GLsizei)max_len, &len, &sz, &ty, (GLchar *)out_name);
  if (out_size) *out_size = (int32_t)sz;
  if (out_type) *out_type = (uint32_t)ty;
  if (len < 0) len = 0;
  if (len >= max_len) len = max_len - 1;
  out_name[len] = '\0';
  return (int32_t)len;
}

int32_t fv_webgl_get_active_uniform(int32_t ctx_id, uint32_t program, uint32_t index,
                                    int32_t *out_size, uint32_t *out_type, char *out_name, int32_t max_len) {
  if (!make_current(ctx_id, NULL) || !out_name || max_len <= 0) return -1;
  GLsizei len = 0;
  GLint sz = 0;
  GLenum ty = 0;
  glGetActiveUniform((GLuint)program, (GLuint)index, (GLsizei)max_len, &len, &sz, &ty, (GLchar *)out_name);
  if (out_size) *out_size = (int32_t)sz;
  if (out_type) *out_type = (uint32_t)ty;
  if (len < 0) len = 0;
  if (len >= max_len) len = max_len - 1;
  out_name[len] = '\0';
  return (int32_t)len;
}

/* --- Buffers, VAOs & Vertex Attributes --- */

uint32_t fv_webgl_create_buffer(int32_t ctx_id) {
  if (!make_current(ctx_id, NULL)) return 0;
  GLuint b = 0;
  glGenBuffers(1, &b);
  return (uint32_t)b;
}

void fv_webgl_bind_buffer(int32_t ctx_id, uint32_t target, uint32_t buffer) {
  if (!make_current(ctx_id, NULL)) return;
  glBindBuffer((GLenum)target, (GLuint)buffer);
}

void fv_webgl_buffer_data(int32_t ctx_id, uint32_t target, int32_t size, const void *data, uint32_t usage) {
  if (!make_current(ctx_id, NULL) || size < 0) return;
  glBufferData((GLenum)target, (GLsizeiptr)size, data, (GLenum)usage);
}

void fv_webgl_buffer_sub_data(int32_t ctx_id, uint32_t target, int32_t offset, int32_t size, const void *data) {
  if (!make_current(ctx_id, NULL) || offset < 0 || size <= 0 || !data) return;
  glBufferSubData((GLenum)target, (GLintptr)offset, (GLsizeiptr)size, data);
}

void fv_webgl_delete_buffer(int32_t ctx_id, uint32_t buffer) {
  if (!make_current(ctx_id, NULL)) return;
  GLuint b = (GLuint)buffer;
  glDeleteBuffers(1, &b);
}

uint32_t fv_webgl_create_vao(int32_t ctx_id) {
  if (!make_current(ctx_id, NULL)) return 0;
  GLuint v = 0;
  glGenVertexArrays(1, &v);
  return (uint32_t)v;
}

void fv_webgl_bind_vao(int32_t ctx_id, uint32_t vao) {
  FvWebGLCtx *c = NULL;
  if (!make_current(ctx_id, &c)) return;
  c->bound_vao = (GLuint)vao;
  glBindVertexArray(vao ? (GLuint)vao : c->default_vao);
}

void fv_webgl_delete_vao(int32_t ctx_id, uint32_t vao) {
  FvWebGLCtx *c = NULL;
  if (!make_current(ctx_id, &c) || !vao) return;
  GLuint v = (GLuint)vao;
  if (c->bound_vao == v) {
    c->bound_vao = 0;
    glBindVertexArray(c->default_vao);
  }
  glDeleteVertexArrays(1, &v);
}

void fv_webgl_enable_vertex_attrib_array(int32_t ctx_id, uint32_t index) {
  if (!make_current(ctx_id, NULL)) return;
  glEnableVertexAttribArray((GLuint)index);
}

void fv_webgl_disable_vertex_attrib_array(int32_t ctx_id, uint32_t index) {
  if (!make_current(ctx_id, NULL)) return;
  glDisableVertexAttribArray((GLuint)index);
}

void fv_webgl_vertex_attrib_pointer(int32_t ctx_id, uint32_t index, int32_t size,
                                    uint32_t type, int32_t normalized, int32_t stride, int32_t offset) {
  if (!make_current(ctx_id, NULL)) return;
  if (type == FV_GL_HALF_FLOAT_OES) type = GL_HALF_FLOAT;
  glVertexAttribPointer((GLuint)index, (GLint)size, (GLenum)type,
                        normalized ? GL_TRUE : GL_FALSE, (GLsizei)stride,
                        (const void *)(uintptr_t)(offset > 0 ? offset : 0));
}

void fv_webgl_vertex_attrib_ipointer(int32_t ctx_id, uint32_t index, int32_t size,
                                     uint32_t type, int32_t stride, int32_t offset) {
  if (!make_current(ctx_id, NULL)) return;
  glVertexAttribIPointer((GLuint)index, (GLint)size, (GLenum)type, (GLsizei)stride,
                         (const void *)(uintptr_t)(offset > 0 ? offset : 0));
}

void fv_webgl_vertex_attrib_divisor(int32_t ctx_id, uint32_t index, uint32_t divisor) {
  if (!make_current(ctx_id, NULL)) return;
  glVertexAttribDivisor((GLuint)index, (GLuint)divisor);
}

void fv_webgl_vertex_attrib_fv(int32_t ctx_id, uint32_t index, int32_t dim, const float *vals) {
  if (!make_current(ctx_id, NULL) || !vals) return;
  switch (dim) {
    case 1: glVertexAttrib1fv((GLuint)index, vals); break;
    case 2: glVertexAttrib2fv((GLuint)index, vals); break;
    case 3: glVertexAttrib3fv((GLuint)index, vals); break;
    case 4: glVertexAttrib4fv((GLuint)index, vals); break;
    default: break;
  }
}

/* --- Uniforms --- */

void fv_webgl_uniform_fv(int32_t ctx_id, int32_t loc, int32_t dim, int32_t count, const float *vals) {
  if (!make_current(ctx_id, NULL) || loc < 0 || count <= 0 || !vals) return;
  switch (dim) {
    case 1: glUniform1fv((GLint)loc, (GLsizei)count, vals); break;
    case 2: glUniform2fv((GLint)loc, (GLsizei)count, vals); break;
    case 3: glUniform3fv((GLint)loc, (GLsizei)count, vals); break;
    case 4: glUniform4fv((GLint)loc, (GLsizei)count, vals); break;
    default: break;
  }
}

void fv_webgl_uniform_iv(int32_t ctx_id, int32_t loc, int32_t dim, int32_t count, const int32_t *vals) {
  if (!make_current(ctx_id, NULL) || loc < 0 || count <= 0 || !vals) return;
  switch (dim) {
    case 1: glUniform1iv((GLint)loc, (GLsizei)count, (const GLint *)vals); break;
    case 2: glUniform2iv((GLint)loc, (GLsizei)count, (const GLint *)vals); break;
    case 3: glUniform3iv((GLint)loc, (GLsizei)count, (const GLint *)vals); break;
    case 4: glUniform4iv((GLint)loc, (GLsizei)count, (const GLint *)vals); break;
    default: break;
  }
}

void fv_webgl_uniform_uiv(int32_t ctx_id, int32_t loc, int32_t dim, int32_t count, const uint32_t *vals) {
  if (!make_current(ctx_id, NULL) || loc < 0 || count <= 0 || !vals) return;
  switch (dim) {
    case 1: glUniform1uiv((GLint)loc, (GLsizei)count, (const GLuint *)vals); break;
    case 2: glUniform2uiv((GLint)loc, (GLsizei)count, (const GLuint *)vals); break;
    case 3: glUniform3uiv((GLint)loc, (GLsizei)count, (const GLuint *)vals); break;
    case 4: glUniform4uiv((GLint)loc, (GLsizei)count, (const GLuint *)vals); break;
    default: break;
  }
}

void fv_webgl_uniform_matrix_fv(int32_t ctx_id, int32_t loc, int32_t dim, int32_t count,
                                int32_t transpose, const float *vals) {
  if (!make_current(ctx_id, NULL) || loc < 0 || count <= 0 || !vals) return;
  GLboolean tr = transpose ? GL_TRUE : GL_FALSE;
  switch (dim) {
    case 2: glUniformMatrix2fv((GLint)loc, (GLsizei)count, tr, vals); break;
    case 3: glUniformMatrix3fv((GLint)loc, (GLsizei)count, tr, vals); break;
    case 4: glUniformMatrix4fv((GLint)loc, (GLsizei)count, tr, vals); break;
    default: break;
  }
}

/* --- Textures & PixelStore --- */

uint32_t fv_webgl_create_texture(int32_t ctx_id) {
  if (!make_current(ctx_id, NULL)) return 0;
  GLuint t = 0;
  glGenTextures(1, &t);
  return (uint32_t)t;
}

void fv_webgl_bind_texture(int32_t ctx_id, uint32_t target, uint32_t texture) {
  if (!make_current(ctx_id, NULL)) return;
  glBindTexture((GLenum)target, (GLuint)texture);
}

void fv_webgl_active_texture(int32_t ctx_id, uint32_t unit) {
  if (!make_current(ctx_id, NULL)) return;
  glActiveTexture((GLenum)unit);
}

void fv_webgl_delete_texture(int32_t ctx_id, uint32_t texture) {
  if (!make_current(ctx_id, NULL)) return;
  GLuint t = (GLuint)texture;
  glDeleteTextures(1, &t);
}

void fv_webgl_pixel_storei(int32_t ctx_id, uint32_t pname, int32_t param) {
  FvWebGLCtx *c = NULL;
  if (!make_current(ctx_id, &c)) return;
  if (pname == FV_GL_UNPACK_FLIP_Y_WEBGL) {
    c->unpack_flip_y = param ? 1 : 0;
    return;
  }
  if (pname == FV_GL_UNPACK_PREMULTIPLY_ALPHA_WEBGL) {
    c->unpack_premultiply_alpha = param ? 1 : 0;
    return;
  }
  if (pname == FV_GL_UNPACK_COLORSPACE_CONVERSION_WEBGL) {
    return;
  }
  if (pname == GL_UNPACK_ALIGNMENT) {
    c->unpack_alignment = param;
  } else if (pname == GL_PACK_ALIGNMENT) {
    c->pack_alignment = param;
  }
  glPixelStorei((GLenum)pname, (GLint)param);
}

static int32_t format_channels(uint32_t fmt) {
  switch (fmt) {
    case GL_RGBA: return 4;
    case GL_RGB: return 3;
    case GL_RG:
    case FV_GL_LUMINANCE_ALPHA: return 2;
    case GL_RED:
    case GL_DEPTH_COMPONENT:
    case FV_GL_LUMINANCE:
    case FV_GL_ALPHA: return 1;
    default: return 4;
  }
}

static int32_t type_bytes_per_comp(uint32_t ty) {
  switch (ty) {
    case GL_UNSIGNED_BYTE: return 1;
    case GL_UNSIGNED_SHORT:
    case GL_HALF_FLOAT:
    case FV_GL_HALF_FLOAT_OES: return 2;
    case GL_FLOAT:
    case GL_UNSIGNED_INT: return 4;
    default: return 1;
  }
}

static void map_tex_formats(int32_t *internalformat, uint32_t *format, uint32_t *type, int32_t *expand_rgba8) {
  *expand_rgba8 = 0;
  if (*type == FV_GL_HALF_FLOAT_OES) *type = GL_HALF_FLOAT;
  if (*format == FV_GL_LUMINANCE || *format == FV_GL_LUMINANCE_ALPHA || *format == FV_GL_ALPHA) {
    *expand_rgba8 = 1;
    *internalformat = GL_RGBA8;
    *format = GL_RGBA;
    *type = GL_UNSIGNED_BYTE;
    return;
  }
  if (*internalformat == (int32_t)GL_RGBA) {
    if (*type == GL_FLOAT) *internalformat = GL_RGBA32F;
    else if (*type == GL_HALF_FLOAT) *internalformat = GL_RGBA16F;
    else *internalformat = GL_RGBA8;
  } else if (*internalformat == (int32_t)GL_RGB) {
    if (*type == GL_FLOAT) *internalformat = GL_RGB32F;
    else if (*type == GL_HALF_FLOAT) *internalformat = GL_RGB16F;
    else *internalformat = GL_RGB8;
  } else if (*internalformat == (int32_t)GL_SRGB) {
    *internalformat = (*format == GL_RGBA) ? GL_SRGB8_ALPHA8 : GL_SRGB8;
  } else if (*internalformat == (int32_t)GL_DEPTH_COMPONENT) {
    *internalformat = GL_DEPTH_COMPONENT24;
  } else if (*internalformat == (int32_t)GL_DEPTH_STENCIL) {
    *internalformat = GL_DEPTH24_STENCIL8;
  }
}

/* Prepares pixel buffer with ES2 legacy format expansion, UNPACK_FLIP_Y_WEBGL, and UNPACK_PREMULTIPLY_ALPHA_WEBGL.
 * Caller must free(*out_alloc) if non-NULL. */
static const void *prepare_tex_pixels(const FvWebGLCtx *c, int32_t width, int32_t height,
                                      uint32_t orig_format, uint32_t orig_type,
                                      int32_t expand_rgba8, const void *data, int32_t data_len,
                                      uint8_t **out_alloc) {
  *out_alloc = NULL;
  if (!data || width <= 0 || height <= 0) return data;
  int32_t flip = c->unpack_flip_y;
  int32_t premul = c->unpack_premultiply_alpha;
  if (!expand_rgba8 && !flip && !premul) return data;

  if (expand_rgba8) {
    size_t total = (size_t)width * (size_t)height * 4;
    uint8_t *dst = (uint8_t *)malloc(total);
    if (!dst) return data;
    const uint8_t *src = (const uint8_t *)data;
    int32_t src_ch = format_channels(orig_format);
    size_t needed = (size_t)width * (size_t)height * (size_t)src_ch;
    if ((size_t)data_len < needed) {
      memset(dst, 0, total);
      *out_alloc = dst;
      return dst;
    }
    for (int32_t y = 0; y < height; y++) {
      int32_t sy = flip ? (height - 1 - y) : y;
      for (int32_t x = 0; x < width; x++) {
        size_t si = ((size_t)sy * (size_t)width + (size_t)x) * (size_t)src_ch;
        size_t di = ((size_t)y * (size_t)width + (size_t)x) * 4;
        uint8_t r = 0, g = 0, b = 0, a = 255;
        if (orig_format == FV_GL_LUMINANCE) {
          r = g = b = src[si]; a = 255;
        } else if (orig_format == FV_GL_LUMINANCE_ALPHA) {
          r = g = b = src[si]; a = src[si + 1];
        } else if (orig_format == FV_GL_ALPHA) {
          r = g = b = 0; a = src[si];
        }
        if (premul && a < 255) {
          r = (uint8_t)((r * a + 127) / 255);
          g = (uint8_t)((g * a + 127) / 255);
          b = (uint8_t)((b * a + 127) / 255);
        }
        dst[di + 0] = r;
        dst[di + 1] = g;
        dst[di + 2] = b;
        dst[di + 3] = a;
      }
    }
    *out_alloc = dst;
    return dst;
  }

  int32_t ch = format_channels(orig_format);
  int32_t bpc = type_bytes_per_comp(orig_type);
  size_t row_bytes = (size_t)width * (size_t)ch * (size_t)bpc;
  size_t total = row_bytes * (size_t)height;
  if ((size_t)data_len < total) return data;
  uint8_t *dst = (uint8_t *)malloc(total);
  if (!dst) return data;
  const uint8_t *src = (const uint8_t *)data;
  for (int32_t y = 0; y < height; y++) {
    int32_t sy = flip ? (height - 1 - y) : y;
    memcpy(dst + (size_t)y * row_bytes, src + (size_t)sy * row_bytes, row_bytes);
  }
  if (premul && orig_format == GL_RGBA && orig_type == GL_UNSIGNED_BYTE) {
    for (size_t i = 0; i < total; i += 4) {
      uint32_t a = dst[i + 3];
      if (a < 255) {
        dst[i + 0] = (uint8_t)((dst[i + 0] * a + 127) / 255);
        dst[i + 1] = (uint8_t)((dst[i + 1] * a + 127) / 255);
        dst[i + 2] = (uint8_t)((dst[i + 2] * a + 127) / 255);
      }
    }
  }
  *out_alloc = dst;
  return dst;
}

static uint64_t tex_required_bytes(int32_t width, int32_t height, uint32_t orig_fmt, uint32_t orig_ty, int32_t align) {
  if (width <= 0 || height <= 0) return 0;
  uint64_t ch = (uint64_t)format_channels(orig_fmt);
  uint64_t bpc = (uint64_t)type_bytes_per_comp(orig_ty);
  uint64_t row_bytes = (uint64_t)width * ch * bpc;
  uint64_t a = (align == 1 || align == 2 || align == 4 || align == 8) ? (uint64_t)align : 1;
  uint64_t row_stride = ((row_bytes + a - 1) / a) * a;
  return row_stride * (uint64_t)(height - 1) + row_bytes;
}

void fv_webgl_tex_image_2d(int32_t ctx_id, uint32_t target, int32_t level, int32_t internalformat,
                           int32_t width, int32_t height, int32_t border, uint32_t format,
                           uint32_t type, const void *data, int32_t data_len) {
  FvWebGLCtx *c = NULL;
  if (!make_current(ctx_id, &c)) return;
  if (width < 0 || height < 0 || width > FV_WEBGL_MAX_DIM || height > FV_WEBGL_MAX_DIM) return;
  uint32_t orig_fmt = format;
  uint32_t orig_ty = type;
  if (data) {
    uint64_t needed = tex_required_bytes(width, height, orig_fmt, orig_ty, c->unpack_alignment);
    if (data_len < 0 || (uint64_t)data_len < needed) return;
  }
  int32_t expand_rgba8 = 0;
  map_tex_formats(&internalformat, &format, &type, &expand_rgba8);
  uint8_t *tmp = NULL;
  const void *pixels = prepare_tex_pixels(c, width, height, orig_fmt, orig_ty, expand_rgba8, data, data_len, &tmp);
  if (tmp || (pixels && c->unpack_alignment != 1)) {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D((GLenum)target, (GLint)level, (GLint)internalformat, (GLsizei)width, (GLsizei)height,
                 (GLint)border, (GLenum)format, (GLenum)type, pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, c->unpack_alignment);
  } else {
    glTexImage2D((GLenum)target, (GLint)level, (GLint)internalformat, (GLsizei)width, (GLsizei)height,
                 (GLint)border, (GLenum)format, (GLenum)type, pixels);
  }
  free(tmp);
}

void fv_webgl_tex_sub_image_2d(int32_t ctx_id, uint32_t target, int32_t level,
                               int32_t xoffset, int32_t yoffset, int32_t width, int32_t height,
                               uint32_t format, uint32_t type, const void *data, int32_t data_len) {
  FvWebGLCtx *c = NULL;
  if (!make_current(ctx_id, &c) || !data) return;
  if (xoffset < 0 || yoffset < 0 || width < 0 || height < 0 ||
      width > FV_WEBGL_MAX_DIM || height > FV_WEBGL_MAX_DIM) return;
  uint32_t orig_fmt = format;
  uint32_t orig_ty = type;
  uint64_t needed = tex_required_bytes(width, height, orig_fmt, orig_ty, c->unpack_alignment);
  if (data_len < 0 || (uint64_t)data_len < needed) return;
  int32_t dummy_ifmt = (int32_t)format;
  int32_t expand_rgba8 = 0;
  map_tex_formats(&dummy_ifmt, &format, &type, &expand_rgba8);
  uint8_t *tmp = NULL;
  const void *pixels = prepare_tex_pixels(c, width, height, orig_fmt, orig_ty, expand_rgba8, data, data_len, &tmp);
  if (tmp && c->unpack_alignment != 1) {
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D((GLenum)target, (GLint)level, (GLint)xoffset, (GLint)yoffset,
                    (GLsizei)width, (GLsizei)height, (GLenum)format, (GLenum)type, pixels);
    glPixelStorei(GL_UNPACK_ALIGNMENT, c->unpack_alignment);
  } else {
    glTexSubImage2D((GLenum)target, (GLint)level, (GLint)xoffset, (GLint)yoffset,
                    (GLsizei)width, (GLsizei)height, (GLenum)format, (GLenum)type, pixels);
  }
  free(tmp);
}

void fv_webgl_tex_parameteri(int32_t ctx_id, uint32_t target, uint32_t pname, int32_t param) {
  if (!make_current(ctx_id, NULL)) return;
  glTexParameteri((GLenum)target, (GLenum)pname, (GLint)param);
}

void fv_webgl_tex_parameterf(int32_t ctx_id, uint32_t target, uint32_t pname, float param) {
  if (!make_current(ctx_id, NULL)) return;
  glTexParameterf((GLenum)target, (GLenum)pname, (GLfloat)param);
}

void fv_webgl_generate_mipmap(int32_t ctx_id, uint32_t target) {
  if (!make_current(ctx_id, NULL)) return;
  glGenerateMipmap((GLenum)target);
}

uint32_t fv_webgl_create_sampler(int32_t ctx_id) {
  if (!make_current(ctx_id, NULL)) return 0;
  GLuint s = 0;
  glGenSamplers(1, &s);
  return (uint32_t)s;
}

void fv_webgl_delete_sampler(int32_t ctx_id, uint32_t sampler) {
  if (!make_current(ctx_id, NULL) || !sampler) return;
  GLuint s = (GLuint)sampler;
  glDeleteSamplers(1, &s);
}

void fv_webgl_bind_sampler(int32_t ctx_id, uint32_t unit, uint32_t sampler) {
  if (!make_current(ctx_id, NULL)) return;
  glBindSampler((GLuint)unit, (GLuint)sampler);
}

void fv_webgl_sampler_parameteri(int32_t ctx_id, uint32_t sampler, uint32_t pname, int32_t param) {
  if (!make_current(ctx_id, NULL) || !sampler) return;
  glSamplerParameteri((GLuint)sampler, (GLenum)pname, (GLint)param);
}

void fv_webgl_sampler_parameterf(int32_t ctx_id, uint32_t sampler, uint32_t pname, float param) {
  if (!make_current(ctx_id, NULL) || !sampler) return;
  glSamplerParameterf((GLuint)sampler, (GLenum)pname, (GLfloat)param);
}

/* --- Framebuffers & Renderbuffers --- */

uint32_t fv_webgl_create_framebuffer(int32_t ctx_id) {
  if (!make_current(ctx_id, NULL)) return 0;
  GLuint f = 0;
  glGenFramebuffers(1, &f);
  return (uint32_t)f;
}

void fv_webgl_bind_framebuffer(int32_t ctx_id, uint32_t target, uint32_t fbo) {
  FvWebGLCtx *c = NULL;
  if (!make_current(ctx_id, &c)) return;
  if (target == GL_FRAMEBUFFER) {
    c->bound_draw_fbo = (GLuint)fbo;
    c->bound_read_fbo = (GLuint)fbo;
  } else if (target == GL_DRAW_FRAMEBUFFER) {
    c->bound_draw_fbo = (GLuint)fbo;
  } else if (target == GL_READ_FRAMEBUFFER) {
    c->bound_read_fbo = (GLuint)fbo;
  }
  glBindFramebuffer((GLenum)target, fbo ? (GLuint)fbo : c->default_fbo);
}

void fv_webgl_framebuffer_texture_2d(int32_t ctx_id, uint32_t target, uint32_t attachment,
                                     uint32_t textarget, uint32_t texture, int32_t level) {
  if (!make_current(ctx_id, NULL)) return;
  glFramebufferTexture2D((GLenum)target, (GLenum)attachment, (GLenum)textarget, (GLuint)texture, (GLint)level);
}

void fv_webgl_framebuffer_renderbuffer(int32_t ctx_id, uint32_t target, uint32_t attachment,
                                       uint32_t rbtarget, uint32_t rbo) {
  if (!make_current(ctx_id, NULL)) return;
  glFramebufferRenderbuffer((GLenum)target, (GLenum)attachment, (GLenum)rbtarget, (GLuint)rbo);
}

uint32_t fv_webgl_check_framebuffer_status(int32_t ctx_id, uint32_t target) {
  if (!make_current(ctx_id, NULL)) return 0;
  return (uint32_t)glCheckFramebufferStatus((GLenum)target);
}

void fv_webgl_delete_framebuffer(int32_t ctx_id, uint32_t fbo) {
  FvWebGLCtx *c = NULL;
  if (!make_current(ctx_id, &c) || !fbo) return;
  GLuint f = (GLuint)fbo;
  if (c->bound_draw_fbo == f) {
    c->bound_draw_fbo = 0;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, c->default_fbo);
  }
  if (c->bound_read_fbo == f) {
    c->bound_read_fbo = 0;
    glBindFramebuffer(GL_READ_FRAMEBUFFER, c->default_fbo);
  }
  glDeleteFramebuffers(1, &f);
}

void fv_webgl_draw_buffers(int32_t ctx_id, int32_t count, const uint32_t *bufs) {
  FvWebGLCtx *c = NULL;
  if (!make_current(ctx_id, &c) || count <= 0 || !bufs) return;
  if (c->bound_draw_fbo == 0) {
    GLenum mapped[16];
    int32_t n = count > 16 ? 16 : count;
    for (int32_t i = 0; i < n; i++) {
      mapped[i] = (bufs[i] == 0x0405u /* GL_BACK */) ? GL_COLOR_ATTACHMENT0 : (GLenum)bufs[i];
    }
    glDrawBuffers((GLsizei)n, mapped);
    return;
  }
  glDrawBuffers((GLsizei)count, (const GLenum *)bufs);
}

void fv_webgl_read_buffer(int32_t ctx_id, uint32_t src) {
  FvWebGLCtx *c = NULL;
  if (!make_current(ctx_id, &c)) return;
  GLenum mapped = (c->bound_read_fbo == 0 && src == 0x0405u /* GL_BACK */)
                      ? GL_COLOR_ATTACHMENT0
                      : (GLenum)src;
  glReadBuffer(mapped);
}

void fv_webgl_blit_framebuffer(int32_t ctx_id, int32_t sx0, int32_t sy0, int32_t sx1, int32_t sy1,
                               int32_t dx0, int32_t dy0, int32_t dx1, int32_t dy1,
                               uint32_t mask, uint32_t filter) {
  FvWebGLCtx *c = NULL;
  if (!make_current(ctx_id, &c)) return;
  glBlitFramebuffer((GLint)sx0, (GLint)sy0, (GLint)sx1, (GLint)sy1,
                    (GLint)dx0, (GLint)dy0, (GLint)dx1, (GLint)dy1,
                    (GLbitfield)mask, (GLenum)filter);
  if (c->bound_draw_fbo == 0) c->dirty = 1;
}

uint32_t fv_webgl_create_renderbuffer(int32_t ctx_id) {
  if (!make_current(ctx_id, NULL)) return 0;
  GLuint r = 0;
  glGenRenderbuffers(1, &r);
  return (uint32_t)r;
}

void fv_webgl_bind_renderbuffer(int32_t ctx_id, uint32_t target, uint32_t rbo) {
  if (!make_current(ctx_id, NULL)) return;
  glBindRenderbuffer((GLenum)target, (GLuint)rbo);
}

void fv_webgl_renderbuffer_storage(int32_t ctx_id, uint32_t target, uint32_t internalformat,
                                   int32_t width, int32_t height) {
  if (!make_current(ctx_id, NULL)) return;
  if (internalformat == GL_DEPTH_STENCIL) internalformat = GL_DEPTH24_STENCIL8;
  else if (internalformat == FV_GL_RGBA4 || internalformat == FV_GL_RGB5_A1 || internalformat == FV_GL_RGB565) {
    internalformat = GL_RGBA8;
  }
  glRenderbufferStorage((GLenum)target, (GLenum)internalformat, (GLsizei)width, (GLsizei)height);
}

void fv_webgl_renderbuffer_storage_multisample(int32_t ctx_id, uint32_t target, int32_t samples,
                                               uint32_t internalformat, int32_t width, int32_t height) {
  if (!make_current(ctx_id, NULL)) return;
  if (internalformat == GL_DEPTH_STENCIL) internalformat = GL_DEPTH24_STENCIL8;
  else if (internalformat == FV_GL_RGBA4 || internalformat == FV_GL_RGB5_A1 || internalformat == FV_GL_RGB565) {
    internalformat = GL_RGBA8;
  }
  if (samples < 0) samples = 0;
  GLint max_samples = 4;
  glGetIntegerv(0x8D57 /* GL_MAX_SAMPLES */, &max_samples);
  if (max_samples > 0 && samples > max_samples) samples = (int32_t)max_samples;
  glRenderbufferStorageMultisample((GLenum)target, (GLsizei)samples, (GLenum)internalformat,
                                   (GLsizei)width, (GLsizei)height);
}

void fv_webgl_delete_renderbuffer(int32_t ctx_id, uint32_t rbo) {
  if (!make_current(ctx_id, NULL) || !rbo) return;
  GLuint r = (GLuint)rbo;
  glDeleteRenderbuffers(1, &r);
}

/* --- State & Draw --- */

void fv_webgl_viewport(int32_t ctx_id, int32_t x, int32_t y, int32_t w, int32_t h) {
  if (!make_current(ctx_id, NULL)) return;
  glViewport((GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h);
}

void fv_webgl_scissor(int32_t ctx_id, int32_t x, int32_t y, int32_t w, int32_t h) {
  if (!make_current(ctx_id, NULL)) return;
  glScissor((GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h);
}

void fv_webgl_clear_color(int32_t ctx_id, float r, float g, float b, float a) {
  if (!make_current(ctx_id, NULL)) return;
  glClearColor(r, g, b, a);
}

void fv_webgl_clear_depth(int32_t ctx_id, float d) {
  if (!make_current(ctx_id, NULL)) return;
  glClearDepth((GLdouble)d);
}

void fv_webgl_clear_stencil(int32_t ctx_id, int32_t s) {
  if (!make_current(ctx_id, NULL)) return;
  glClearStencil((GLint)s);
}

void fv_webgl_clear(int32_t ctx_id, uint32_t mask) {
  FvWebGLCtx *c = NULL;
  if (!make_current(ctx_id, &c)) return;
  glClear((GLbitfield)mask);
  if (c->bound_draw_fbo == 0) c->dirty = 1;
}

void fv_webgl_clear_buffer_fv(int32_t ctx_id, uint32_t buffer, int32_t drawbuffer, const float *vals) {
  FvWebGLCtx *c = NULL;
  if (!make_current(ctx_id, &c) || !vals) return;
  glClearBufferfv((GLenum)buffer, (GLint)drawbuffer, (const GLfloat *)vals);
  if (c->bound_draw_fbo == 0) c->dirty = 1;
}

void fv_webgl_clear_buffer_iv(int32_t ctx_id, uint32_t buffer, int32_t drawbuffer, const int32_t *vals) {
  FvWebGLCtx *c = NULL;
  if (!make_current(ctx_id, &c) || !vals) return;
  glClearBufferiv((GLenum)buffer, (GLint)drawbuffer, (const GLint *)vals);
  if (c->bound_draw_fbo == 0) c->dirty = 1;
}

void fv_webgl_clear_buffer_uiv(int32_t ctx_id, uint32_t buffer, int32_t drawbuffer, const uint32_t *vals) {
  FvWebGLCtx *c = NULL;
  if (!make_current(ctx_id, &c) || !vals) return;
  glClearBufferuiv((GLenum)buffer, (GLint)drawbuffer, (const GLuint *)vals);
  if (c->bound_draw_fbo == 0) c->dirty = 1;
}

void fv_webgl_clear_buffer_fi(int32_t ctx_id, uint32_t buffer, int32_t drawbuffer, float depth, int32_t stencil) {
  FvWebGLCtx *c = NULL;
  if (!make_current(ctx_id, &c)) return;
  glClearBufferfi((GLenum)buffer, (GLint)drawbuffer, (GLfloat)depth, (GLint)stencil);
  if (c->bound_draw_fbo == 0) c->dirty = 1;
}

void fv_webgl_enable(int32_t ctx_id, uint32_t cap) {
  if (!make_current(ctx_id, NULL)) return;
  glEnable((GLenum)cap);
}

void fv_webgl_disable(int32_t ctx_id, uint32_t cap) {
  if (!make_current(ctx_id, NULL)) return;
  glDisable((GLenum)cap);
}

int32_t fv_webgl_is_enabled(int32_t ctx_id, uint32_t cap) {
  if (!make_current(ctx_id, NULL)) return 0;
  return glIsEnabled((GLenum)cap) == GL_TRUE ? 1 : 0;
}

void fv_webgl_depth_func(int32_t ctx_id, uint32_t func) {
  if (!make_current(ctx_id, NULL)) return;
  glDepthFunc((GLenum)func);
}

void fv_webgl_depth_mask(int32_t ctx_id, int32_t flag) {
  if (!make_current(ctx_id, NULL)) return;
  glDepthMask(flag ? GL_TRUE : GL_FALSE);
}

void fv_webgl_depth_range(int32_t ctx_id, float znear, float zfar) {
  if (!make_current(ctx_id, NULL)) return;
  glDepthRange((GLdouble)znear, (GLdouble)zfar);
}

void fv_webgl_color_mask(int32_t ctx_id, int32_t r, int32_t g, int32_t b, int32_t a) {
  if (!make_current(ctx_id, NULL)) return;
  glColorMask(r ? GL_TRUE : GL_FALSE, g ? GL_TRUE : GL_FALSE,
              b ? GL_TRUE : GL_FALSE, a ? GL_TRUE : GL_FALSE);
}

void fv_webgl_cull_face(int32_t ctx_id, uint32_t mode) {
  if (!make_current(ctx_id, NULL)) return;
  glCullFace((GLenum)mode);
}

void fv_webgl_front_face(int32_t ctx_id, uint32_t mode) {
  if (!make_current(ctx_id, NULL)) return;
  glFrontFace((GLenum)mode);
}

void fv_webgl_blend_func(int32_t ctx_id, uint32_t sfactor, uint32_t dfactor) {
  if (!make_current(ctx_id, NULL)) return;
  glBlendFunc((GLenum)sfactor, (GLenum)dfactor);
}

void fv_webgl_blend_func_separate(int32_t ctx_id, uint32_t src_rgb, uint32_t dst_rgb,
                                  uint32_t src_a, uint32_t dst_a) {
  if (!make_current(ctx_id, NULL)) return;
  glBlendFuncSeparate((GLenum)src_rgb, (GLenum)dst_rgb, (GLenum)src_a, (GLenum)dst_a);
}

void fv_webgl_blend_equation(int32_t ctx_id, uint32_t mode) {
  if (!make_current(ctx_id, NULL)) return;
  glBlendEquation((GLenum)mode);
}

void fv_webgl_blend_equation_separate(int32_t ctx_id, uint32_t mode_rgb, uint32_t mode_a) {
  if (!make_current(ctx_id, NULL)) return;
  glBlendEquationSeparate((GLenum)mode_rgb, (GLenum)mode_a);
}

void fv_webgl_blend_color(int32_t ctx_id, float r, float g, float b, float a) {
  if (!make_current(ctx_id, NULL)) return;
  glBlendColor(r, g, b, a);
}

void fv_webgl_line_width(int32_t ctx_id, float width) {
  if (!make_current(ctx_id, NULL)) return;
#ifdef __APPLE__
  if (width > 1.0f) width = 1.0f;
  if (width <= 0.0f) width = 1.0f;
#endif
  glLineWidth(width);
}

void fv_webgl_polygon_offset(int32_t ctx_id, float factor, float units) {
  if (!make_current(ctx_id, NULL)) return;
  glPolygonOffset(factor, units);
}

void fv_webgl_stencil_func(int32_t ctx_id, uint32_t func, int32_t ref, uint32_t mask) {
  if (!make_current(ctx_id, NULL)) return;
  glStencilFunc((GLenum)func, (GLint)ref, (GLuint)mask);
}

void fv_webgl_stencil_func_separate(int32_t ctx_id, uint32_t face, uint32_t func, int32_t ref, uint32_t mask) {
  if (!make_current(ctx_id, NULL)) return;
  glStencilFuncSeparate((GLenum)face, (GLenum)func, (GLint)ref, (GLuint)mask);
}

void fv_webgl_stencil_op(int32_t ctx_id, uint32_t sfail, uint32_t dpfail, uint32_t dppass) {
  if (!make_current(ctx_id, NULL)) return;
  glStencilOp((GLenum)sfail, (GLenum)dpfail, (GLenum)dppass);
}

void fv_webgl_stencil_op_separate(int32_t ctx_id, uint32_t face, uint32_t sfail, uint32_t dpfail, uint32_t dppass) {
  if (!make_current(ctx_id, NULL)) return;
  glStencilOpSeparate((GLenum)face, (GLenum)sfail, (GLenum)dpfail, (GLenum)dppass);
}

void fv_webgl_stencil_mask(int32_t ctx_id, uint32_t mask) {
  if (!make_current(ctx_id, NULL)) return;
  glStencilMask((GLuint)mask);
}

void fv_webgl_stencil_mask_separate(int32_t ctx_id, uint32_t face, uint32_t mask) {
  if (!make_current(ctx_id, NULL)) return;
  glStencilMaskSeparate((GLenum)face, (GLuint)mask);
}

void fv_webgl_draw_arrays(int32_t ctx_id, uint32_t mode, int32_t first, int32_t count) {
  FvWebGLCtx *c = NULL;
  if (!make_current(ctx_id, &c) || count <= 0) return;
  glDrawArrays((GLenum)mode, (GLint)first, (GLsizei)count);
  if (c->bound_draw_fbo == 0) c->dirty = 1;
}

void fv_webgl_draw_elements(int32_t ctx_id, uint32_t mode, int32_t count, uint32_t type, int32_t offset) {
  FvWebGLCtx *c = NULL;
  if (!make_current(ctx_id, &c) || count <= 0) return;
  glDrawElements((GLenum)mode, (GLsizei)count, (GLenum)type, (const void *)(uintptr_t)(offset > 0 ? offset : 0));
  if (c->bound_draw_fbo == 0) c->dirty = 1;
}

void fv_webgl_draw_arrays_instanced(int32_t ctx_id, uint32_t mode, int32_t first, int32_t count, int32_t primcount) {
  FvWebGLCtx *c = NULL;
  if (!make_current(ctx_id, &c) || count <= 0 || primcount <= 0) return;
  glDrawArraysInstanced((GLenum)mode, (GLint)first, (GLsizei)count, (GLsizei)primcount);
  if (c->bound_draw_fbo == 0) c->dirty = 1;
}

void fv_webgl_draw_elements_instanced(int32_t ctx_id, uint32_t mode, int32_t count, uint32_t type,
                                      int32_t offset, int32_t primcount) {
  FvWebGLCtx *c = NULL;
  if (!make_current(ctx_id, &c) || count <= 0 || primcount <= 0) return;
  glDrawElementsInstanced((GLenum)mode, (GLsizei)count, (GLenum)type,
                          (const void *)(uintptr_t)(offset > 0 ? offset : 0), (GLsizei)primcount);
  if (c->bound_draw_fbo == 0) c->dirty = 1;
}

void fv_webgl_finish(int32_t ctx_id) {
  if (!make_current(ctx_id, NULL)) return;
  glFinish();
}

void fv_webgl_flush(int32_t ctx_id) {
  if (!make_current(ctx_id, NULL)) return;
  glFlush();
}

uint32_t fv_webgl_get_error(int32_t ctx_id) {
  if (!make_current(ctx_id, NULL)) return GL_NO_ERROR;
  return (uint32_t)glGetError();
}

int32_t fv_webgl_get_string(int32_t ctx_id, uint32_t pname, char *out_buf, int32_t max_len) {
  if (!make_current(ctx_id, NULL) || !out_buf || max_len <= 0) return 0;
  const char *s = (const char *)glGetString((GLenum)pname);
  if (!s) {
    out_buf[0] = '\0';
    return 0;
  }
  size_t n = strlen(s);
  if (n >= (size_t)max_len) n = (size_t)max_len - 1;
  memcpy(out_buf, s, n);
  out_buf[n] = '\0';
  return (int32_t)n;
}

int32_t fv_webgl_get_integerv(int32_t ctx_id, uint32_t pname, int32_t *out_vals, int32_t max_count) {
  FvWebGLCtx *c = NULL;
  if (!make_current(ctx_id, &c) || !out_vals || max_count <= 0) return 0;
  if (pname == GL_FRAMEBUFFER_BINDING) {
    out_vals[0] = (int32_t)c->bound_draw_fbo;
    return 1;
  }
  if (pname == GL_VERTEX_ARRAY_BINDING) {
    out_vals[0] = (int32_t)c->bound_vao;
    return 1;
  }
  if (pname == FV_GL_UNPACK_FLIP_Y_WEBGL) {
    out_vals[0] = c->unpack_flip_y;
    return 1;
  }
  if (pname == FV_GL_UNPACK_PREMULTIPLY_ALPHA_WEBGL) {
    out_vals[0] = c->unpack_premultiply_alpha;
    return 1;
  }
  int32_t cnt = 1;
  if (pname == GL_VIEWPORT || pname == GL_SCISSOR_BOX || pname == GL_COLOR_WRITEMASK) cnt = 4;
  else if (pname == GL_MAX_VIEWPORT_DIMS) cnt = 2;
  if (cnt > max_count) cnt = max_count;
  GLint tmp[4] = {0, 0, 0, 0};
  glGetIntegerv((GLenum)pname, tmp);
  for (int32_t i = 0; i < cnt; i++) out_vals[i] = (int32_t)tmp[i];
  return cnt;
}

int32_t fv_webgl_get_floatv(int32_t ctx_id, uint32_t pname, float *out_vals, int32_t max_count) {
  if (!make_current(ctx_id, NULL) || !out_vals || max_count <= 0) return 0;
  int32_t cnt = 1;
  if (pname == GL_COLOR_CLEAR_VALUE || pname == GL_BLEND_COLOR) cnt = 4;
  else if (pname == GL_ALIASED_LINE_WIDTH_RANGE || pname == FV_GL_ALIASED_POINT_SIZE_RANGE || pname == GL_DEPTH_RANGE) cnt = 2;
  if (cnt > max_count) cnt = max_count;
  GLfloat tmp[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  glGetFloatv((GLenum)pname, tmp);
  for (int32_t i = 0; i < cnt; i++) out_vals[i] = (float)tmp[i];
  return cnt;
}

/* --- Readback --- */

int32_t fv_webgl_read_pixels(int32_t ctx_id, int32_t x, int32_t y, int32_t w, int32_t h,
                             uint32_t format, uint32_t type, void *out_buf, int32_t buf_len) {
  FvWebGLCtx *c = NULL;
  if (!make_current(ctx_id, &c) || !out_buf || w <= 0 || h <= 0) return 0;
  int32_t ch = format_channels(format);
  int32_t bpc = type_bytes_per_comp(type);
  int64_t need = (int64_t)w * (int64_t)h * (int64_t)ch * (int64_t)bpc;
  if (need <= 0 || need > (int64_t)buf_len) return 0;
  glFinish();
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glReadPixels((GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h, (GLenum)format, (GLenum)type, out_buf);
  glPixelStorei(GL_PACK_ALIGNMENT, c->pack_alignment);
  return (int32_t)need;
}

int32_t fv_webgl_read_rgba_scaled(int32_t ctx_id, int32_t max_dim, int32_t keep_premul,
                                  void *out_buf, int32_t buf_len) {
  FvWebGLCtx *c = NULL;
  if (!make_current(ctx_id, &c) || !out_buf) return 0;
  int32_t w = c->width;
  int32_t h = c->height;
  if (w <= 0 || h <= 0) return 0;
  int32_t dw = w;
  int32_t dh = h;
  if (max_dim > 0 && (w > max_dim || h > max_dim)) {
    if (w >= h) {
      dw = max_dim;
      dh = (int32_t)(((int64_t)h * (int64_t)max_dim + w / 2) / w);
      if (dh < 1) dh = 1;
    } else {
      dh = max_dim;
      dw = (int32_t)(((int64_t)w * (int64_t)max_dim + h / 2) / h);
      if (dw < 1) dw = 1;
    }
  }
  int64_t need = (int64_t)dw * (int64_t)dh * 4;
  if (need <= 0 || need > (int64_t)buf_len) return 0;

  GLuint prev_read = c->bound_read_fbo;
  GLuint prev_draw = c->bound_draw_fbo;
  GLuint read_target = c->default_fbo;
  /* When keep_premul != 0 (live MBRAWPRE streaming) or downscaling, blit on GPU with inverted Y
   * so both linear downscale and vertical flip execute in hardware (0 CPU row memcpy). */
  int32_t use_blit = (dw < w || dh < h || keep_premul != 0);
  int32_t gpu_flipped = 0;

  if (use_blit) {
    GLint prev_tex = 0;
    glGetIntegerv(0x8069 /* GL_TEXTURE_BINDING_2D */, &prev_tex);
    if (!c->blit_fbo) glGenFramebuffers(1, &c->blit_fbo);
    if (!c->blit_tex) glGenTextures(1, &c->blit_tex);
    if (c->blit_w != dw || c->blit_h != dh) {
      glBindTexture(GL_TEXTURE_2D, c->blit_tex);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)dw, (GLsizei)dh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, c->blit_fbo);
      glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->blit_tex, 0);
      c->blit_w = dw;
      c->blit_h = dh;
    } else {
      glBindFramebuffer(GL_DRAW_FRAMEBUFFER, c->blit_fbo);
    }
    glBindTexture(GL_TEXTURE_2D, (GLuint)prev_tex);
    GLboolean was_scissor = glIsEnabled(0x0C11 /* GL_SCISSOR_TEST */);
    if (was_scissor) glDisable(0x0C11);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, c->default_fbo);
    glBlitFramebuffer(0, (GLint)h, (GLint)w, 0, 0, 0, (GLint)dw, (GLint)dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    if (was_scissor) glEnable(0x0C11);
    read_target = c->blit_fbo;
    gpu_flipped = 1;
  }

  glBindFramebuffer(GL_READ_FRAMEBUFFER, read_target);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);

  int32_t read_via_pbo = 0;
  if (keep_premul >= 2) {
    GLint prev_pbo = 0;
    glGetIntegerv(0x88ED /* GL_PIXEL_PACK_BUFFER_BINDING */, &prev_pbo);
    if (!c->pbo[0] || !c->pbo[1]) {
      glGenBuffers(2, c->pbo);
    }
    if (c->pbo[0] && c->pbo[1]) {
      if (c->pbo_w != dw || c->pbo_h != dh) {
        glBindBuffer(GL_PIXEL_PACK_BUFFER, c->pbo[0]);
        glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)need, NULL, GL_STREAM_READ);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, c->pbo[1]);
        glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)need, NULL, GL_STREAM_READ);
        c->pbo_w = dw;
        c->pbo_h = dh;
        c->pbo_idx = 0;
        c->pbo_primed = 0;
      }
      int32_t cur = c->pbo_idx & 1;
      int32_t prev = 1 - cur;
      glBindBuffer(GL_PIXEL_PACK_BUFFER, c->pbo[cur]);
      glReadPixels(0, 0, (GLsizei)dw, (GLsizei)dh, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
      int32_t map_idx = c->pbo_primed ? prev : cur;
      glBindBuffer(GL_PIXEL_PACK_BUFFER, c->pbo[map_idx]);
      void *mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)need, GL_MAP_READ_BIT);
      if (mapped) {
        memcpy(out_buf, mapped, (size_t)need);
        glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        read_via_pbo = 1;
        c->pbo_primed = 1;
        c->pbo_idx = prev;
      }
      glBindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)prev_pbo);
    }
  }
  if (!read_via_pbo) {
    glFinish();
    glReadPixels(0, 0, (GLsizei)dw, (GLsizei)dh, GL_RGBA, GL_UNSIGNED_BYTE, out_buf);
  }
  glPixelStorei(GL_PACK_ALIGNMENT, c->pack_alignment);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, prev_read ? prev_read : c->default_fbo);
  if (use_blit) {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, prev_draw ? prev_draw : c->default_fbo);
  }

  uint8_t *base = (uint8_t *)out_buf;
  if (!gpu_flipped) {
    size_t row_bytes = (size_t)dw * 4;
    uint8_t *row_tmp = (uint8_t *)malloc(row_bytes);
    if (row_tmp) {
      for (int32_t y = 0; y < dh / 2; y++) {
        uint8_t *top = base + (size_t)y * row_bytes;
        uint8_t *bot = base + (size_t)(dh - 1 - y) * row_bytes;
        memcpy(row_tmp, top, row_bytes);
        memcpy(top, bot, row_bytes);
        memcpy(bot, row_tmp, row_bytes);
      }
      free(row_tmp);
    }
  }
  if (!keep_premul) {
    for (size_t i = 0; i < (size_t)need; i += 4) {
      uint32_t a = base[i + 3];
      if (a > 0 && a < 255) {
        uint32_t r = ((uint32_t)base[i + 0] * 255u + (a >> 1)) / a;
        uint32_t g = ((uint32_t)base[i + 1] * 255u + (a >> 1)) / a;
        uint32_t b = ((uint32_t)base[i + 2] * 255u + (a >> 1)) / a;
        base[i + 0] = (uint8_t)(r > 255u ? 255u : r);
        base[i + 1] = (uint8_t)(g > 255u ? 255u : g);
        base[i + 2] = (uint8_t)(b > 255u ? 255u : b);
      }
    }
  }
  return (int32_t)need;
}

int32_t fv_webgl_read_rgba_top_down(int32_t ctx_id, void *out_buf, int32_t buf_len) {
  return fv_webgl_read_rgba_scaled(ctx_id, 0, 0, out_buf, buf_len);
}

static inline int32_t fv_rd_i32(const uint8_t *p) {
  int32_t v = 0;
  memcpy(&v, p, 4);
  return v;
}

static inline uint32_t fv_rd_u32(const uint8_t *p) {
  uint32_t v = 0;
  memcpy(&v, p, 4);
  return v;
}

static inline float fv_rd_f32(const uint8_t *p) {
  float v = 0.0f;
  memcpy(&v, p, 4);
  return v;
}

/* Fast binary WebGL command buffer executor (zero JSON, single make_current per batch). */
int32_t fv_webgl_exec_batch(int32_t ctx_id, const uint8_t *buf, int32_t byte_len) {
  FvWebGLCtx *c = NULL;
  if (!buf || byte_len < 4 || !make_current(ctx_id, &c)) return 0;
  int32_t pos = 0;
  int32_t dirty_hit = 0;
  while (pos + 4 <= byte_len) {
    int32_t op = fv_rd_i32(buf + pos);
    pos += 4;
    switch (op) {
      case 1: /* useProgram(p) */
        if (pos + 4 > byte_len) return dirty_hit;
        glUseProgram((GLuint)fv_rd_u32(buf + pos));
        pos += 4;
        break;
      case 2: /* bindBuffer(t, b) */
        if (pos + 8 > byte_len) return dirty_hit;
        glBindBuffer((GLenum)fv_rd_u32(buf + pos), (GLuint)fv_rd_u32(buf + pos + 4));
        pos += 8;
        break;
      case 3: /* bindVertexArray(v) */
        if (pos + 4 > byte_len) return dirty_hit;
        {
          uint32_t vao = fv_rd_u32(buf + pos);
          c->bound_vao = (GLuint)vao;
          glBindVertexArray(vao ? (GLuint)vao : c->default_vao);
        }
        pos += 4;
        break;
      case 4: /* bindFramebuffer(t, f) */
        if (pos + 8 > byte_len) return dirty_hit;
        {
          uint32_t t = fv_rd_u32(buf + pos);
          uint32_t fb = fv_rd_u32(buf + pos + 4);
          if (t == GL_FRAMEBUFFER) { c->bound_draw_fbo = (GLuint)fb; c->bound_read_fbo = (GLuint)fb; }
          else if (t == GL_DRAW_FRAMEBUFFER) { c->bound_draw_fbo = (GLuint)fb; }
          else if (t == GL_READ_FRAMEBUFFER) { c->bound_read_fbo = (GLuint)fb; }
          glBindFramebuffer((GLenum)t, fb ? (GLuint)fb : c->default_fbo);
        }
        pos += 8;
        break;
      case 5: /* bindRenderbuffer(t, r) */
        if (pos + 8 > byte_len) return dirty_hit;
        glBindRenderbuffer((GLenum)fv_rd_u32(buf + pos), (GLuint)fv_rd_u32(buf + pos + 4));
        pos += 8;
        break;
      case 6: /* activeTexture(u) */
        if (pos + 4 > byte_len) return dirty_hit;
        glActiveTexture((GLenum)fv_rd_u32(buf + pos));
        pos += 4;
        break;
      case 7: /* bindTexture(t, tx) */
        if (pos + 8 > byte_len) return dirty_hit;
        glBindTexture((GLenum)fv_rd_u32(buf + pos), (GLuint)fv_rd_u32(buf + pos + 4));
        pos += 8;
        break;
      case 8: /* viewport(x, y, w, h) */
        if (pos + 16 > byte_len) return dirty_hit;
        glViewport(fv_rd_i32(buf + pos), fv_rd_i32(buf + pos + 4),
                   fv_rd_i32(buf + pos + 8), fv_rd_i32(buf + pos + 12));
        pos += 16;
        break;
      case 9: /* scissor(x, y, w, h) */
        if (pos + 16 > byte_len) return dirty_hit;
        glScissor(fv_rd_i32(buf + pos), fv_rd_i32(buf + pos + 4),
                  fv_rd_i32(buf + pos + 8), fv_rd_i32(buf + pos + 12));
        pos += 16;
        break;
      case 10: /* enable(cap) */
        if (pos + 4 > byte_len) return dirty_hit;
        glEnable((GLenum)fv_rd_u32(buf + pos));
        pos += 4;
        break;
      case 11: /* disable(cap) */
        if (pos + 4 > byte_len) return dirty_hit;
        glDisable((GLenum)fv_rd_u32(buf + pos));
        pos += 4;
        break;
      case 12: /* clear(mask) */
        if (pos + 4 > byte_len) return dirty_hit;
        glClear((GLbitfield)fv_rd_u32(buf + pos));
        if (c->bound_draw_fbo == 0) { c->dirty = 1; dirty_hit = 1; }
        pos += 4;
        break;
      case 13: /* clearColor(r, g, b, a) */
        if (pos + 16 > byte_len) return dirty_hit;
        glClearColor(fv_rd_f32(buf + pos), fv_rd_f32(buf + pos + 4),
                     fv_rd_f32(buf + pos + 8), fv_rd_f32(buf + pos + 12));
        pos += 16;
        break;
      case 14: /* clearDepth(d) */
        if (pos + 4 > byte_len) return dirty_hit;
        glClearDepth((GLdouble)fv_rd_f32(buf + pos));
        pos += 4;
        break;
      case 15: /* clearStencil(s) */
        if (pos + 4 > byte_len) return dirty_hit;
        glClearStencil(fv_rd_i32(buf + pos));
        pos += 4;
        break;
      case 16: /* colorMask(r, g, b, a) */
        if (pos + 16 > byte_len) return dirty_hit;
        glColorMask(fv_rd_i32(buf + pos) ? GL_TRUE : GL_FALSE,
                    fv_rd_i32(buf + pos + 4) ? GL_TRUE : GL_FALSE,
                    fv_rd_i32(buf + pos + 8) ? GL_TRUE : GL_FALSE,
                    fv_rd_i32(buf + pos + 12) ? GL_TRUE : GL_FALSE);
        pos += 16;
        break;
      case 17: /* depthMask(f) */
        if (pos + 4 > byte_len) return dirty_hit;
        glDepthMask(fv_rd_i32(buf + pos) ? GL_TRUE : GL_FALSE);
        pos += 4;
        break;
      case 18: /* depthFunc(f) */
        if (pos + 4 > byte_len) return dirty_hit;
        glDepthFunc((GLenum)fv_rd_u32(buf + pos));
        pos += 4;
        break;
      case 19: /* depthRange(n, f) */
        if (pos + 8 > byte_len) return dirty_hit;
        glDepthRange((GLdouble)fv_rd_f32(buf + pos), (GLdouble)fv_rd_f32(buf + pos + 4));
        pos += 8;
        break;
      case 20: /* cullFace(m) */
        if (pos + 4 > byte_len) return dirty_hit;
        glCullFace((GLenum)fv_rd_u32(buf + pos));
        pos += 4;
        break;
      case 21: /* frontFace(m) */
        if (pos + 4 > byte_len) return dirty_hit;
        glFrontFace((GLenum)fv_rd_u32(buf + pos));
        pos += 4;
        break;
      case 22: /* blendFunc(s, d) */
        if (pos + 8 > byte_len) return dirty_hit;
        glBlendFunc((GLenum)fv_rd_u32(buf + pos), (GLenum)fv_rd_u32(buf + pos + 4));
        pos += 8;
        break;
      case 23: /* blendFuncSeparate(sr, dr, sa, da) */
        if (pos + 16 > byte_len) return dirty_hit;
        glBlendFuncSeparate((GLenum)fv_rd_u32(buf + pos), (GLenum)fv_rd_u32(buf + pos + 4),
                            (GLenum)fv_rd_u32(buf + pos + 8), (GLenum)fv_rd_u32(buf + pos + 12));
        pos += 16;
        break;
      case 24: /* blendEquation(m) */
        if (pos + 4 > byte_len) return dirty_hit;
        glBlendEquation((GLenum)fv_rd_u32(buf + pos));
        pos += 4;
        break;
      case 25: /* blendEquationSeparate(mr, ma) */
        if (pos + 8 > byte_len) return dirty_hit;
        glBlendEquationSeparate((GLenum)fv_rd_u32(buf + pos), (GLenum)fv_rd_u32(buf + pos + 4));
        pos += 8;
        break;
      case 26: /* blendColor(r, g, b, a) */
        if (pos + 16 > byte_len) return dirty_hit;
        glBlendColor(fv_rd_f32(buf + pos), fv_rd_f32(buf + pos + 4),
                     fv_rd_f32(buf + pos + 8), fv_rd_f32(buf + pos + 12));
        pos += 16;
        break;
      case 27: /* stencilFunc(f, r, m) */
        if (pos + 12 > byte_len) return dirty_hit;
        glStencilFunc((GLenum)fv_rd_u32(buf + pos), fv_rd_i32(buf + pos + 4), (GLuint)fv_rd_u32(buf + pos + 8));
        pos += 12;
        break;
      case 28: /* stencilFuncSeparate(fc, f, r, m) */
        if (pos + 16 > byte_len) return dirty_hit;
        glStencilFuncSeparate((GLenum)fv_rd_u32(buf + pos), (GLenum)fv_rd_u32(buf + pos + 4),
                              fv_rd_i32(buf + pos + 8), (GLuint)fv_rd_u32(buf + pos + 12));
        pos += 16;
        break;
      case 29: /* stencilOp(sf, df, dp) */
        if (pos + 12 > byte_len) return dirty_hit;
        glStencilOp((GLenum)fv_rd_u32(buf + pos), (GLenum)fv_rd_u32(buf + pos + 4), (GLenum)fv_rd_u32(buf + pos + 8));
        pos += 12;
        break;
      case 30: /* stencilOpSeparate(fc, sf, df, dp) */
        if (pos + 16 > byte_len) return dirty_hit;
        glStencilOpSeparate((GLenum)fv_rd_u32(buf + pos), (GLenum)fv_rd_u32(buf + pos + 4),
                            (GLenum)fv_rd_u32(buf + pos + 8), (GLenum)fv_rd_u32(buf + pos + 12));
        pos += 16;
        break;
      case 31: /* stencilMask(m) */
        if (pos + 4 > byte_len) return dirty_hit;
        glStencilMask((GLuint)fv_rd_u32(buf + pos));
        pos += 4;
        break;
      case 32: /* stencilMaskSeparate(fc, m) */
        if (pos + 8 > byte_len) return dirty_hit;
        glStencilMaskSeparate((GLenum)fv_rd_u32(buf + pos), (GLuint)fv_rd_u32(buf + pos + 4));
        pos += 8;
        break;
      case 33: /* polygonOffset(f, u) */
        if (pos + 8 > byte_len) return dirty_hit;
        glPolygonOffset(fv_rd_f32(buf + pos), fv_rd_f32(buf + pos + 4));
        pos += 8;
        break;
      case 34: /* lineWidth(w) */
        if (pos + 4 > byte_len) return dirty_hit;
        {
          float lw = fv_rd_f32(buf + pos);
#ifdef __APPLE__
          if (lw > 1.0f || lw <= 0.0f) lw = 1.0f;
#endif
          glLineWidth(lw);
        }
        pos += 4;
        break;
      case 35: /* pixelStorei(p, v) */
        if (pos + 8 > byte_len) return dirty_hit;
        fv_webgl_pixel_storei(ctx_id, fv_rd_u32(buf + pos), fv_rd_i32(buf + pos + 4));
        pos += 8;
        break;
      case 36: /* enableVertexAttribArray(i) */
        if (pos + 4 > byte_len) return dirty_hit;
        glEnableVertexAttribArray((GLuint)fv_rd_u32(buf + pos));
        pos += 4;
        break;
      case 37: /* disableVertexAttribArray(i) */
        if (pos + 4 > byte_len) return dirty_hit;
        glDisableVertexAttribArray((GLuint)fv_rd_u32(buf + pos));
        pos += 4;
        break;
      case 38: /* vertexAttribPointer(i, sz, ty, norm, st, o) */
        if (pos + 24 > byte_len) return dirty_hit;
        {
          int32_t off = fv_rd_i32(buf + pos + 20);
          glVertexAttribPointer((GLuint)fv_rd_u32(buf + pos), fv_rd_i32(buf + pos + 4),
                                (GLenum)fv_rd_u32(buf + pos + 8), fv_rd_i32(buf + pos + 12) ? GL_TRUE : GL_FALSE,
                                fv_rd_i32(buf + pos + 16), (const void *)(uintptr_t)(off > 0 ? off : 0));
        }
        pos += 24;
        break;
      case 39: /* vertexAttribIPointer(i, sz, ty, st, o) */
        if (pos + 20 > byte_len) return dirty_hit;
        {
          int32_t off = fv_rd_i32(buf + pos + 16);
          glVertexAttribIPointer((GLuint)fv_rd_u32(buf + pos), fv_rd_i32(buf + pos + 4),
                                 (GLenum)fv_rd_u32(buf + pos + 8), fv_rd_i32(buf + pos + 12),
                                 (const void *)(uintptr_t)(off > 0 ? off : 0));
        }
        pos += 20;
        break;
      case 40: /* vertexAttribDivisor(i, d) */
        if (pos + 8 > byte_len) return dirty_hit;
        glVertexAttribDivisor((GLuint)fv_rd_u32(buf + pos), (GLuint)fv_rd_u32(buf + pos + 4));
        pos += 8;
        break;
      case 41: /* drawArrays(m, f, cnt) */
        if (pos + 12 > byte_len) return dirty_hit;
        {
          int32_t cnt = fv_rd_i32(buf + pos + 8);
          if (cnt > 0) {
            glDrawArrays((GLenum)fv_rd_u32(buf + pos), fv_rd_i32(buf + pos + 4), (GLsizei)cnt);
            if (c->bound_draw_fbo == 0) { c->dirty = 1; dirty_hit = 1; }
          }
        }
        pos += 12;
        break;
      case 42: /* drawElements(m, cnt, ty, o) */
        if (pos + 16 > byte_len) return dirty_hit;
        {
          int32_t cnt = fv_rd_i32(buf + pos + 4);
          int32_t off = fv_rd_i32(buf + pos + 12);
          if (cnt > 0) {
            glDrawElements((GLenum)fv_rd_u32(buf + pos), (GLsizei)cnt, (GLenum)fv_rd_u32(buf + pos + 8),
                           (const void *)(uintptr_t)(off > 0 ? off : 0));
            if (c->bound_draw_fbo == 0) { c->dirty = 1; dirty_hit = 1; }
          }
        }
        pos += 16;
        break;
      case 43: /* drawArraysInstanced(m, f, cnt, prims) */
        if (pos + 16 > byte_len) return dirty_hit;
        {
          int32_t cnt = fv_rd_i32(buf + pos + 8);
          int32_t pr = fv_rd_i32(buf + pos + 12);
          if (cnt > 0 && pr > 0) {
            glDrawArraysInstanced((GLenum)fv_rd_u32(buf + pos), fv_rd_i32(buf + pos + 4), (GLsizei)cnt, (GLsizei)pr);
            if (c->bound_draw_fbo == 0) { c->dirty = 1; dirty_hit = 1; }
          }
        }
        pos += 16;
        break;
      case 44: /* drawElementsInstanced(m, cnt, ty, o, prims) */
        if (pos + 20 > byte_len) return dirty_hit;
        {
          int32_t cnt = fv_rd_i32(buf + pos + 4);
          int32_t off = fv_rd_i32(buf + pos + 12);
          int32_t pr = fv_rd_i32(buf + pos + 16);
          if (cnt > 0 && pr > 0) {
            glDrawElementsInstanced((GLenum)fv_rd_u32(buf + pos), (GLsizei)cnt, (GLenum)fv_rd_u32(buf + pos + 8),
                                    (const void *)(uintptr_t)(off > 0 ? off : 0), (GLsizei)pr);
            if (c->bound_draw_fbo == 0) { c->dirty = 1; dirty_hit = 1; }
          }
        }
        pos += 20;
        break;
      case 45: /* uniformFv(loc, dim, nfloats, float[nfloats]) */
        if (pos + 12 > byte_len) return dirty_hit;
        {
          int32_t loc = fv_rd_i32(buf + pos);
          int32_t dim = fv_rd_i32(buf + pos + 4);
          int32_t nf = fv_rd_i32(buf + pos + 8);
          pos += 12;
          if (nf < 0 || pos + nf * 4 > byte_len) return dirty_hit;
          if (loc >= 0 && nf > 0 && dim >= 1 && dim <= 4) {
            int32_t cnt = nf / dim;
            const GLfloat *fv = (const GLfloat *)(buf + pos);
            if (cnt > 0) {
              if (dim == 1) glUniform1fv(loc, cnt, fv);
              else if (dim == 2) glUniform2fv(loc, cnt, fv);
              else if (dim == 3) glUniform3fv(loc, cnt, fv);
              else glUniform4fv(loc, cnt, fv);
            }
          }
          pos += nf * 4;
        }
        break;
      case 46: /* uniformIv(loc, dim, nints, int32[nints]) */
        if (pos + 12 > byte_len) return dirty_hit;
        {
          int32_t loc = fv_rd_i32(buf + pos);
          int32_t dim = fv_rd_i32(buf + pos + 4);
          int32_t ni = fv_rd_i32(buf + pos + 8);
          pos += 12;
          if (ni < 0 || pos + ni * 4 > byte_len) return dirty_hit;
          if (loc >= 0 && ni > 0 && dim >= 1 && dim <= 4) {
            int32_t cnt = ni / dim;
            const GLint *iv = (const GLint *)(buf + pos);
            if (cnt > 0) {
              if (dim == 1) glUniform1iv(loc, cnt, iv);
              else if (dim == 2) glUniform2iv(loc, cnt, iv);
              else if (dim == 3) glUniform3iv(loc, cnt, iv);
              else glUniform4iv(loc, cnt, iv);
            }
          }
          pos += ni * 4;
        }
        break;
      case 47: /* uniformUiv(loc, dim, nuints, uint32[nuints]) */
        if (pos + 12 > byte_len) return dirty_hit;
        {
          int32_t loc = fv_rd_i32(buf + pos);
          int32_t dim = fv_rd_i32(buf + pos + 4);
          int32_t nu = fv_rd_i32(buf + pos + 8);
          pos += 12;
          if (nu < 0 || pos + nu * 4 > byte_len) return dirty_hit;
          if (loc >= 0 && nu > 0 && dim >= 1 && dim <= 4) {
            int32_t cnt = nu / dim;
            const GLuint *uv = (const GLuint *)(buf + pos);
            if (cnt > 0) {
              if (dim == 1) glUniform1uiv(loc, cnt, uv);
              else if (dim == 2) glUniform2uiv(loc, cnt, uv);
              else if (dim == 3) glUniform3uiv(loc, cnt, uv);
              else glUniform4uiv(loc, cnt, uv);
            }
          }
          pos += nu * 4;
        }
        break;
      case 48: /* uniformMatrixFv(loc, dim, tr, nfloats, float[nfloats]) */
        if (pos + 16 > byte_len) return dirty_hit;
        {
          int32_t loc = fv_rd_i32(buf + pos);
          int32_t dim = fv_rd_i32(buf + pos + 4);
          int32_t tr = fv_rd_i32(buf + pos + 8);
          int32_t nf = fv_rd_i32(buf + pos + 12);
          pos += 16;
          if (nf < 0 || pos + nf * 4 > byte_len) return dirty_hit;
          if (loc >= 0 && nf > 0) {
            const GLfloat *fv = (const GLfloat *)(buf + pos);
            if (dim == 2 && nf >= 4) glUniformMatrix2fv(loc, nf / 4, tr ? GL_TRUE : GL_FALSE, fv);
            else if (dim == 3 && nf >= 9) glUniformMatrix3fv(loc, nf / 9, tr ? GL_TRUE : GL_FALSE, fv);
            else if (dim == 4 && nf >= 16) glUniformMatrix4fv(loc, nf / 16, tr ? GL_TRUE : GL_FALSE, fv);
          }
          pos += nf * 4;
        }
        break;
      case 49: /* texParameteri(t, p, v) */
        if (pos + 12 > byte_len) return dirty_hit;
        glTexParameteri((GLenum)fv_rd_u32(buf + pos), (GLenum)fv_rd_u32(buf + pos + 4), fv_rd_i32(buf + pos + 8));
        pos += 12;
        break;
      case 50: /* texParameterf(t, p, v) */
        if (pos + 12 > byte_len) return dirty_hit;
        glTexParameterf((GLenum)fv_rd_u32(buf + pos), (GLenum)fv_rd_u32(buf + pos + 4), fv_rd_f32(buf + pos + 8));
        pos += 12;
        break;
      case 51: /* generateMipmap(t) */
        if (pos + 4 > byte_len) return dirty_hit;
        glGenerateMipmap((GLenum)fv_rd_u32(buf + pos));
        pos += 4;
        break;
      case 52: /* bindBufferBase(t, i, b) */
        if (pos + 12 > byte_len) return dirty_hit;
        glBindBufferBase((GLenum)fv_rd_u32(buf + pos), (GLuint)fv_rd_u32(buf + pos + 4), (GLuint)fv_rd_u32(buf + pos + 8));
        pos += 12;
        break;
      case 53: /* bindBufferRange(t, i, b, o, sz) */
        if (pos + 20 > byte_len) return dirty_hit;
        glBindBufferRange((GLenum)fv_rd_u32(buf + pos), (GLuint)fv_rd_u32(buf + pos + 4),
                          (GLuint)fv_rd_u32(buf + pos + 8), (GLintptr)fv_rd_i32(buf + pos + 12),
                          (GLsizeiptr)fv_rd_i32(buf + pos + 16));
        pos += 20;
        break;
      case 54: /* uniformBlockBinding(p, bi, bb) */
        if (pos + 12 > byte_len) return dirty_hit;
        glUniformBlockBinding((GLuint)fv_rd_u32(buf + pos), (GLuint)fv_rd_u32(buf + pos + 4), (GLuint)fv_rd_u32(buf + pos + 8));
        pos += 12;
        break;
      case 55: /* bufferDataAlloc(t, sz, u) */
        if (pos + 12 > byte_len) return dirty_hit;
        {
          int32_t sz = fv_rd_i32(buf + pos + 4);
          if (sz >= 0) glBufferData((GLenum)fv_rd_u32(buf + pos), (GLsizeiptr)sz, NULL, (GLenum)fv_rd_u32(buf + pos + 8));
        }
        pos += 12;
        break;
      case 56: /* framebufferTexture2D(t, a, tt, tx, lv) */
        if (pos + 20 > byte_len) return dirty_hit;
        glFramebufferTexture2D((GLenum)fv_rd_u32(buf + pos), (GLenum)fv_rd_u32(buf + pos + 4),
                               (GLenum)fv_rd_u32(buf + pos + 8), (GLuint)fv_rd_u32(buf + pos + 12),
                               fv_rd_i32(buf + pos + 16));
        pos += 20;
        break;
      case 57: /* framebufferRenderbuffer(t, a, rt, rb) */
        if (pos + 16 > byte_len) return dirty_hit;
        glFramebufferRenderbuffer((GLenum)fv_rd_u32(buf + pos), (GLenum)fv_rd_u32(buf + pos + 4),
                                  (GLenum)fv_rd_u32(buf + pos + 8), (GLuint)fv_rd_u32(buf + pos + 12));
        pos += 16;
        break;
      case 58: /* renderbufferStorage(t, ifmt, w, h) */
        if (pos + 16 > byte_len) return dirty_hit;
        fv_webgl_renderbuffer_storage(ctx_id, fv_rd_u32(buf + pos), fv_rd_u32(buf + pos + 4),
                                      fv_rd_i32(buf + pos + 8), fv_rd_i32(buf + pos + 12));
        pos += 16;
        break;
      case 59: /* blitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter) */
        if (pos + 40 > byte_len) return dirty_hit;
        fv_webgl_blit_framebuffer(ctx_id, fv_rd_i32(buf + pos), fv_rd_i32(buf + pos + 4),
                                  fv_rd_i32(buf + pos + 8), fv_rd_i32(buf + pos + 12),
                                  fv_rd_i32(buf + pos + 16), fv_rd_i32(buf + pos + 20),
                                  fv_rd_i32(buf + pos + 24), fv_rd_i32(buf + pos + 28),
                                  fv_rd_u32(buf + pos + 32), fv_rd_u32(buf + pos + 36));
        if (c->bound_draw_fbo == 0) { c->dirty = 1; dirty_hit = 1; }
        pos += 40;
        break;
      case 60: /* drawBuffers(count, u32[count]) */
        if (pos + 4 > byte_len) return dirty_hit;
        {
          int32_t cnt = fv_rd_i32(buf + pos);
          pos += 4;
          if (cnt < 0 || pos + cnt * 4 > byte_len) return dirty_hit;
          fv_webgl_draw_buffers(ctx_id, cnt, (const uint32_t *)(buf + pos));
          pos += cnt * 4;
        }
        break;
      case 61: /* readBuffer(s) */
        if (pos + 4 > byte_len) return dirty_hit;
        fv_webgl_read_buffer(ctx_id, fv_rd_u32(buf + pos));
        pos += 4;
        break;
      case 62: /* bindSampler(u, s) */
        if (pos + 8 > byte_len) return dirty_hit;
        glBindSampler((GLuint)fv_rd_u32(buf + pos), (GLuint)fv_rd_u32(buf + pos + 4));
        pos += 8;
        break;
      case 63: /* samplerParameteri(s, p, v) */
        if (pos + 12 > byte_len) return dirty_hit;
        glSamplerParameteri((GLuint)fv_rd_u32(buf + pos), (GLenum)fv_rd_u32(buf + pos + 4), fv_rd_i32(buf + pos + 8));
        pos += 12;
        break;
      case 64: /* samplerParameterf(s, p, v) */
        if (pos + 12 > byte_len) return dirty_hit;
        glSamplerParameterf((GLuint)fv_rd_u32(buf + pos), (GLenum)fv_rd_u32(buf + pos + 4), fv_rd_f32(buf + pos + 8));
        pos += 12;
        break;
      case 65: /* vertexAttribFv(i, dim, float[dim]) */
        if (pos + 8 > byte_len) return dirty_hit;
        {
          uint32_t idx = fv_rd_u32(buf + pos);
          int32_t dim = fv_rd_i32(buf + pos + 4);
          pos += 8;
          if (dim < 1 || dim > 4 || pos + dim * 4 > byte_len) return dirty_hit;
          const GLfloat *fv = (const GLfloat *)(buf + pos);
          if (dim == 1) glVertexAttrib1fv(idx, fv);
          else if (dim == 2) glVertexAttrib2fv(idx, fv);
          else if (dim == 3) glVertexAttrib3fv(idx, fv);
          else glVertexAttrib4fv(idx, fv);
          pos += dim * 4;
        }
        break;
      case 66: /* flush() */
        glFlush();
        break;
      case 67: /* finish() */
        glFinish();
        break;
      case 68: /* bufferSubDataInline(t, off, byteLen, bytes[padded4]) */
        if (pos + 12 > byte_len) return dirty_hit;
        {
          uint32_t t = fv_rd_u32(buf + pos);
          int32_t off = fv_rd_i32(buf + pos + 4);
          int32_t blen = fv_rd_i32(buf + pos + 8);
          pos += 12;
          int32_t pad = (blen + 3) & ~3;
          if (blen < 0 || pos + pad > byte_len) return dirty_hit;
          if (blen > 0 && off >= 0) {
            glBufferSubData((GLenum)t, (GLintptr)off, (GLsizeiptr)blen, buf + pos);
          }
          pos += pad;
        }
        break;
      case 69: /* bufferDataInline(t, u, byteLen, bytes[padded4]) */
        if (pos + 12 > byte_len) return dirty_hit;
        {
          uint32_t t = fv_rd_u32(buf + pos);
          uint32_t u = fv_rd_u32(buf + pos + 4);
          int32_t blen = fv_rd_i32(buf + pos + 8);
          pos += 12;
          int32_t pad = (blen + 3) & ~3;
          if (blen < 0 || pos + pad > byte_len) return dirty_hit;
          glBufferData((GLenum)t, (GLsizeiptr)blen, blen > 0 ? (const void *)(buf + pos) : NULL, (GLenum)u);
          pos += pad;
        }
        break;
      default:
        return dirty_hit;
    }
  }
  return dirty_hit;
}

/* Zero-copy binary upload from V8 ArrayBufferView directly into OpenGL. */
int32_t fv_webgl_exec_upload(int32_t ctx_id, int32_t op, const int32_t *args,
                             const void *data, int32_t data_len) {
  if (!args || ctx_id <= 0) return 0;
  if (op == 1) {
    /* bufferData(target=args[0], usage=args[1]) */
    fv_webgl_buffer_data(ctx_id, (uint32_t)args[0], data_len, data, (uint32_t)args[1]);
    return 1;
  }
  if (op == 2) {
    /* bufferSubData(target=args[0], offset=args[1]) */
    fv_webgl_buffer_sub_data(ctx_id, (uint32_t)args[0], args[1], data_len, data);
    return 1;
  }
  if (op == 3) {
    /* texImage2D(t, lv, ifmt, w, h, border, fmt, type) */
    fv_webgl_tex_image_2d(ctx_id, (uint32_t)args[0], args[1], args[2], args[3], args[4],
                          args[5], (uint32_t)args[6], (uint32_t)args[7], data, data_len);
    return 1;
  }
  if (op == 4) {
    /* texSubImage2D(t, lv, x, y, w, h, fmt, type) */
    fv_webgl_tex_sub_image_2d(ctx_id, (uint32_t)args[0], args[1], args[2], args[3], args[4],
                              args[5], (uint32_t)args[6], (uint32_t)args[7], data, data_len);
    return 1;
  }
  return 0;
}

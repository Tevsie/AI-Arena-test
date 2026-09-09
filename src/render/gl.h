// gl.h — zero-dependency OpenGL loader (GL 1.1..3.3 core + KHR_debug + ARB_instanced_arrays).
// Every entry point is a function pointer resolved at runtime from the platform
// backend (dlopen on X11). No third-party loader library required.
//
// Type definitions are copied from the Khronos registry; enum values follow the
// core GL headers. Only the subset the engine actually uses is declared, keeping
// the surface small and auditable.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace aw {

// ---------------------------------------------------------------------------
// GL core types
// ---------------------------------------------------------------------------
using GLenum  = uint32_t;
using GLboolean = uint8_t;
using GLbitfield = uint32_t;
using GLbyte  = int8_t;
using GLshort = int16_t;
using GLint   = int32_t;
using GLsizei = int32_t;
using GLubyte = uint8_t;
using GLushort = uint16_t;
using GLuint  = uint32_t;
using GLfloat = float;
using GLclampf = float;
using GLdouble = double;
using GLchar  = char;
using GLintptr = intptr_t;
using GLsizeiptr = intptr_t;

// ---------------------------------------------------------------------------
// Enums (values from the OpenGL 3.3 core header)
// ---------------------------------------------------------------------------
enum : GLenum {
    GL_DEPTH_BUFFER_BIT   = 0x00000100,
    GL_COLOR_BUFFER_BIT   = 0x00004000,
    GL_POINTS             = 0x0000,
    GL_LINES              = 0x0001,
    GL_LINE_LOOP          = 0x0002,
    GL_TRIANGLES          = 0x0004,
    GL_TRIANGLE_STRIP     = 0x0005,
    GL_DEPTH_TEST         = 0x0B71,
    GL_CULL_FACE          = 0x0B44,
    GL_BLEND              = 0x0BE2,
    GL_SRC_ALPHA          = 0x0302,
    GL_ONE_MINUS_SRC_ALPHA = 0x0303,
    GL_BACK               = 0x0405,
    GL_FRONT              = 0x0404,
    GL_CCW                = 0x0901,
    GL_CW                 = 0x0900,
    GL_TEXTURE_2D         = 0x0DE1,
    GL_TEXTURE0            = 0x84C0,
    GL_UNSIGNED_BYTE      = 0x1401,
    GL_UNSIGNED_SHORT     = 0x1403,
    GL_UNSIGNED_INT       = 0x1405,
    GL_FLOAT              = 0x1406,
    GL_RGBA               = 0x1908,
    GL_RGBA8              = 0x8058,
    GL_RGB                = 0x1907,
    GL_DEPTH_COMPONENT    = 0x1902,
    GL_DEPTH_COMPONENT16  = 0x81A5,
    GL_DEPTH_COMPONENT24  = 0x81A6,
    GL_TEXTURE_MAG_FILTER = 0x2800,
    GL_TEXTURE_MIN_FILTER = 0x2801,
    GL_NEAREST            = 0x2600,
    GL_LINEAR             = 0x2601,
    GL_TEXTURE_WRAP_S     = 0x2802,
    GL_TEXTURE_WRAP_T     = 0x2803,
    GL_CLAMP_TO_EDGE      = 0x812F,
    GL_REPEAT             = 0x2901,
    GL_ARRAY_BUFFER       = 0x8892,
    GL_ELEMENT_ARRAY_BUFFER = 0x8893,
    GL_STREAM_DRAW        = 0x88E0,
    GL_STATIC_DRAW        = 0x88E4,
    GL_DYNAMIC_DRAW       = 0x88E8,
    GL_FRAGMENT_SHADER    = 0x8B30,
    GL_VERTEX_SHADER      = 0x8B31,
    GL_COMPILE_STATUS     = 0x8B81,
    GL_LINK_STATUS        = 0x8B82,
    GL_INFO_LOG_LENGTH    = 0x8B84,
    GL_ACTIVE_UNIFORMS    = 0x8B86,
    GL_TRIANGLES_ADJACENCY = 0x000C,  // (unused, kept for completeness)
    GL_FALSE              = 0,
    GL_TRUE               = 1,
    GL_FILL               = 0x1B02,
    GL_LINE               = 0x1B01,
    GL_FRONT_AND_BACK     = 0x0408,
    GL_LESS               = 0x0201,
    GL_LEQUAL             = 0x0203,
    GL_ALWAYS             = 0x0207,
    GL_VENDOR             = 0x1F00,
    GL_RENDERER           = 0x1F01,
    GL_VERSION            = 0x1F02,
    GL_SHADING_LANGUAGE_VERSION = 0x8B8C,
    GL_NUM_EXTENSIONS     = 0x821D,
    GL_EXTENSIONS         = 0x1F03,
    GL_UNIFORM_BUFFER     = 0x8A11,
    GL_DYNAMIC_COPY       = 0x88EA,
    // ARB_instanced_arrays
    GL_VERTEX_ATTRIB_ARRAY_DIVISOR_ARB = 0x88FE,
};

// ---------------------------------------------------------------------------
// Function pointer table
// ---------------------------------------------------------------------------
struct GL {
    // loader / core
    void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
    void (*Clear)(GLbitfield) = nullptr;
    void (*Viewport)(GLint, GLint, GLsizei, GLsizei) = nullptr;
    void (*Enable)(GLenum) = nullptr;
    void (*Disable)(GLenum) = nullptr;
    void (*DepthFunc)(GLenum) = nullptr;
    void (*DepthMask)(GLboolean) = nullptr;
    void (*CullFace)(GLenum) = nullptr;
    void (*FrontFace)(GLenum) = nullptr;
    void (*BlendFunc)(GLenum, GLenum) = nullptr;
    void (*PolygonMode)(GLenum, GLenum) = nullptr;
    void (*LineWidth)(GLfloat) = nullptr;
    void (*PointSize)(GLfloat) = nullptr;

    // buffers
    void (*GenBuffers)(GLsizei, GLuint*) = nullptr;
    void (*DeleteBuffers)(GLsizei, const GLuint*) = nullptr;
    void (*BindBuffer)(GLenum, GLuint) = nullptr;
    void (*BufferData)(GLenum, GLsizeiptr, const void*, GLenum) = nullptr;
    void (*BufferSubData)(GLenum, GLintptr, GLsizeiptr, const void*) = nullptr;

    // vertex arrays
    void (*GenVertexArrays)(GLsizei, GLuint*) = nullptr;
    void (*DeleteVertexArrays)(GLsizei, const GLuint*) = nullptr;
    void (*BindVertexArray)(GLuint) = nullptr;
    void (*EnableVertexAttribArray)(GLuint) = nullptr;
    void (*DisableVertexAttribArray)(GLuint) = nullptr;
    void (*VertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) = nullptr;
    void (*VertexAttribDivisorARB)(GLuint, GLuint) = nullptr;   // instancing
    void (*DrawArrays)(GLenum, GLint, GLsizei) = nullptr;
    void (*DrawElements)(GLenum, GLsizei, GLenum, const void*) = nullptr;
    void (*DrawArraysInstancedARB)(GLenum, GLint, GLsizei, GLsizei) = nullptr;  // instancing

    // shaders
    GLuint (*CreateShader)(GLenum) = nullptr;
    void (*DeleteShader)(GLuint) = nullptr;
    void (*ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*) = nullptr;
    void (*CompileShader)(GLuint) = nullptr;
    void (*GetShaderiv)(GLuint, GLenum, GLint*) = nullptr;
    void (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    GLuint (*CreateProgram)(void) = nullptr;
    void (*DeleteProgram)(GLuint) = nullptr;
    void (*AttachShader)(GLuint, GLuint) = nullptr;
    void (*LinkProgram)(GLuint) = nullptr;
    void (*GetProgramiv)(GLuint, GLenum, GLint*) = nullptr;
    void (*GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*) = nullptr;
    void (*UseProgram)(GLuint) = nullptr;
    GLint (*GetUniformLocation)(GLuint, const GLchar*) = nullptr;
    void (*Uniform1i)(GLint, GLint) = nullptr;
    void (*Uniform1f)(GLint, GLfloat) = nullptr;
    void (*Uniform2f)(GLint, GLfloat, GLfloat) = nullptr;
    void (*Uniform3f)(GLint, GLfloat, GLfloat, GLfloat) = nullptr;
    void (*Uniform4f)(GLint, GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
    void (*UniformMatrix4fv)(GLint, GLsizei, GLboolean, const GLfloat*) = nullptr;
    void (*UniformMatrix3fv)(GLint, GLsizei, GLboolean, const GLfloat*) = nullptr;

    // textures
    void (*GenTextures)(GLsizei, GLuint*) = nullptr;
    void (*DeleteTextures)(GLsizei, const GLuint*) = nullptr;
    void (*BindTexture)(GLenum, GLuint) = nullptr;
    void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*) = nullptr;
    void (*TexParameteri)(GLenum, GLenum, GLint) = nullptr;
    void (*ActiveTexture)(GLenum) = nullptr;

    // uniform buffers
    void (*BindBufferBase)(GLenum, GLuint, GLuint) = nullptr;
    void (*GetUniformBlockIndex)(GLuint, const GLchar*) = nullptr;
    void (*UniformBlockBinding)(GLuint, GLuint, GLuint) = nullptr;
    void (*GetIntegeri_v)(GLenum, GLuint, GLint*) = nullptr;

    // queries / strings
    const GLubyte* (*GetString)(GLenum) = nullptr;
    const GLubyte* (*GetStringi)(GLenum, GLuint) = nullptr;

    // debug (KHR_debug)
    void (*DebugMessageCallbackARB)(void (*)(GLenum, GLenum, GLuint, GLenum, GLsizei, const GLchar*, const void*), const void*) = nullptr;

    bool ready = false;
};

// Global GL table; populated by the GL context backend.
extern GL gl;

// ---------------------------------------------------------------------------
// Function resolution. `loader` is any callable `void*(const char*)`.
// Returns false if a required GL 3.3 core entry point is missing.
// ---------------------------------------------------------------------------
namespace detail {
struct Entry {
    const char* name;
    size_t offset;
};
inline const Entry* entries() {
    static const Entry kEntries[] = {
        {"glClearColor", offsetof(GL, ClearColor)},
        {"glClear", offsetof(GL, Clear)},
        {"glViewport", offsetof(GL, Viewport)},
        {"glEnable", offsetof(GL, Enable)},
        {"glDisable", offsetof(GL, Disable)},
        {"glDepthFunc", offsetof(GL, DepthFunc)},
        {"glDepthMask", offsetof(GL, DepthMask)},
        {"glCullFace", offsetof(GL, CullFace)},
        {"glFrontFace", offsetof(GL, FrontFace)},
        {"glBlendFunc", offsetof(GL, BlendFunc)},
        {"glPolygonMode", offsetof(GL, PolygonMode)},
        {"glLineWidth", offsetof(GL, LineWidth)},
        {"glPointSize", offsetof(GL, PointSize)},
        {"glGenBuffers", offsetof(GL, GenBuffers)},
        {"glDeleteBuffers", offsetof(GL, DeleteBuffers)},
        {"glBindBuffer", offsetof(GL, BindBuffer)},
        {"glBufferData", offsetof(GL, BufferData)},
        {"glBufferSubData", offsetof(GL, BufferSubData)},
        {"glGenVertexArrays", offsetof(GL, GenVertexArrays)},
        {"glDeleteVertexArrays", offsetof(GL, DeleteVertexArrays)},
        {"glBindVertexArray", offsetof(GL, BindVertexArray)},
        {"glEnableVertexAttribArray", offsetof(GL, EnableVertexAttribArray)},
        {"glDisableVertexAttribArray", offsetof(GL, DisableVertexAttribArray)},
        {"glVertexAttribPointer", offsetof(GL, VertexAttribPointer)},
        {"glVertexAttribDivisorARB", offsetof(GL, VertexAttribDivisorARB)},
        {"glDrawArrays", offsetof(GL, DrawArrays)},
        {"glDrawElements", offsetof(GL, DrawElements)},
        {"glDrawArraysInstancedARB", offsetof(GL, DrawArraysInstancedARB)},
        {"glCreateShader", offsetof(GL, CreateShader)},
        {"glDeleteShader", offsetof(GL, DeleteShader)},
        {"glShaderSource", offsetof(GL, ShaderSource)},
        {"glCompileShader", offsetof(GL, CompileShader)},
        {"glGetShaderiv", offsetof(GL, GetShaderiv)},
        {"glGetShaderInfoLog", offsetof(GL, GetShaderInfoLog)},
        {"glCreateProgram", offsetof(GL, CreateProgram)},
        {"glDeleteProgram", offsetof(GL, DeleteProgram)},
        {"glAttachShader", offsetof(GL, AttachShader)},
        {"glLinkProgram", offsetof(GL, LinkProgram)},
        {"glGetProgramiv", offsetof(GL, GetProgramiv)},
        {"glGetProgramInfoLog", offsetof(GL, GetProgramInfoLog)},
        {"glUseProgram", offsetof(GL, UseProgram)},
        {"glGetUniformLocation", offsetof(GL, GetUniformLocation)},
        {"glUniform1i", offsetof(GL, Uniform1i)},
        {"glUniform1f", offsetof(GL, Uniform1f)},
        {"glUniform2f", offsetof(GL, Uniform2f)},
        {"glUniform3f", offsetof(GL, Uniform3f)},
        {"glUniform4f", offsetof(GL, Uniform4f)},
        {"glUniformMatrix4fv", offsetof(GL, UniformMatrix4fv)},
        {"glUniformMatrix3fv", offsetof(GL, UniformMatrix3fv)},
        {"glGenTextures", offsetof(GL, GenTextures)},
        {"glDeleteTextures", offsetof(GL, DeleteTextures)},
        {"glBindTexture", offsetof(GL, BindTexture)},
        {"glTexImage2D", offsetof(GL, TexImage2D)},
        {"glTexParameteri", offsetof(GL, TexParameteri)},
        {"glActiveTexture", offsetof(GL, ActiveTexture)},
        {"glBindBufferBase", offsetof(GL, BindBufferBase)},
        {"glGetUniformBlockIndex", offsetof(GL, GetUniformBlockIndex)},
        {"glUniformBlockBinding", offsetof(GL, UniformBlockBinding)},
        {"glGetIntegeri_v", offsetof(GL, GetIntegeri_v)},
        {"glGetString", offsetof(GL, GetString)},
        {"glGetStringi", offsetof(GL, GetStringi)},
        {nullptr, 0},  // sentinel
    };
    return kEntries;
}
}  // namespace detail

template <typename F>
bool loadGL(F&& loader) {
    int missing = 0;
    const char* firstMissing[4] = {nullptr, nullptr, nullptr, nullptr};
    for (const detail::Entry* e = detail::entries(); e->name; ++e) {
        auto** slot = reinterpret_cast<void**>(reinterpret_cast<char*>(&gl) + e->offset);
        void* p = loader(e->name);
        // Fall back to the non-ARB (core) spelling for instanced arrays, which is
        // what a strict 3.3 core context exposes.
        if (!p && std::strcmp(e->name, "glVertexAttribDivisorARB") == 0)
            p = loader("glVertexAttribDivisor");
        if (!p && std::strcmp(e->name, "glDrawArraysInstancedARB") == 0)
            p = loader("glDrawArraysInstanced");
        if (!p) {
            if (missing < 4) firstMissing[missing] = e->name;
            ++missing;
            continue;
        }
        *slot = p;
    }
    if (missing > 0) {
        std::fprintf(stderr, "[aw] loadGL: %d missing entry points", missing);
        for (int i = 0; i < 4 && firstMissing[i]; ++i)
            std::fprintf(stderr, "  %s", firstMissing[i]);
        std::fprintf(stderr, "\n");
    }
    gl.DebugMessageCallbackARB =
        reinterpret_cast<decltype(gl.DebugMessageCallbackARB)>(loader("glDebugMessageCallback"));
    gl.ready = missing == 0;
    return gl.ready;
}

}  // namespace aw

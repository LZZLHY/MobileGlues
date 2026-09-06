#pragma once
// Shared single/batch baseVertex and primitive-restart arithmetic.
inline void mg_rebase_indices_to_u32(GLuint* dst, const void* src, GLsizei count, GLenum type, GLint basevertex,
                                     bool restart_enabled, GLuint sentinel) {
    const GLuint bv = static_cast<GLuint>(basevertex);

#define MG_REBASE_LOOP(SRCTYPE)                                                                                        \
    do {                                                                                                               \
        const SRCTYPE* s = static_cast<const SRCTYPE*>(src);                                                           \
        if (restart_enabled) {                                                                                         \
            for (GLsizei j = 0; j < count; ++j)                                                                        \
                dst[j] = (static_cast<GLuint>(s[j]) == sentinel) ? 0xFFFFFFFFu : (static_cast<GLuint>(s[j]) + bv);     \
        } else {                                                                                                       \
            for (GLsizei j = 0; j < count; ++j)                                                                        \
                dst[j] = static_cast<GLuint>(s[j]) + bv;                                                               \
        }                                                                                                              \
    } while (0)

    switch (type) {
    case GL_UNSIGNED_INT:
        MG_REBASE_LOOP(GLuint);
        break;
    case GL_UNSIGNED_SHORT:
        MG_REBASE_LOOP(GLushort);
        break;
    case GL_UNSIGNED_BYTE:
        MG_REBASE_LOOP(GLubyte);
        break;
    default:
        break;
    }

#undef MG_REBASE_LOOP
}

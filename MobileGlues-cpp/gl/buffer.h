// MobileGlues - gl/buffer.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#ifndef MOBILEGLUES_BUFFER_H
#define GL_GLEXT_PROTOTYPES
#include "../config/settings.h"
#include "../gles/loader.h"
#include "../includes.h"
#include "glcorearb.h"
#include "log.h"
#include "mg.h"
#include <GL/gl.h>
#include <cstddef>
#include <vector>

#ifdef __cplusplus
extern "C"
{
#endif

    GLuint gen_buffer();

    GLboolean has_buffer(GLuint key);

    void modify_buffer(GLuint key, GLuint value);

    void remove_buffer(GLuint key);

    GLuint find_real_buffer(GLuint key);

    GLuint find_bound_buffer(GLenum key);

#if defined(MG_PLATFORM_OHOS)

    // Size of a buffer's store as this layer recorded it, 0 when unknown. Recorded by
    // glBufferData and glBufferStorage. Declared here so an upload can answer "how big is this
    // buffer" without a driver round trip; both were previously file-local to buffer.cpp.
    void set_buffer_data_size(GLuint buffer, size_t size);
    size_t get_buffer_data_size(GLuint buffer);

    // "This buffer has been the destination of a buffer-to-buffer copy at least once."
    //
    // A probe, not a policy: nothing in the layer changes behaviour on it yet. It exists to test a
    // candidate discriminator for the MC 26.2 + Sodium terrain race using only calls this layer
    // already sees, rather than assumptions about the application's internal ordering.
    //
    // The idea being tested: Sodium moves terrain through its own persistently mapped ring plus
    // glCopyBufferSubData, and reaches glNamedBufferSubData only when an upload does not fit the
    // ring's remaining space - so its arena is a copy destination. Minecraft's own terrain heap is
    // written directly and, as far as is known, never copied into. If that holds on the device, this
    // flag separates the buffer that races from the one that does not.
    //
    // It must be confirmed on vanilla 26.2 before anything depends on it: see the
    // direct_dest_copy_target counter.
    void mark_buffer_copy_destination(GLuint buffer);
    GLboolean is_buffer_copy_destination(GLuint buffer);

    // Zero-timeout poll of the per-present frame fence; defined in egl/egl.cpp. Returns a
    // glClientWaitSync result, or 0 when there is no fence to poll. Cannot block.
    GLenum mg_frame_fence_poll(void);

    // ---- Deferred terrain upload ------------------------------------------------------------
    //
    // Queues the bytes of a large glNamedBufferSubData instead of writing them immediately, and
    // replays them at the frame boundary. See gl/buffer.cpp for the full reasoning; the short
    // version is that this changes *when* the write happens, which is the only axis left.
    //
    // Returns GL_TRUE when the upload was queued and the caller must do nothing else.
    GLboolean mg_deferred_upload_enqueue(GLuint realBuffer, GLintptr offset, GLsizeiptr size, const void* data);

    // Replays everything queued, waiting first for the fence taken when the queue was opened.
    // Called once per present from egl/egl.cpp. Safe to call with an empty queue.
    void mg_deferred_upload_flush_all(void);

    // Replays only the records targeting one buffer. The safety net: any operation that could
    // observe a queued buffer's contents, or destroy it, has to call this first.
    void mg_deferred_upload_flush_buffer(GLuint realBuffer);

    // GL_TRUE while anything is queued. Lets hot paths skip the per-buffer check entirely.
    GLboolean mg_deferred_upload_pending(void);

#endif

    GLuint gen_array();

    GLboolean has_array(GLuint key);

    void modify_array(GLuint key, GLuint value);

    void remove_array(GLuint key);

    GLuint find_real_array(GLuint key);

    GLuint find_bound_array();

    static GLenum get_binding_query(GLenum target);

    void InitBufferMap(size_t expectedSize);

    void InitVertexArrayMap(size_t expectedSize);

    GLAPI GLAPIENTRY void glGenBuffers(GLsizei n, GLuint* buffers);

    GLAPI GLAPIENTRY void glDeleteBuffers(GLsizei n, const GLuint* buffers);

    GLAPI GLAPIENTRY GLboolean glIsBuffer(GLuint buffer);

    GLAPI GLAPIENTRY void glBindBuffer(GLenum target, GLuint buffer);

    GLAPI GLAPIENTRY void glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset,
                                            GLsizeiptr size);

    GLAPI GLAPIENTRY void glBindBufferBase(GLenum target, GLuint index, GLuint buffer);

    GLAPI GLAPIENTRY void glBindVertexBuffer(GLuint bindingindex, GLuint buffer, GLintptr offset, GLsizei stride);

    GLAPI GLAPIENTRY void glTexBuffer(GLenum target, GLenum internalformat, GLuint buffer);

    GLAPI GLAPIENTRY void glTexBufferRange(GLenum target, GLenum internalformat, GLuint buffer, GLintptr offset,
                                           GLsizeiptr size);

    GLAPI GLAPIENTRY GLboolean glUnmapBuffer(GLenum target);

    GLAPI GLAPIENTRY void* glMapBuffer(GLenum target, GLenum access);

    GLAPI GLAPIENTRY void* glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access);

    GLAPI GLAPIENTRY void glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage);

    GLAPI GLAPIENTRY void glBufferStorage(GLenum target, GLsizeiptr size, const void* data, GLbitfield flags);

    GLAPI GLAPIENTRY void glFlushMappedBufferRange(GLenum target, GLintptr offset, GLsizeiptr length);

    GLAPI GLAPIENTRY void glGenVertexArrays(GLsizei n, GLuint* arrays);

    GLAPI GLAPIENTRY void glDeleteVertexArrays(GLsizei n, const GLuint* arrays);

    GLAPI GLAPIENTRY GLboolean glIsVertexArray(GLuint array);

    GLAPI GLAPIENTRY void glBindVertexArray(GLuint array);

#ifdef __cplusplus
}
#endif

#define MOBILEGLUES_BUFFER_H

#endif // MOBILEGLUES_BUFFER_H

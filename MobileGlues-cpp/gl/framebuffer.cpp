// MobileGlues - gl/framebuffer.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include "framebuffer.h"
#include "../egl/context.h"
#include <mutex>
#include <memory>
#include <algorithm>
#include <ska/flat_hash_map.hpp>
#include "log.h"
#include "../config/settings.h"
#include "FSR1/FSR1.h"

#define DEBUG 0

// One line per site, not one per call. LOG_W_FORCE is unconditional -- that is
// what it is for, LOG_W compiles out in release -- and each line costs an
// __android_log_print, a printf and an fflush'd write to the log file. The only
// site is in glDrawBuffers, whose condition is a property of the framebuffer's
// attachment layout: it holds for as long as the application keeps that layout,
// so it fires on every pass of every frame and says nothing new after the first.
#define FB_WARN_ONCE(...)                                                                                              \
    do {                                                                                                               \
        static bool mg_fb_warned = false;                                                                              \
        if (!mg_fb_warned) {                                                                                           \
            mg_fb_warned = true;                                                                                       \
            LOG_W_FORCE(__VA_ARGS__)                                                                                   \
        }                                                                                                              \
    } while (0)

static GLint MAX_COLOR_ATTACHMENTS = 0;
static GLint MAX_DRAW_BUFFERS = 0;
// Framebuffer objects are container state: GL does not share them across a share
// group, so they belong to one context. See gl/buffer.cpp for the pattern.
//
// The two current bindings live in this record rather than at file scope. They
// used to be process-global while the table they index was already per-context,
// so a context switch left a name that was only valid in the old context
// indexing the new context's table -- and every index site in this file is an
// unchecked operator[]. The new table is freshly default-constructed, so the
// read landed either on a null data() or, worse, inside the reserved capacity of
// g_fbo_default, which yields an unconstructed framebuffer_t whose
// color_attachments is an uninitialised pointer that update_attachment writes
// twelve bytes through.
namespace {
    // Keyed by name rather than indexed by it. The vector this replaces was resized
    // to id + 10 on every miss, so one framebuffer name out of the usual small
    // sequential run cost a record for every name below it -- and framebuffer_t
    // carries two vectors now, which made that worse. The records are held by
    // pointer: the map moves its elements when it grows, and several functions
    // here hold a framebuffer_t& across calls that can insert another name.
    struct fbo_ctx_state_t {
        ska::flat_hash_map<GLuint, std::unique_ptr<framebuffer_t>> table;
        GLuint draw = 0;
        GLuint read = 0;
    };
    std::mutex g_fbo_mutex;
    // By pointer for the same reason: g_fc is a thread_local into an entry.
    ska::flat_hash_map<unsigned long long, std::unique_ptr<fbo_ctx_state_t>> g_fbo_ctxs;
    // Per thread, not one shared instance. This is where a context this layer never
    // saw created lands, and every such thread used to read and write the same
    // tables and the same two bindings with no lock between them. A context is
    // current on one thread at a time, so a thread-local fallback is also the more
    // accurate model of what it stands for.
    thread_local fbo_ctx_state_t g_fbo_default;
    thread_local fbo_ctx_state_t* g_fc = &g_fbo_default;
} // namespace

void mg_framebuffer_bind_context(unsigned long long ctx_id) {
    if (ctx_id == 0) {
        g_fc = &g_fbo_default;
        return;
    }
    std::lock_guard<std::mutex> lock(g_fbo_mutex);
    std::unique_ptr<fbo_ctx_state_t>& slot = g_fbo_ctxs[ctx_id];
    if (!slot) slot = std::make_unique<fbo_ctx_state_t>();
    g_fc = slot.get();
}

void mg_framebuffer_forget_context(unsigned long long ctx_id) {
    if (ctx_id == 0) return;
    std::lock_guard<std::mutex> lock(g_fbo_mutex);
    const auto it = g_fbo_ctxs.find(ctx_id);
    if (it == g_fbo_ctxs.end()) return;
    if (g_fc == it->second.get()) g_fc = &g_fbo_default;
    g_fbo_ctxs.erase(it);
}

// The bodies below are unchanged: the names now resolve into the per-context
// record instead of to file-scope globals.
#define framebuffers (g_fc->table)
#define current_draw_fbo (g_fc->draw)
#define current_read_fbo (g_fc->read)
// gl/gl.cpp used to reach in with mg_framebuffers()[current_draw_fbo], which is
// the same unchecked index from another translation unit and could not see the
// table's size. It gets the predicate instead.
bool mg_draw_framebuffer_all_none() {
    const auto it = framebuffers.find(current_draw_fbo);
    return it != framebuffers.end() && it->second->color_attachments_all_none;
}
void ensure_max_attachments() {
    // The fallback is used but no longer cached. A query made with no context
    // current answers nothing, and writing 8 into the static then meant 8 for the
    // life of the process -- on four-attachment hardware the draw-buffer shuffle
    // would attach past the end of what the driver has. Leaving the static at
    // zero makes the next call, once a context exists, ask again.
    if (MAX_COLOR_ATTACHMENTS == 0) {
        GLint v = 0;
        GLES.glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &v);
        if (v > 0) MAX_COLOR_ATTACHMENTS = v;
    }
    if (MAX_DRAW_BUFFERS == 0) {
        GLint v = 0;
        GLES.glGetIntegerv(GL_MAX_DRAW_BUFFERS, &v);
        if (v > 0) MAX_DRAW_BUFFERS = v;
    }
}

// What the tables are sized against while the query has not answered yet.
static GLint max_color_attachments_or_default() {
    return MAX_COLOR_ATTACHMENTS > 0 ? MAX_COLOR_ATTACHMENTS : 8;
}
framebuffer_t& get_framebuffer(GLuint id) {
    // Created on first sight of the name, as the old default-construct-on-index
    // did. The reference is what callers keep across further calls to this
    // function, so the record is what must not move when the map grows -- which
    // is why the table holds framebuffer_t by unique_ptr and why the lookup below
    // must keep returning the pointee rather than anything stored inline.
    //
    // A hit is the overwhelmingly common case: every glBindFramebuffer, every
    // attachment change and every draw-buffer call routes through here, and a
    // name is new only once. operator[] is the insertion path -- it drags the
    // rehash-and-grow tail in at the call site and only then discovers there was
    // nothing to insert -- so the hit is answered by find() and operator[] is
    // reached only on a genuine miss.
    const auto it = framebuffers.find(id);
    if (it != framebuffers.end() && it->second) return *it->second;
    // The null-pointer half of that test is not dead: a throwing make_unique
    // leaves an inserted-but-empty slot behind, and this function has always
    // filled such a slot rather than dereferencing it.
    std::unique_ptr<framebuffer_t>& slot = framebuffers[id];
    if (!slot) slot = std::make_unique<framebuffer_t>();
    return *slot;
}
void InitFramebufferMap(size_t expectedSize) {
    framebuffers.reserve(expectedSize);
}
void init_framebuffer(framebuffer_t& fbo) {
    ensure_max_attachments();
    const size_t want = static_cast<size_t>(max_color_attachments_or_default());
    if (!fbo.initialized) {
        fbo.color_attachments.assign(want, attachment_t{});
        fbo.initialized = true;
        return;
    }
    // The limit can go up after a record was sized. ensure_max_attachments
    // deliberately stopped latching its 8-slot fallback -- caching it from a query
    // made with no context current was its own bug -- so a framebuffer first
    // touched before the driver could answer holds eight slots while every bounds
    // check now uses the driver's real, larger number. Indexing the difference is
    // a heap write past the end.
    if (fbo.color_attachments.size() < want) fbo.color_attachments.resize(want, attachment_t{});
}
void glBindFramebuffer(GLenum target, GLuint framebuffer) {
    LOG()
    LOG_D("glBindFramebuffer, target = %s, framebuffer = %u", glEnumToString(target), framebuffer)
    ensure_max_attachments();

    // Resolve the redirect before touching the table: this used to take the
    // reference for the id the application passed and then initialise that
    // record, while the id that actually became current was g_renderFBO -- so
    // framebuffers[g_renderFBO].color_attachments stayed null and the per-fbo
    // state written later landed on the wrong record.
    //
    // The redirect is the draw binding only. GL_READ_FRAMEBUFFER was already left
    // alone, but GL_FRAMEBUFFER is two bindings in one call, so the target test
    // moved the read binding to g_renderFBO as well: the two ways of saying "read
    // framebuffer 0" then meant different framebuffers, and restoring a saved read
    // binding of 0 -- which gl/texture.cpp does around its blits -- landed on the
    // FSR1 target or not depending on which spelling the caller used.
    GLuint draw_fb = framebuffer;
    if (framebuffer == 0 && target != GL_READ_FRAMEBUFFER) {
        draw_fb = FSR1_Context::g_renderFBO;
        FSR1_Context::g_dirty = true;
    }

    if (draw_fb != 0) {
        init_framebuffer(get_framebuffer(draw_fb));
    }

    if (target != GL_READ_FRAMEBUFFER) {
        set_gl_state_current_draw_fbo(draw_fb);
    }

    if (target == GL_DRAW_FRAMEBUFFER || target == GL_FRAMEBUFFER) {
        current_draw_fbo = draw_fb;
    }
    if (target == GL_READ_FRAMEBUFFER || target == GL_FRAMEBUFFER) {
        current_read_fbo = framebuffer;
    }

    if (target == GL_FRAMEBUFFER && draw_fb != framebuffer) {
        GLES.glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fb);
        GLES.glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    } else {
        GLES.glBindFramebuffer(target, draw_fb);
    }
}
// Only the outermost scope on a thread touches the binding.
//
// The tracked read binding stays 0 the whole time one of these is active -- the
// redirect goes straight to the backend and deliberately does not change what the
// application asked for -- so a nested scope would see the same "needs
// redirecting" state, activate as well, and then undo the outer one's redirect on
// its way out while the outer scope still had work to do.
static thread_local int g_fsr_read_depth = 0;

mg_fsr_read_scope_t::mg_fsr_read_scope_t() {
    // g_renderFBO is nonzero only while FSR1 is on, so this is also the FSR1 test.
    if (FSR1_Context::g_renderFBO == 0 || current_read_fbo != 0) return;
    counted = true;
    if (g_fsr_read_depth++ > 0) return; // an outer scope already holds it
    active = true;
    GLES.glBindFramebuffer(GL_READ_FRAMEBUFFER, FSR1_Context::g_renderFBO);
}
mg_fsr_read_scope_t::~mg_fsr_read_scope_t() {
    if (!counted) return;
    --g_fsr_read_depth;
    // Back to the raw surface, which is what the tracked binding of 0 means for
    // the read target -- glBindFramebuffer never redirects that one.
    if (active) GLES.glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
}

// Record what is now attached at `attachment`. Only colour attachments are kept:
// nothing reads depth or stencil back out of here.
void update_attachment(GLenum target, GLenum attachment, const attachment_t& what) {
    GLuint current_fbo = (target == GL_READ_FRAMEBUFFER) ? current_read_fbo : current_draw_fbo;
    if (current_fbo == 0) return;
    if (attachment < GL_COLOR_ATTACHMENT0 ||
        attachment >= GL_COLOR_ATTACHMENT0 + (GLenum)max_color_attachments_or_default()) {
        return;
    }
    framebuffer_t& fbo = get_framebuffer(current_fbo);
    init_framebuffer(fbo);
    const size_t index = attachment - GL_COLOR_ATTACHMENT0;
    if (index >= fbo.color_attachments.size()) return;
    fbo.color_attachments[index] = what;
    // The attachment wrapper updates the mapped physical slot. Its routing
    // remains valid when the image, layer or renderbuffer is replaced.
}

static GLenum physical_attachment(GLenum target, GLenum attachment) {
    const GLuint id = target == GL_READ_FRAMEBUFFER ? current_read_fbo : current_draw_fbo;
    if (id == 0 || attachment < GL_COLOR_ATTACHMENT0 ||
        attachment >= GL_COLOR_ATTACHMENT0 + max_color_attachments_or_default())
        return attachment;
    auto& fbo = get_framebuffer(id);
    const size_t index = attachment - GL_COLOR_ATTACHMENT0;
    return index < fbo.draw_buffer_map.size() && fbo.draw_buffer_map[index] ? fbo.draw_buffer_map[index] : attachment;
}

// Undo a shuffle: put every attachment glDrawBuffers moved back on its own
// attachment point.
//
// Needed because leaving the shuffled state is not free. After
// glDrawBuffers({ATTACHMENT1, ATTACHMENT0}) the texture the application calls
// attachment 0 physically sits on GL_COLOR_ATTACHMENT1, so a later
// glDrawBuffers({ATTACHMENT0}) -- identity order, nothing to move by itself --
// would draw into whatever is on physical attachment 0, which is the other
// texture. The old unconditional re-attach happened to fix this up on every call;
// anything that skips it has to put things back first.
void restore_home_attachments(framebuffer_t& fbo);

// Put a recorded attachment onto a (possibly different) attachment point, the
// same way it originally arrived.
//
// Deliberately calls GLES directly: the only caller is the glDrawBuffers shuffle,
// which is in the middle of building draw_buffer_map, and going back through the
// wrappers above would clear it.
void reattach(GLenum target, GLenum attachment, const attachment_t& a) {
    switch (a.kind) {
    case attach_kind_t::Texture2D:
        GLES.glFramebufferTexture2D(target, attachment, a.textarget, a.texture, a.level);
        break;
    case attach_kind_t::TextureLayer:
        GLES.glFramebufferTextureLayer(target, attachment, a.texture, a.level, a.layer);
        break;
    case attach_kind_t::TextureAll:
        GLES.glFramebufferTexture(target, attachment, a.texture, a.level);
        break;
    case attach_kind_t::Renderbuffer:
        GLES.glFramebufferRenderbuffer(target, attachment, a.textarget, a.texture);
        break;
    case attach_kind_t::None:
        GLES.glFramebufferTexture2D(target, attachment, GL_TEXTURE_2D, 0, 0);
        break;
    }
}

void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
    mg_begin_driver_operation();
    GLES.glFramebufferTexture2D(target, physical_attachment(target, attachment), textarget, texture, level);
    if (mg_end_driver_operation("glFramebufferTexture2D"))
        update_attachment(target, attachment, {attach_kind_t::Texture2D, textarget, texture, level, 0});
}
void glFramebufferTexture(GLenum target, GLenum attachment, GLuint texture, GLint level) {
    // Kind rather than a made-up GL_TEXTURE_2D. This entry point attaches the
    // whole texture, whatever its target is, and recording it as a 2D attachment
    // meant a replay re-attached an array or 3D texture as if it were flat.
    mg_begin_driver_operation();
    GLES.glFramebufferTexture(target, physical_attachment(target, attachment), texture, level);
    if (mg_end_driver_operation("glFramebufferTexture"))
        update_attachment(target, attachment, {attach_kind_t::TextureAll, 0, texture, level, 0});
}
// Wrapped rather than passed straight through, so the record knows about them.
// While these bypassed the table, the record for an attachment they wrote stayed
// all-zero and the glDrawBuffers shuffle detached it.
void glFramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer) {
    mg_begin_driver_operation();
    GLES.glFramebufferTextureLayer(target, physical_attachment(target, attachment), texture, level, layer);
    if (mg_end_driver_operation("glFramebufferTextureLayer"))
        update_attachment(target, attachment, {attach_kind_t::TextureLayer, 0, texture, level, layer});
}
void glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer) {
    mg_begin_driver_operation();
    GLES.glFramebufferRenderbuffer(target, physical_attachment(target, attachment), renderbuffertarget, renderbuffer);
    if (mg_end_driver_operation("glFramebufferRenderbuffer"))
        update_attachment(target, attachment, {attach_kind_t::Renderbuffer, renderbuffertarget, renderbuffer, 0, 0});
}

void restore_home_attachments(framebuffer_t& fbo) {
    if (fbo.draw_buffer_map.empty()) return;
    // Every recorded attachment, not just the ones the map says were moved.
    //
    // The map answers "where did logical i go", which is the wrong direction for
    // undoing. A slot that was only ever a DESTINATION -- something else was moved
    // on top of it, detaching what lived there -- has a zero entry, so walking the
    // map skipped exactly the slot that needs repairing. With colortex0 on
    // attachment 0 and colortex1 on attachment 1, glDrawBuffers({ATTACHMENT1})
    // moves colortex1 onto physical 0; a following glDrawBuffers({ATTACHMENT0})
    // then found map[0] == 0, left physical 0 holding colortex1, and the pass
    // wrote its output into the wrong texture.
    //
    // color_attachments is the layout the application actually asked for, so
    // putting every entry back on its own point is both sufficient and
    // idempotent: re-attaching something already in place costs one call and
    // changes nothing.
    for (size_t idx = 0; idx < fbo.color_attachments.size(); ++idx) {
        const attachment_t& a = fbo.color_attachments[idx];
        if (a.kind == attach_kind_t::None) continue;
        reattach(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + (GLenum)idx, a);
    }
    fbo.draw_buffer_map.clear();
}

// The ARB spellings, which these three had while they were NATIVE_FUNCTION_HEAD
// entries -- that macro emits a `name##ARB` alias next to every function it
// defines, and moving them here quietly dropped three of the 500-odd aliases the
// library exports. An application that resolves glDeleteFramebuffersARB, as
// anything written against EXT_framebuffer_object does, got a null pointer.
extern "C"
{
    GLAPI GLAPIENTRY void glDeleteFramebuffersARB(GLsizei n, const GLuint* names)
        __attribute__((alias("glDeleteFramebuffers")));
    GLAPI GLAPIENTRY void glFramebufferRenderbufferARB(GLenum target, GLenum attachment, GLenum renderbuffertarget,
                                                       GLuint renderbuffer)
        __attribute__((alias("glFramebufferRenderbuffer")));
    GLAPI GLAPIENTRY void glFramebufferTextureLayerARB(GLenum target, GLenum attachment, GLuint texture, GLint level,
                                                       GLint layer) __attribute__((alias("glFramebufferTextureLayer")));
}

// Wrapped for the same reason glReadPixels is: the source is the read framebuffer,
// and while FSR1 is on the application's framebuffer 0 is not where its frame is.
// The commit that added mg_fsr_read_scope_t covered this layer's own internal
// blits and left the application-facing entry point a raw passthrough, so a host
// blitting from framebuffer 0 -- which is how most post-processing gets its
// input, and the only way to reach the depth buffer that lives on the FSR target
// and never on the surface -- still read the window.
void glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1,
                       GLint dstY1, GLbitfield mask, GLenum filter) {
    LOG()
    mg_fsr_read_scope_t fsr_read;
    GLES.glBlitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
    CHECK_GL_ERROR
}

void glDeleteFramebuffers(GLsizei n, const GLuint* names) {
    // The record has to die with the name. Drivers hand deleted framebuffer names
    // straight back out of the next glGenFramebuffers, and this table never
    // dropped anything -- so a recycled name inherited the previous framebuffer's
    // attachments (which the draw-buffer shuffle would then re-attach over
    // whatever the application had just bound), its draw_buffer_map (silently
    // redirecting glReadBuffer) and its all-none flag (misfiring the ANGLE
    // depth-clear workaround in gl/gl.cpp). The pixel helpers in gl/texture.cpp
    // create and delete temporary framebuffers constantly, so this recycling is
    // the common case rather than a corner one.
    if (names != nullptr) {
        for (GLsizei i = 0; i < n; ++i) {
            const GLuint name = names[i];
            if (name == 0) continue; // silently ignored, per spec
            framebuffers.erase(name);
            // "If a framebuffer object that is currently bound is deleted, the
            // binding reverts to 0" -- through this layer's own entry point, so the
            // FSR1 redirect and the tracked bindings stay in step.
            if (current_draw_fbo == name) glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
            if (current_read_fbo == name) glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        }
    }
    GLES.glDeleteFramebuffers(n, names);
}
void glDrawBuffer(GLenum buffer) {
    LOG()
    if (current_draw_fbo != 0 && current_draw_fbo == FSR1_Context::g_renderFBO &&
        (buffer == GL_BACK || buffer == GL_FRONT || buffer == GL_FRONT_AND_BACK || buffer == GL_LEFT ||
         buffer == GL_BACK_LEFT || buffer == GL_FRONT_LEFT))
        buffer = GL_COLOR_ATTACHMENT0;
    // A single-output API always assigns fragment output zero.
    glDrawBuffers(1, &buffer);
}

static void install_attachment_map(framebuffer_t& fbo, const std::vector<GLenum>& map) {
    for (size_t i = 0; i < fbo.color_attachments.size(); ++i)
        reattach(GL_DRAW_FRAMEBUFFER, map.empty() ? GL_COLOR_ATTACHMENT0 + i : map[i], fbo.color_attachments[i]);
}

void glDrawBuffers(GLsizei n, const GLenum* bufs) {
    LOG()
    ensure_max_attachments();
    const GLint maxDrawBuffers = MAX_DRAW_BUFFERS > 0 ? MAX_DRAW_BUFFERS : 4;
    if (n < 0 || n > maxDrawBuffers || (n > 0 && !bufs)) {
        mg_set_gl_error(GL_INVALID_VALUE);
        return;
    }
    if (current_draw_fbo == 0) {
        GLES.glDrawBuffers(n, bufs);
        return;
    }
    auto& fbo = get_framebuffer(current_draw_fbo);
    init_framebuffer(fbo);
    const size_t capacity = fbo.color_attachments.size();
    std::vector<GLenum> logical;
    if (n > 0) logical.assign(bufs, bufs + n);
    std::vector<GLenum> mapping(capacity, 0), driver(n, GL_NONE);
    std::vector<bool> occupied(capacity, false);
    bool identity = true, all_none = true;
    for (GLsizei slot = 0; slot < n; ++slot) {
        const GLenum value = bufs[slot];
        if (value == GL_NONE) continue;
        all_none = false;
        if (value < GL_COLOR_ATTACHMENT0 || value >= GL_COLOR_ATTACHMENT0 + capacity) {
            mg_set_gl_error(GL_INVALID_ENUM);
            return;
        }
        const size_t index = value - GL_COLOR_ATTACHMENT0;
        if (mapping[index]) {
            mg_set_gl_error(GL_INVALID_OPERATION);
            return;
        }
        if (static_cast<size_t>(slot) >= capacity) {
            mg_set_gl_error(GL_INVALID_VALUE);
            return;
        }
        mapping[index] = GL_COLOR_ATTACHMENT0 + slot;
        occupied[slot] = true;
        driver[slot] = GL_COLOR_ATTACHMENT0 + slot;
        identity &= index == static_cast<size_t>(slot);
    }
    // Complete the permutation, including attachments not currently drawn.
    // Otherwise a shuffle can overwrite an attachment that glReadBuffer later
    // needs, or an attachment update can hit another output's physical slot.
    for (size_t i = 0; i < capacity; ++i)
        if (!mapping[i] && !occupied[i]) {
            mapping[i] = GL_COLOR_ATTACHMENT0 + i;
            occupied[i] = true;
        }
    for (size_t i = 0; i < capacity; ++i)
        if (!mapping[i]) {
            const size_t free = std::find(occupied.begin(), occupied.end(), false) - occupied.begin();
            mapping[i] = GL_COLOR_ATTACHMENT0 + free;
            occupied[free] = true;
        }
    mg_begin_driver_operation();
    if (!identity || !fbo.draw_buffer_map.empty()) install_attachment_map(fbo, mapping);
    GLES.glDrawBuffers(n, driver.data());
    if (!mg_end_driver_operation("glDrawBuffers")) {
        install_attachment_map(fbo, fbo.draw_buffer_map);
        std::vector<GLenum> previous(fbo.logical_draw_buffers.size(), GL_NONE);
        for (size_t i = 0; i < previous.size(); ++i)
            if (fbo.logical_draw_buffers[i] != GL_NONE) previous[i] = GL_COLOR_ATTACHMENT0 + i;
        GLES.glDrawBuffers(previous.size(), previous.data());
        return;
    }
    fbo.logical_draw_buffers = std::move(logical);
    fbo.color_attachments_all_none = all_none;
    if (identity)
        fbo.draw_buffer_map.clear();
    else
        fbo.draw_buffer_map = std::move(mapping);
    // Read-buffer selection belongs to the FBO and survives draw routing changes.
    if (GLES.glReadBuffer) {
        const GLuint previous = current_read_fbo;
        if (previous != current_draw_fbo) GLES.glBindFramebuffer(GL_READ_FRAMEBUFFER, current_draw_fbo);
        const GLenum read = fbo.logical_read_buffer;
        const size_t index = read >= GL_COLOR_ATTACHMENT0 ? read - GL_COLOR_ATTACHMENT0 : capacity;
        GLES.glReadBuffer(index < fbo.draw_buffer_map.size() ? fbo.draw_buffer_map[index] : read);
        if (previous != current_draw_fbo) GLES.glBindFramebuffer(GL_READ_FRAMEBUFFER, previous);
    }
}

void glReadBuffer(GLenum src) {
    mg_begin_driver_operation();
    GLES.glReadBuffer(physical_attachment(GL_READ_FRAMEBUFFER, src));
    if (mg_end_driver_operation("glReadBuffer") && current_read_fbo != 0)
        get_framebuffer(current_read_fbo).logical_read_buffer = src;
}
GLenum glCheckFramebufferStatus(GLenum target) {
    GLenum status = GLES.glCheckFramebufferStatus(target);
    return status;
}

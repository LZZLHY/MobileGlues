// 旧实现负对照，冻结自f52379eb的gles/loader.h和gl/gl_stub.cpp；仅在宿主测试中编译。
// SPDX-License-Identifier: LGPL-2.1-only
#include <type_traits>
template <class T> static inline T mg_stub_default() {
    if constexpr (!std::is_void_v<T>) return T{};
}
#define GLAPI
#define GLAPIENTRY
#define STUB_FUNCTION_HEAD(type, name, ...) extern "C" GLAPI GLAPIENTRY type name(__VA_ARGS__) { LOG()
#define STUB_FUNCTION_END_NO_RETURN(type, name, ...) LOG_W("Stub function"); return mg_stub_default<type>(); }
STUB_FUNCTION_HEAD(void, glGetBufferSubData, GLenum target, GLintptr offset, GLsizeiptr size, void* data); STUB_FUNCTION_END_NO_RETURN(void, glGetBufferSubData,target,offset,size,data)
STUB_FUNCTION_HEAD(void, glGetBufferSubDataARB, GLenum target, GLintptrARB offset, GLsizeiptrARB size, void* data); STUB_FUNCTION_END_NO_RETURN(void, glGetBufferSubDataARB,target,offset,size,data)

// 深度范围生产 TU 回归：链接真实 gl_native.cpp、gl_stub.cpp、enable.cpp。
// 这里只替换 GLES 驱动边界，以确定 setter/getter 的路由、双分量写入及状态归属；
// EGL context 创建、光栅化和实际像素判据由独立的真实 provider 语义场景验证。
#include "gl/mg.h"
#include "gl/glcorearb.h"
#include "egl/context.h"
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>

gles_func_t g_gles_func{};
gles_caps_t g_gles_caps{};
thread_local MGContext* g_current_ctx = nullptr;

// 日志与无关 pixel-store 查询是本测试的外部接缝；不能接管被测深度 getter。
extern "C" void write_log(const char*, ...) {}
int __android_log_print(int, const char*, const char*, ...) { return 0; }
bool mg_pixel_store_query_int(GLenum, GLint*) { return false; }

namespace {
// 每个驱动 context 自己持有深度范围。TLS 仅选中当前 context，范围不跨线程共享。
struct DriverContext {
    GLfloat near_value = 0.0f;
    GLfloat far_value = 1.0f;
    GLenum pending_error = GL_NO_ERROR;
    unsigned setters = 0;
    unsigned queries = 0;
    unsigned nonfinite_inputs = 0;
};
thread_local DriverContext* driver_context = nullptr;
using DoubleSetter = void (*)(GLdouble, GLdouble);

// 模拟 GLES 规范的有限值 clamp；此处输入已是 float，不能替 MG 掩盖 narrowing 缺陷。
GLfloat clamp_driver(GLfloat value) {
    return value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
}
void driver_set_depth_range(GLfloat near_value, GLfloat far_value) {
    if (!std::isfinite(near_value) || !std::isfinite(far_value)) ++driver_context->nonfinite_inputs;
    driver_context->near_value = clamp_driver(near_value);
    driver_context->far_value = clamp_driver(far_value);
    ++driver_context->setters;
}
void driver_get_float(GLenum pname, GLfloat* output) {
    if (pname != GL_DEPTH_RANGE) {
        driver_context->pending_error = GL_INVALID_ENUM;
        return;
    }
    output[0] = driver_context->near_value;
    output[1] = driver_context->far_value;
    ++driver_context->queries;
}
void driver_get_boolean(GLenum pname, GLboolean* output) {
    if (pname != GL_DEPTH_RANGE) {
        driver_context->pending_error = GL_INVALID_ENUM;
        return;
    }
    output[0] = driver_context->near_value != 0.0f ? GL_TRUE : GL_FALSE;
    output[1] = driver_context->far_value != 0.0f ? GL_TRUE : GL_FALSE;
    ++driver_context->queries;
}

// 负控制忠实保留修复前 gl_stub.cpp 的空行为；只替换本次 double setter 选择。
// 同一范围/查询断言必须拒绝它，不能因新增测试总是读默认值而制造假通过。
void legacy_depth_range_noop(GLdouble, GLdouble) {}

bool require(bool condition, const char* description) {
    if (!condition) std::fprintf(stderr, "FAIL %s\n", description);
    return condition;
}

// guard 同时证明每个 getter 恰好写两分量。期望来自独立常量，不从 driver 镜像反推。
bool check_range(GLfloat near_value, GLfloat far_value) {
    GLfloat floats[4] = {-123.0f, -9.0f, -9.0f, -456.0f};
    GLdouble doubles[4] = {-123.0, -9.0, -9.0, -456.0};
    GLboolean booleans[4] = {17, 19, 23, 29};
    glGetFloatv(GL_DEPTH_RANGE, floats + 1);
    glGetDoublev(GL_DEPTH_RANGE, doubles + 1);
    glGetBooleanv(GL_DEPTH_RANGE, booleans + 1);
    return require(floats[1] == near_value && floats[2] == far_value,
                   "glDepthRange changes actual driver range") &&
           require(doubles[1] == static_cast<GLdouble>(near_value) &&
                       doubles[2] == static_cast<GLdouble>(far_value),
                   "double query returns both current driver components") &&
           require(booleans[1] == (near_value != 0.0f ? GL_TRUE : GL_FALSE) &&
                       booleans[2] == (far_value != 0.0f ? GL_TRUE : GL_FALSE),
                   "boolean query follows the same two driver components") &&
           require(floats[0] == -123.0f && floats[3] == -456.0f &&
                       doubles[0] == -123.0 && doubles[3] == -456.0 &&
                       booleans[0] == 17 && booleans[3] == 29,
                   "range queries preserve adjacent guards");
}

// 设置两个接缝指针以模拟 eglMakeCurrent 已成功后的 TLS 状态；不声称验证真实 EGL。
void select_context(MGContext& context, DriverContext& driver) {
    g_current_ctx = &context;
    driver_context = &driver;
}

bool run_range_matrix(DoubleSetter set_double) {
    MGContext first{}, second{};
    DriverContext driver_first, driver_second;
    select_context(first, driver_first);
    if (!check_range(0.0f, 1.0f)) return false;

    // 非默认、反向、等端点、越界及两入口交替；反向范围合法，不重排两端。
    set_double(0.25, 0.75);
    if (!check_range(0.25f, 0.75f)) return false;
    set_double(0.875, 0.125);
    if (!check_range(0.875f, 0.125f)) return false;
    set_double(0.375, 0.375);
    if (!check_range(0.375f, 0.375f)) return false;
    set_double(-0.25, 1.25);
    if (!check_range(0.0f, 1.0f)) return false;
    set_double(1.25, -0.25);
    if (!check_range(1.0f, 0.0f)) return false;
    glDepthRangef(0.125f, 0.625f);
    if (!check_range(0.125f, 0.625f)) return false;
    set_double(0.75, 0.25);
    if (!check_range(0.75f, 0.25f)) return false;
    glDepthRangef(-2.0f, 3.0f);
    if (!check_range(0.0f, 1.0f)) return false;

    // 最大有限 double 不能在转 float 时越出可表示范围；先 clamp 才能保证可移植。
    set_double(std::numeric_limits<GLdouble>::max(), -std::numeric_limits<GLdouble>::max());
    if (!check_range(1.0f, 0.0f) ||
        !require(driver_first.nonfinite_inputs == 0,
                 "finite double values are clamped before float narrowing")) return false;
    set_double(0.2, 0.7);
    if (!check_range(static_cast<GLfloat>(0.2), static_cast<GLfloat>(0.7))) return false;

    // 新 context 默认独立；float 入口、切回和直接 driver 更新均不能被 MG 全局缓存遮蔽。
    select_context(second, driver_second);
    if (!check_range(0.0f, 1.0f)) return false;
    glDepthRangef(0.5f, 0.875f);
    if (!check_range(0.5f, 0.875f)) return false;
    select_context(first, driver_first);
    if (!check_range(static_cast<GLfloat>(0.2), static_cast<GLfloat>(0.7))) return false;
    driver_set_depth_range(0.625f, 0.375f);
    if (!check_range(0.625f, 0.375f)) return false;

    // 普通 setter/query 不应主动读取并吞掉无关的 pending driver 错误。
    driver_first.pending_error = GL_INVALID_OPERATION;
    const unsigned before = driver_first.setters;
    set_double(0.25, 0.5);
    if (!check_range(0.25f, 0.5f) ||
        !require(driver_first.pending_error == GL_INVALID_OPERATION,
                 "range path preserves unrelated pending driver error") ||
        !require(driver_first.setters == before + 1,
                 "double setter performs one driver state update")) return false;
    g_current_ctx = nullptr;
    driver_context = nullptr;
    return true;
}

// 函数表在启动时建立一次；线程只选择自己的 context，不能热替换进程全局 provider。
bool check_threads() {
    std::atomic<bool> success{true};
    auto worker = [&success](GLfloat near_value, GLfloat far_value) {
        MGContext context{};
        DriverContext driver;
        select_context(context, driver);
        for (int iteration = 0; iteration < 100; ++iteration) {
            glDepthRange(near_value, far_value);
            if (!check_range(near_value, far_value)) success.store(false);
        }
        g_current_ctx = nullptr;
        driver_context = nullptr;
    };
    std::thread first(worker, 0.125f, 0.875f);
    std::thread second(worker, 0.75f, 0.25f);
    first.join();
    second.join();
    return require(success.load(), "range state stays in the current thread context");
}
} // namespace

int main(int argc, char** argv) {
    // 独立进程中的固定驱动接缝，不与应用/像素测试进程的 GLES 函数表混用。
    GLES.glDepthRangef = driver_set_depth_range;
    GLES.glGetFloatv = driver_get_float;
    GLES.glGetBooleanv = driver_get_boolean;
    const bool legacy = argc == 2 && std::strcmp(argv[1], "--verify-legacy-noop") == 0;
    if (legacy) {
        // 只有同一语义矩阵确实拒绝旧空行为才通过；旧行为意外过关时必须非零退出。
        if (run_range_matrix(legacy_depth_range_noop)) return 1;
        std::puts("PASS negative control: original double noop rejected by driver range query");
        return 0;
    }
    if (!run_range_matrix(glDepthRange)) return 1;
    if (!check_threads()) return 1;
    std::puts("PASS production depth range: setters, queries, guards, clamp, context/thread seam");
    return 0;
}

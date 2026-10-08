// desktop版本回归链接真实context创建/切换/退休及getter TU。仅GPU与无关资源表边界替换；
// EGL合法性由独立真实MG runtime矩阵补证，此测试不把人工句柄称为真实driver context。
#include "gl/getter.h"
#include "egl/context.h"
#include "gl/random_string_gen.h"
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <sstream>
#include <vector>

gles_func_t g_gles_func{};
gles_caps_t g_gles_caps{};
global_settings_t global_settings{};
hardware_s test_hardware{320, false};
hardware_t hardware=&test_hardware;
gl_state_s g_default_gl_state{};
thread_local gl_state_t gl_state=&g_default_gl_state;
namespace FSR1_Context { thread_local GLuint g_renderFBO=0; }
extern "C" void write_log(const char*,...) {}
int __android_log_print(int,const char*,const char*,...) { return 0; }
extern "C" GLuint find_bound_buffer(GLenum) { return 0; }
extern "C" GLuint find_bound_array() { return 0; }
int mg_max_texture_units() { return 32; }
bool mg_pixel_store_query_int(GLenum,GLint*) { return false; }
std::string dump_settings_string(std::string) { return "test"; }
std::string GenerateRandomString(const RandomStringOptions&) { return "test"; }

// 资源边界不生成版本身份；生产context表持有引用、current计数和destroy_pending的逻辑不替换。
extern "C" void mg_buffer_register_context(unsigned long long,unsigned long long) {}
void mg_buffer_bind_context(unsigned long long,unsigned long long) {}
void mg_texture_bind_context(unsigned long long,unsigned long long) {}
void mg_framebuffer_bind_context(unsigned long long) {}
void mg_fsr1_bind_context(unsigned long long) {}
void mg_buffer_forget_context(unsigned long long) {}
void mg_texture_forget_context(unsigned long long) {}
void mg_framebuffer_forget_context(unsigned long long) {}
void mg_fsr1_forget_context(unsigned long long) {}
void mg_depth_clear_forget_context(unsigned long long) {}

namespace {
void Check(bool condition,const char* message) {
    if(!condition) throw std::runtime_error(message);
}
void DriverInteger(GLenum name,GLint* value) {
    *value=name==GL_MAJOR_VERSION ? 3 : name==GL_MINOR_VERSION ? 2 : 0;
}
const GLubyte* DriverString(GLenum) { return reinterpret_cast<const GLubyte*>("unit driver boundary"); }
void CheckExtensionNamespace() {
    // 运行生产base initializer及getter，不手写扩展列表或core标识过滤器。显式
    // GL_ARB真实名称须保留；OpenGLxx不是GL扩展名称，会污染实际LWJGL版本集合。
    InitGLESBaseExtensions();
    const char* raw=reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    Check(raw!=nullptr,"production extension string exists");
    std::istringstream stream(raw);std::string part;std::vector<std::string> names;
    while(stream>>part) {
        Check(part.rfind("OpenGL",0)!=0,"public extension namespace contains forged core token");
        names.push_back(part);
    }
    Check(std::strstr(raw,"GL_ARB_fragment_program")&&std::strstr(raw,"GL_ARB_vertex_buffer_object"),"real GL extensions retained");
    int count=-1;glGetIntegerv(GL_NUM_EXTENSIONS,&count);
    Check(count==static_cast<int>(names.size()),"production extension count equals raw token count");
    for(int i=0;i<count;++i) {
        const auto* indexed=glGetStringi(GL_EXTENSIONS,i);
        Check(indexed&&names[i]==reinterpret_cast<const char*>(indexed),"production indexed extension ordering equals raw string");
    }
}
void CheckDesktopVersion(int major,int minor) {
    int actualMajor=-1,actualMinor=-1,stringMajor=-1,stringMinor=-1;
    glGetIntegerv(GL_MAJOR_VERSION,&actualMajor); glGetIntegerv(GL_MINOR_VERSION,&actualMinor);
    const char* version=reinterpret_cast<const char*>(glGetString(GL_VERSION));
    Check(version && std::sscanf(version,"%d.%d",&stringMajor,&stringMinor)==2,"version string prefix");
    Check(actualMajor==major && actualMinor==minor,"production granted integer identity");
    Check(stringMajor==major && stringMinor==minor,"desktop string matches granted current identity");
    if (g_current_ctx && g_current_ctx->client_type==EGL_OPENGL_API) {
        // 只验证既有MG token兼容行为，不把GL_VERSION参数描述成标准indexed查询。
        const auto* majorToken=glGetStringi(GL_VERSION,0);
        const auto* minorToken=glGetStringi(GL_VERSION,1);
        Check(majorToken && std::string(reinterpret_cast<const char*>(majorToken))==std::to_string(major),"compat version major token follows current record");
        Check(minorToken && std::string(reinterpret_cast<const char*>(minorToken))==std::to_string(minor),"compat version minor token follows current record");
        Check(glGetStringi(GL_VERSION,1000000)==nullptr,"compat out-of-range remains null");
    }
}
}

int main(int argc,char** argv) {
    try {
        GLVersion=Version(4,0,0);
        global_settings.hide_mg_env_level=HideMGEnvLevel::Disabled;
        GLES.glGetIntegerv=DriverInteger;
        GLES.glGetString=DriverString;
        // 第一次任意indexed cache读取之前初始化，遵守真实bootstrap的发布顺序。
        CheckExtensionNamespace();
        // 无current保留既有配置fallback；GLSL表达实现语言上限，不强制等于context的GL版本。
        CheckDesktopVersion(4,0);
        const std::string language=reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
        Check(language.find("4.60")==0,"existing implementation GLSL ceiling");
        const auto display=reinterpret_cast<EGLDisplay>(1);
        const auto low=reinterpret_cast<EGLContext>(11),high=reinterpret_cast<EGLContext>(12);
        Check(mg_context_create(display,low,EGL_NO_CONTEXT,EGL_OPENGL_API,3,2,GL_CONTEXT_CORE_PROFILE_BIT,0),"create low record");
        Check(mg_context_create(display,high,EGL_NO_CONTEXT,EGL_OPENGL_API,4,0,GL_CONTEXT_CORE_PROFILE_BIT,0),"create high record");
        if(argc==2 && std::strcmp(argv[1],"--high-first")==0) {
            mg_context_make_current(display,EGL_NO_SURFACE,EGL_NO_SURFACE,high); CheckDesktopVersion(4,0);
        }
        mg_context_make_current(display,EGL_NO_SURFACE,EGL_NO_SURFACE,low); CheckDesktopVersion(3,2);
        const char* lowPointer=reinterpret_cast<const char*>(glGetString(GL_VERSION));
        const char* lowToken=reinterpret_cast<const char*>(glGetStringi(GL_VERSION,0));
        const std::string lowText=lowPointer;
        mg_context_make_current(display,EGL_NO_SURFACE,EGL_NO_SURFACE,high); CheckDesktopVersion(4,0);
        const char* highPointer=reinterpret_cast<const char*>(glGetString(GL_VERSION));
        Check(std::string(lowPointer)==lowText,"other-context query preserves live low pointer");
        Check(std::string(lowToken)=="3","other-context query preserves live low token pointer");
        mg_context_make_current(display,EGL_NO_SURFACE,EGL_NO_SURFACE,low); CheckDesktopVersion(3,2);
        Check(glGetString(GL_VERSION)==reinterpret_cast<const GLubyte*>(lowPointer),"switch-back preserves exact pointer");
        mg_context_destroy(low);
        Check(mg_context_find(low)!=nullptr,"pending destroy retains current record");
        CheckDesktopVersion(3,2);
        Check(std::string(lowPointer)==lowText,"pending destroy retains current string lifetime");
        mg_context_make_current(display,EGL_NO_SURFACE,EGL_NO_SURFACE,high);
        Check(mg_context_find(low)==nullptr,"unbind retires pending low record");
        // EGL允许解绑后迁移线程；生产表的同一记录和immutable字符串必须继续稳定。
        mg_context_make_current(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        std::exception_ptr failure;
        std::thread migrated([&] {
            try {
                mg_context_make_current(display,EGL_NO_SURFACE,EGL_NO_SURFACE,high);
                CheckDesktopVersion(4,0);
                Check(glGetString(GL_VERSION)==reinterpret_cast<const GLubyte*>(highPointer),"thread migration preserves pointer");
                mg_context_make_current(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
            } catch(...) { failure=std::current_exception(); }
        });
        migrated.join(); if(failure) std::rethrow_exception(failure);
        mg_context_destroy(high);
        Check(mg_context_find(high)==nullptr,"unbound high record retires");
        CheckDesktopVersion(4,0);
        const auto es=reinterpret_cast<EGLContext>(13);
        Check(mg_context_create(display,es,EGL_NO_CONTEXT,EGL_OPENGL_ES_API,3,2,0,0),"create existing ES transport record");
        mg_context_make_current(display,EGL_NO_SURFACE,EGL_NO_SURFACE,es);
        int major=0,minor=0; glGetIntegerv(GL_MAJOR_VERSION,&major); glGetIntegerv(GL_MINOR_VERSION,&minor);
        Check(major==3 && minor==2,"existing ES integer branch retained");
        Check(std::strncmp(reinterpret_cast<const char*>(glGetString(GL_VERSION)),"4.0",3)==0,"existing public MG ES-owner string contract retained");
        Check(std::string(reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION)))==language,"GLSL implementation ceiling unchanged");
        mg_context_destroy(es); mg_context_make_current(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
        std::puts("PASS production desktop version identity: orders, pointers, pending destroy, thread migration, legacy fallback; ES frontend boundary retained");
        return 0;
    } catch(const std::exception& error) { std::fprintf(stderr,"FAIL %s\n",error.what()); return 1; }
}

// 真实enable.cpp TU：仅替换driver查询/错误边界，不替换参数分类或公共生产入口。
#include "gl/mg.h"
#include "gl/enable.h"
#include "egl/context.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

gles_func_t g_gles_func{};
gles_caps_t g_gles_caps{};
thread_local MGContext* g_current_ctx = nullptr;
extern "C" void write_log(const char*, ...) {}
int __android_log_print(int, const char*, const char*, ...) { return 0; }
namespace {
GLenum driverError = 0;
std::vector<GLenum> errors;
unsigned queries = 0, setters = 0;
bool blend = false;
constexpr GLenum futureDriverCap = 0xf123;
void require(bool pass, const char* text) {
    if (!pass) { std::fprintf(stderr,"失败：%s queries=%u setters=%u errors=%zu\n",text,queries,setters,errors.size()); std::exit(1); }
}
GLenum getError() { GLenum value=driverError;driverError=0;return value; }
GLboolean isEnabled(GLenum cap) {
    ++queries;
    if(cap==GL_BLEND)return blend;
    if(cap==futureDriverCap)return GL_TRUE; // 模拟实际driver接受未来extension参数，但MG尚无实现。
    driverError=GL_INVALID_ENUM;return GL_FALSE;
}
void enable(GLenum cap) { ++setters;require(cap==GL_BLEND,"不能将未知参数转发成新backing");blend=true; }
void disable(GLenum cap) { ++setters;require(cap==GL_BLEND,"不能将未知参数转发成新backing");blend=false; }
void getInt(GLenum,GLint* value) { *value=0; }
}
void mg_set_gl_error(GLenum value) {
    if(value && std::find(errors.begin(),errors.end(),value)==errors.end())errors.push_back(value);
}
void mg_begin_driver_operation() { mg_set_gl_error(getError()); }
bool mg_end_driver_operation(const char*) { const auto error=getError();mg_set_gl_error(error);return error==0; }

int main(){
    GLES.glEnable=enable;GLES.glDisable=disable;GLES.glIsEnabled=isEnabled;GLES.glGetError=getError;GLES.glGetIntegerv=getInt;
    MGContext context{};mg_enable_reset(&context.enable);g_current_ctx=&context;
    for(bool on:{false,true}){
        on?glEnable(GL_BLEND):glDisable(GL_BLEND);require(errors.empty()&&glIsEnabled(GL_BLEND)==on,"合法Blend基础状态");
        for(GLenum invalid:{GLenum(0xdeadbeef),GLenum(0),GLenum(GL_TEXTURE0),GLenum(GL_MAJOR_VERSION)}){
            for(int entry=0;entry<3;++entry){
                errors.clear();const auto oldSetters=setters;
                if(entry==0)glEnable(invalid);else if(entry==1)glDisable(invalid);else require(glIsEnabled(invalid)==GL_FALSE,"invalid query返回false");
                require(errors==std::vector<GLenum>{GL_INVALID_ENUM},"公共scalar无效enum必须置准确错误");
                require(setters==oldSetters&&glIsEnabled(GL_BLEND)==on,"invalid不转发/不改变Blend");
                errors.clear();driverError=GL_INVALID_VALUE;
                if(entry==0)glEnable(invalid);else if(entry==1)glDisable(invalid);else glIsEnabled(invalid);
                require(errors==std::vector<GLenum>{GL_INVALID_VALUE,GL_INVALID_ENUM},"旧driver错先保留，新enum错不吞");
            }
        }
        // 合法但无实现的legacy/已宣告extension保留原限制，不向GLES错误分类，也不伪造tracked状态。
        errors.clear();const auto beforeQueries=queries,beforeSetters=setters;
        for(GLenum known:{GLenum(GL_ALPHA_TEST),GLenum(GL_LIGHTING),GLenum(GL_FRAGMENT_PROGRAM_ARB),GLenum(GL_COLOR_SUM)}){
            glEnable(known);glDisable(known);require(glIsEnabled(known)==GL_FALSE,"未实现合法cap仍非tracked");
        }
        require(errors.empty()&&queries==beforeQueries&&setters==beforeSetters,"合法未实现项不误判/不扩实现");
        require(glIsEnabled(GL_VERTEX_ARRAY)==GL_FALSE&&errors.empty(),"legacy client query不误判为scalar setter参数");
        glEnable(GL_VERTEX_ARRAY);require(errors==std::vector<GLenum>{GL_INVALID_ENUM},"client state不能作为scalar enable参数");
        errors.clear();const auto prior=setters;
        glEnable(futureDriverCap);glDisable(futureDriverCap);require(glIsEnabled(futureDriverCap)==GL_FALSE,"未来driver cap保持原未跟踪限制");
        require(errors.empty()&&setters==prior,"当前driver接受的未知extension不能误Invalid/不能增加setter backing");
        std::printf("future-driver fixture cap=f123 accepted=1 tracked=0 setterDelta=0 blend=%u\n",unsigned(on));
        require(mg_enable_get(GL_MAJOR_VERSION,0)==GL_FALSE&&errors.empty(),"内部未匹配getter不能置GL错误");
        GLboolean out=GL_TRUE;require(!mg_enable_query(GL_MAJOR_VERSION,&out)&&errors.empty(),"多pname辅助仍返回未匹配");
    }
    g_current_ctx=nullptr;std::puts("scalar capability error/known-limit/driver/old-error production contracts passed");return 0;
}

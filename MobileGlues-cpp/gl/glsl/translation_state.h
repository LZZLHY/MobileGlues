#pragma once
#include <map>
#include <string>
#include <vector>
#include <cstdint>

// Serialized with cached ESSL as comments; cache hits preserve the contract.
struct mg_glsl_binding {
    std::string kind; // sampler, image, ubo, ssbo
    std::string name;
    unsigned binding = 0;
    unsigned count = 1;
};
struct mg_glsl_metadata {
    std::vector<mg_glsl_binding> bindings;
    std::vector<std::string> buffer_samplers;
    // glslang求值后的desktop uniform常量按叶字段记录：array/struct展开为GL查询名称，
    // vector/matrix保持一项、column-major顺序；每项只有一个element。成功link后写回真实driver。
    // float用IEEE32位保存，int/uint/bool用32位值保存，磁盘cache不重新求值或发生文本舍入。
    struct initializer {
        std::string name;
        char kind = 0; // f=float，i=int/bool，u=uint；矩阵只允许f。
        unsigned columns = 1, rows = 1;
        std::vector<std::uint32_t> bits;
        bool operator==(const initializer&) const = default;
    };
    std::vector<initializer> initializers;
    // 转译ESSL必须含完整BEGIN/count/END。缺项、重复或损坏使shader保持失败，不能默认为零初值。
    // 直通合法ESSL源码不经过此serialized contract，也不会宣称desktop initializer语义。
    std::string initializer_failure;
};
using mg_frag_bindings = std::map<std::string, unsigned>;
std::string mg_translation_error();
void mg_read_translation_metadata(const std::string& essl, mg_glsl_metadata& metadata);

#pragma once
#include <map>
#include <string>
#include <vector>

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
};
using mg_frag_bindings = std::map<std::string, unsigned>;
std::string mg_translation_error();
void mg_read_translation_metadata(const std::string& essl, mg_glsl_metadata& metadata);

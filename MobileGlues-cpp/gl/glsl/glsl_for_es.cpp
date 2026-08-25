// MobileGlues - gl/glsl/glsl_for_es.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header
#include "glsl_for_es.h"

#include <glslang/Public/ShaderLang.h>
#include <glslang/Include/Types.h>
#include <glslang/Public/ShaderLang.h>
#include <spirv_cross/spirv_cross_c.h>
#include <iostream>
#include <fstream>
#include "../log.h"
#include "glslang/SPIRV/GlslangToSpv.h"
#include <string>
#include <regex>
#include <strstream>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <vector>
#include "cache.h"
#include "../../version.h"

#define DEBUG 0

// Cache-key component describing the *behaviour* of this translator, kept
// separate from the MobileGlues release version.
//
// The key built in GLSLtoGLSLES() already carries MAJOR.MINOR.REVISION, and that
// is not sufficient: changing what this file emits does not require a version
// bump, and during development it never gets one. The failure mode is not a
// stale-looking log line. It is a shader that silently keeps the translation it
// was given before the change, so the new code appears to do nothing at all.
//
// That is precisely what happened when the fragment-output flattening below was
// restored. AMCL's preset sets maxGlslCacheSize to 32 MB, so the on-disk cache
// still held entries written before the pass existed; nearly every shader was
// served a translation without it, only the few that missed the cache showed the
// new output, and the driver rejected the cached ones with the very error the
// pass exists to prevent.
//
// ⇒ Bump this whenever a change alters the ESSL this file produces. Entries
// persisted under an older value then stop matching and are evicted by the
// cache's normal LRU, so no user-visible "clear your cache" step is required --
// which matters because an installed application's cache directory outlives its
// upgrade, and asking users to clear it is not a fix.
//
// Cost, stated plainly: the first launch after a bump re-translates every shader
// because the whole persisted cache misses. That is one slower startup, and it
// is the price of never serving a translation the current code would not produce.
#define GLSL_TRANSLATOR_REVISION 1

static TBuiltInResource InitResources() {
    TBuiltInResource Resources{};

    Resources.maxLights = 32;
    Resources.maxClipPlanes = 6;
    Resources.maxTextureUnits = 32;
    Resources.maxTextureCoords = 32;
    Resources.maxVertexAttribs = 64;
    Resources.maxVertexUniformComponents = 4096;
    Resources.maxVaryingFloats = 64;
    Resources.maxVertexTextureImageUnits = 32;
    Resources.maxCombinedTextureImageUnits = 80;
    Resources.maxTextureImageUnits = 32;
    Resources.maxFragmentUniformComponents = 4096;
    Resources.maxDrawBuffers = 32;
    Resources.maxVertexUniformVectors = 128;
    Resources.maxVaryingVectors = 8;
    Resources.maxFragmentUniformVectors = 16;
    Resources.maxVertexOutputVectors = 16;
    Resources.maxFragmentInputVectors = 15;
    Resources.minProgramTexelOffset = -8;
    Resources.maxProgramTexelOffset = 7;
    Resources.maxClipDistances = 8;
    Resources.maxComputeWorkGroupCountX = 65535;
    Resources.maxComputeWorkGroupCountY = 65535;
    Resources.maxComputeWorkGroupCountZ = 65535;
    Resources.maxComputeWorkGroupSizeX = 1024;
    Resources.maxComputeWorkGroupSizeY = 1024;
    Resources.maxComputeWorkGroupSizeZ = 64;
    Resources.maxComputeUniformComponents = 1024;
    Resources.maxComputeTextureImageUnits = 16;
    Resources.maxComputeImageUniforms = 8;
    Resources.maxComputeAtomicCounters = 8;
    Resources.maxComputeAtomicCounterBuffers = 1;
    Resources.maxVaryingComponents = 60;
    Resources.maxVertexOutputComponents = 64;
    Resources.maxGeometryInputComponents = 64;
    Resources.maxGeometryOutputComponents = 128;
    Resources.maxFragmentInputComponents = 128;
    Resources.maxImageUnits = 8;
    Resources.maxCombinedImageUnitsAndFragmentOutputs = 8;
    Resources.maxCombinedShaderOutputResources = 8;
    Resources.maxImageSamples = 0;
    Resources.maxVertexImageUniforms = 0;
    Resources.maxTessControlImageUniforms = 0;
    Resources.maxTessEvaluationImageUniforms = 0;
    Resources.maxGeometryImageUniforms = 0;
    Resources.maxFragmentImageUniforms = 8;
    Resources.maxCombinedImageUniforms = 8;
    Resources.maxGeometryTextureImageUnits = 16;
    Resources.maxGeometryOutputVertices = 256;
    Resources.maxGeometryTotalOutputComponents = 1024;
    Resources.maxGeometryUniformComponents = 1024;
    Resources.maxGeometryVaryingComponents = 64;
    Resources.maxTessControlInputComponents = 128;
    Resources.maxTessControlOutputComponents = 128;
    Resources.maxTessControlTextureImageUnits = 16;
    Resources.maxTessControlUniformComponents = 1024;
    Resources.maxTessControlTotalOutputComponents = 4096;
    Resources.maxTessEvaluationInputComponents = 128;
    Resources.maxTessEvaluationOutputComponents = 128;
    Resources.maxTessEvaluationTextureImageUnits = 16;
    Resources.maxTessEvaluationUniformComponents = 1024;
    Resources.maxTessPatchComponents = 120;
    Resources.maxPatchVertices = 32;
    Resources.maxTessGenLevel = 64;
    Resources.maxViewports = 16;
    Resources.maxVertexAtomicCounters = 0;
    Resources.maxTessControlAtomicCounters = 0;
    Resources.maxTessEvaluationAtomicCounters = 0;
    Resources.maxGeometryAtomicCounters = 0;
    Resources.maxFragmentAtomicCounters = 8;
    Resources.maxCombinedAtomicCounters = 8;
    Resources.maxAtomicCounterBindings = 1;
    Resources.maxVertexAtomicCounterBuffers = 0;
    Resources.maxTessControlAtomicCounterBuffers = 0;
    Resources.maxTessEvaluationAtomicCounterBuffers = 0;
    Resources.maxGeometryAtomicCounterBuffers = 0;
    Resources.maxFragmentAtomicCounterBuffers = 1;
    Resources.maxCombinedAtomicCounterBuffers = 1;
    Resources.maxAtomicCounterBufferSize = 16384;
    Resources.maxTransformFeedbackBuffers = 4;
    Resources.maxTransformFeedbackInterleavedComponents = 64;
    Resources.maxCullDistances = 8;
    Resources.maxCombinedClipAndCullDistances = 8;
    Resources.maxSamples = 4;
    Resources.maxMeshOutputVerticesNV = 256;
    Resources.maxMeshOutputPrimitivesNV = 512;
    Resources.maxMeshWorkGroupSizeX_NV = 32;
    Resources.maxMeshWorkGroupSizeY_NV = 1;
    Resources.maxMeshWorkGroupSizeZ_NV = 1;
    Resources.maxTaskWorkGroupSizeX_NV = 32;
    Resources.maxTaskWorkGroupSizeY_NV = 1;
    Resources.maxTaskWorkGroupSizeZ_NV = 1;
    Resources.maxMeshViewCountNV = 4;

    Resources.limits.nonInductiveForLoops = true;
    Resources.limits.whileLoops = true;
    Resources.limits.doWhileLoops = true;
    Resources.limits.generalUniformIndexing = true;
    Resources.limits.generalAttributeMatrixVectorIndexing = true;
    Resources.limits.generalVaryingIndexing = true;
    Resources.limits.generalSamplerIndexing = true;
    Resources.limits.generalVariableIndexing = true;
    Resources.limits.generalConstantMatrixVectorIndexing = true;

    // Ten fields this table never set, left at 0 by the value-initialisation
    // above. Nine are mesh-shader limits that glslang only reads when a shader
    // asks for them, so 0 was harmless. maxDualSourceDrawBuffersEXT was not:
    // glslang emits
    //     mediump vec4 gl_SecondaryFragDataEXT[gl_MaxDualSourceDrawBuffersEXT];
    // into the ESSL built-in block, and an array sized 0 fails to parse -- which
    // fails the whole built-in table, so every shader routed through glslang was
    // rejected with "unsupported shader version". It only showed on a context
    // whose ESSL version is below the shader's, since a shader the driver can
    // take is passed straight through; ANGLE presents ES 3.1, so turning ANGLE on
    // meant nothing using #version 320 es could compile at all.
    //
    // The values are glslang's own defaults (glslang/ResourceLimits.cpp).
    Resources.maxDualSourceDrawBuffersEXT = 1;
    Resources.maxMeshOutputVerticesEXT = 256;
    Resources.maxMeshOutputPrimitivesEXT = 256;
    Resources.maxMeshWorkGroupSizeX_EXT = 128;
    Resources.maxMeshWorkGroupSizeY_EXT = 128;
    Resources.maxMeshWorkGroupSizeZ_EXT = 128;
    Resources.maxTaskWorkGroupSizeX_EXT = 128;
    Resources.maxTaskWorkGroupSizeY_EXT = 128;
    Resources.maxTaskWorkGroupSizeZ_EXT = 128;
    Resources.maxMeshViewCountEXT = 4;

    return Resources;
}

int getGLSLVersion(const char* glsl_code) {
    std::string code(glsl_code);
    static std::regex version_pattern(R"(#version\s+(\d{3}))");
    std::smatch match;
    if (std::regex_search(code, match, version_pattern)) {
        return std::stoi(match[1].str());
    }

    return -1;
}

std::string forceSupporterOutput(const std::string& glslCode) {
    bool hasPrecisionFloat =
        glslCode.find("precision ") != std::string::npos && glslCode.find("float;") != std::string::npos;
    bool hasPrecisionInt =
        glslCode.find("precision ") != std::string::npos && glslCode.find("int;") != std::string::npos;

    std::string result = glslCode;
    std::string precisionFloat;
    std::string precisionInt;

    if (hasPrecisionFloat && hasPrecisionInt) {
        std::istringstream iss(result);
        std::vector<std::string> lines;
        std::string line;
        while (std::getline(iss, line)) {
            bool isPrecisionLine = (line.find("precision ") != std::string::npos) &&
                                   (line.find("float;") != std::string::npos || line.find("int;") != std::string::npos);
            if (!isPrecisionLine) {
                lines.push_back(line);
            }
        }
        result.clear();
        for (size_t i = 0; i < lines.size(); ++i) {
            if (i != 0) result += '\n';
            result += lines[i];
        }
        precisionFloat = "precision highp float;\n";
        precisionInt = "precision highp int;\n";
    } else {
        precisionFloat = hasPrecisionFloat ? "" : "precision highp float;\n";
        precisionInt = hasPrecisionInt ? "" : "precision highp int;\n";
    }

    size_t lastExtensionPos = result.rfind("#extension");
    size_t insertionPos = 0;

    if (lastExtensionPos != std::string::npos) {
        size_t nextNewline = result.find('\n', lastExtensionPos);
        if (nextNewline != std::string::npos) {
            insertionPos = nextNewline + 1;
        } else {
            insertionPos = result.length();
        }
    } else {
        size_t firstNewline = result.find('\n');
        if (firstNewline != std::string::npos) {
            insertionPos = firstNewline + 1;
        } else {
            result = precisionFloat + precisionInt + result;
            return result;
        }
    }

    result.insert(insertionPos, precisionFloat + precisionInt);
    return result;
}

std::string removeLayoutBinding(const std::string& glslCode) {
    static std::regex bindingRegex(R"(layout\s*\(\s*binding\s*=\s*\d+\s*\)\s*)");
    std::string result = std::regex_replace(glslCode, bindingRegex, "");
    static std::regex bindingRegex2(R"(layout\s*\(\s*binding\s*=\s*\d+\s*,)");
    result = std::regex_replace(result, bindingRegex2, "layout(");
    return result;
}

void trim(std::string& str) {
    str.erase(str.begin(), std::find_if(str.begin(), str.end(), [](int ch) { return !std::isspace(ch); }));
    str.erase(std::find_if(str.rbegin(), str.rend(), [](int ch) { return !std::isspace(ch); }).base(), str.end());
}

// Process all uniform declarations into `uniform <precision> <type> <name>;` form
std::string process_uniform_declarations(const std::string& glslCode) {
    std::string result;
    size_t scan_pos = 0;
    size_t chunk_start = 0;
    const size_t length = glslCode.length();
    const std::vector<std::string> precision_kws = {"highp", "lowp", "mediump"};

    result.reserve(glslCode.length());

    while (scan_pos < length) {
        if (glslCode.compare(scan_pos, 7, "uniform") == 0) {
            if (scan_pos > chunk_start) {
                result.append(glslCode, chunk_start, scan_pos - chunk_start);
            }

            const size_t decl_start = scan_pos;
            scan_pos += 7; // Skip "uniform"

            std::string precision, type;
            bool found_precision = false;

            while (scan_pos < length) {
                while (scan_pos < length && std::isspace(glslCode[scan_pos]))
                    ++scan_pos;

                for (const auto& kw : precision_kws) {
                    if (glslCode.compare(scan_pos, kw.length(), kw) == 0) {
                        precision = " " + kw;
                        scan_pos += kw.length();
                        found_precision = true;
                        break;
                    }
                }
                if (found_precision) break;

                const size_t type_start = scan_pos;
                while (scan_pos < length && (std::isalnum(glslCode[scan_pos]) || glslCode[scan_pos] == '_')) {
                    ++scan_pos;
                }
                type = glslCode.substr(type_start, scan_pos - type_start);
                break;
            }

            while (scan_pos < length) {
                while (scan_pos < length && std::isspace(glslCode[scan_pos]))
                    ++scan_pos;

                bool found = false;
                for (const auto& kw : precision_kws) {
                    if (glslCode.compare(scan_pos, kw.length(), kw) == 0) {
                        if (precision.empty()) precision = " " + kw;
                        scan_pos += kw.length();
                        found = true;
                        break;
                    }
                }
                if (!found) break;
            }

            if (type.empty()) {
                const size_t type_start = scan_pos;
                while (scan_pos < length && (std::isalnum(glslCode[scan_pos]) || glslCode[scan_pos] == '_')) {
                    ++scan_pos;
                }
                type = glslCode.substr(type_start, scan_pos - type_start);
            }

            while (scan_pos < length && std::isspace(glslCode[scan_pos]))
                ++scan_pos;
            const size_t name_start = scan_pos;
            while (scan_pos < length && (std::isalnum(glslCode[scan_pos]) || glslCode[scan_pos] == '_')) {
                ++scan_pos;
            }
            const std::string name = glslCode.substr(name_start, scan_pos - name_start);

            size_t decl_end = glslCode.find(';', scan_pos);
            if (decl_end == std::string::npos)
                decl_end = length;
            else
                ++decl_end;
            const bool has_initializer = (glslCode.find('=', scan_pos) < decl_end);
            if (has_initializer) {
                result.append("uniform").append(precision).append(" ").append(type).append(" ").append(name).append(
                    ";");
            } else {
                result.append(glslCode, decl_start, decl_end - decl_start);
            }

            scan_pos = chunk_start = decl_end;
        } else {
            ++scan_pos;
        }
    }

    if (chunk_start < length) {
        result.append(glslCode, chunk_start, length - chunk_start);
    }

    return result;
}

std::string processOutColorLocations(const std::string& glslCode) {
    const static std::regex pattern(R"(\n(out highp vec4 outColor)(\d+);)");
    const std::string replacement = "\nlayout(location=$2) $1$2;";
    return std::regex_replace(glslCode, pattern, replacement);
}

// ---------------------------------------------------------------------------
// Fragment outputs declared as arrays and indexed by a non-constant expression
// ---------------------------------------------------------------------------
// GLSL ES forbids this outright -- "Arrays of fragment outputs may only be
// indexed by a constant integral expression" (ESSL 3.00 / 3.20 section 4.3.6)
// -- while desktop GLSL permits it. A shader can therefore leave SPIRV-Cross as
// perfectly valid desktop GLSL and still be rejected by the GLES driver's front
// end. On Maleoon that reads:
//
//   S0015: Outputs declared as arrays may only be indexed by a constant
//          integral expression.
//
// Minecraft 26.3 trips this on nearly every fragment shader, because its new
// order-independent-transparency include writes the transmittance coefficients
// through a loop (assets/minecraft/shaders/include/oit_add_transmittance.glsl):
//
//   layout(location = 0) out vec4 coeff[COEFF_ATTACHMENT_COUNT];
//   for (int a = 0; a < COEFF_ATTACHMENT_COUNT; a++)
//       for (int i = 0; i < 4; i++)
//           coeff[a][i] = coefficients[a * 4 + i];
//
// Neither stage of this translator can absorb it: the loop survives the SPIR-V
// round trip (there is no SPIRV-Tools in this tree to unroll it), and
// SPIRV-Cross has no option for flattening plain I/O arrays --
// FORCE_FLATTENED_IO_BLOCKS only covers interface blocks.
//
// So rewrite the declaration rather than the code that uses it:
//
//   layout(location = L) out T name[N];        // illegal to index dynamically
//     ->  layout(location = L+k) out T name__mg_out_k;   // k = 0 .. N-1
//         T name[N];                           // ordinary global: legal
//         void main() { mg__orig_main(); name__mg_out_k = name[k]; ... }
//
// Every existing read and write of name[...] stays exactly as emitted, because
// indexing a non-output array dynamically is legal. The values reach the real
// outputs from a wrapper that runs after the original entry point returns.
//
// Why a wrapper instead of appending the copies to the end of main(): the
// original main() may `return` early, and a `discard` must skip the copy
// entirely. Wrapping gets both right without analysing control flow.
//
// This is a conformance fix in the GLSL-to-ESSL translator, not driver policy:
// the restriction lives in the ES specification and applies to every ES target.
// It is therefore deliberately not guarded by MG_PLATFORM_OHOS, and is an
// upstream candidate.
//
// ⚠️ Observability: LOG_W and LOG_E compile to nothing unless DEBUG or
// GLOBAL_DEBUG is set (gl/log.h), so every event a shipping build must be able
// to prove or refute uses LOG_I / LOG_W_FORCE instead. The literals below are
// also the artefact-level evidence that this pass is present in a built .so:
// the functions have internal linkage and may be inlined away, so a symbol-name
// probe is not a reliable check -- a string scan of the .so is.
namespace {

// Three states, deliberately not a bool. `Unparseable` has to stay distinct
// from `Dynamic` because the correct response differs: a genuinely dynamic
// index is what this pass exists to rewrite, whereas an index this pass cannot
// read means the translation unit is not in the shape assumed here -- and
// restructuring a shader on the strength of a failed parse is the fail-open
// behaviour that would turn one unreadable declaration into a broken shader.
enum class IndexExpressionKind {
    Literal,
    Dynamic,
    Unparseable,
};

IndexExpressionKind classifyIndexExpression(const std::string& code, size_t open_bracket) {
    const size_t close = code.find(']', open_bracket);
    if (close == std::string::npos) return IndexExpressionKind::Unparseable;
    bool digit_seen = false;
    for (size_t i = open_bracket + 1; i < close; ++i) {
        const auto c = static_cast<unsigned char>(code[i]);
        if (std::isdigit(c)) {
            digit_seen = true;
        } else if (!std::isspace(c)) {
            return IndexExpressionKind::Dynamic;
        }
    }
    // "[]" and "[ ]" carry no index at all: unreadable rather than literal zero.
    return digit_seen ? IndexExpressionKind::Literal : IndexExpressionKind::Unparseable;
}

struct DynamicUseScan {
    bool dynamic = false;
    bool unparseable = false;
};

// Is `name` indexed by anything other than a literal after its declaration?
// Whole-identifier matching only, so `coeffScale[i]` never counts as a use of
// `coeff`, and `mg_coeff[i]` does not either.
DynamicUseScan scanForDynamicUse(const std::string& code, const std::string& name, size_t from) {
    DynamicUseScan scan;
    size_t pos = from;
    while ((pos = code.find(name, pos)) != std::string::npos) {
        const size_t after = pos + name.length();
        const bool left_is_boundary =
            (pos == 0) || !(std::isalnum(static_cast<unsigned char>(code[pos - 1])) || code[pos - 1] == '_');
        size_t cursor = after;
        while (cursor < code.length() && std::isspace(static_cast<unsigned char>(code[cursor]))) ++cursor;
        if (left_is_boundary && cursor < code.length() && code[cursor] == '[') {
            switch (classifyIndexExpression(code, cursor)) {
            case IndexExpressionKind::Dynamic:
                scan.dynamic = true;
                break;
            case IndexExpressionKind::Unparseable:
                scan.unparseable = true;
                break;
            case IndexExpressionKind::Literal:
                break;
            }
        }
        pos = after;
    }
    return scan;
}

struct ArrayOutputDecl {
    size_t pos = 0;
    size_t length = 0;
    int location = 0;
    int count = 0;
    std::string qualifiers;
    std::string precision;
    std::string type;
    std::string name;
};

// Upper bound on the outputs a single declaration may expand into. It mirrors
// TBuiltInResource::maxDrawBuffers set above; a larger array cannot be a legal
// fragment output set on any target this translator emits for, so such a
// declaration is treated as one this pass does not understand rather than
// silently emitting 33+ outputs.
constexpr int kMaxFragmentOutputArraySize = 32;

const char* const kOriginalEntryPoint = "mg__orig_main";

// Interpolation and auxiliary storage qualifiers are captured so they can be
// carried onto every scalar output the rewrite emits. Dropping `flat` would
// silently change interpolation, and for integer outputs `flat` is mandatory --
// losing it turns a legal shader into an illegal one.
std::vector<ArrayOutputDecl> collectRewritableArrayOutputs(const std::string& essl) {
    static const std::regex decl_pattern(
        R"(layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*)"
        R"(((?:(?:flat|smooth|noperspective|centroid|sample)\s+)*))"
        R"(out\s+((?:highp|mediump|lowp)\s+)?(\w+)\s+(\w+)\s*\[\s*(\d+)\s*\]\s*;)");

    std::vector<ArrayOutputDecl> decls;
    const auto end = std::sregex_iterator();
    for (auto it = std::sregex_iterator(essl.begin(), essl.end(), decl_pattern); it != end; ++it) {
        const std::smatch& m = *it;
        ArrayOutputDecl decl;
        decl.pos = static_cast<size_t>(m.position(0));
        decl.length = static_cast<size_t>(m.length(0));
        decl.location = std::atoi(m[1].str().c_str());
        decl.qualifiers = m[2].str();
        decl.precision = m[3].matched ? m[3].str() : "";
        decl.type = m[4].str();
        decl.name = m[5].str();
        decl.count = std::atoi(m[6].str().c_str());
        trim(decl.qualifiers);
        trim(decl.precision);

        if (decl.count <= 0 || decl.count > kMaxFragmentOutputArraySize) {
            LOG_W_FORCE("[Shader] Not flattening fragment output array %s[%d]: size outside 1..%d",
                        decl.name.c_str(), decl.count, kMaxFragmentOutputArraySize)
            continue;
        }
        const DynamicUseScan scan = scanForDynamicUse(essl, decl.name, decl.pos + decl.length);
        if (scan.unparseable) {
            LOG_W_FORCE("[Shader] Not flattening fragment output array %s[%d]: an index expression could not be read",
                        decl.name.c_str(), decl.count)
            continue;
        }
        // Already conforming shaders are passed through byte for byte.
        if (!scan.dynamic) continue;
        decls.push_back(std::move(decl));
    }
    return decls;
}

// Report array-output declarations that are dynamically indexed but whose shape
// the pattern above does not cover -- an unfamiliar qualifier, a non-literal
// array size, more than one dimension. Without this the driver would reject
// such a shader with nothing pointing back to this pass, which is precisely how
// the original defect stayed unexplained for as long as it did.
void warnAboutUnhandledArrayOutputs(const std::string& essl, const std::vector<ArrayOutputDecl>& handled) {
    static const std::regex loose_pattern(R"(\bout\b[^;{}]{0,200}?\b(\w+)\s*\[\s*(\w+)\s*\]\s*;)");

    const auto end = std::sregex_iterator();
    for (auto it = std::sregex_iterator(essl.begin(), essl.end(), loose_pattern); it != end; ++it) {
        const std::smatch& m = *it;
        const auto pos = static_cast<size_t>(m.position(0));
        const auto length = static_cast<size_t>(m.length(0));
        const bool covered = std::any_of(handled.begin(), handled.end(), [&](const ArrayOutputDecl& decl) {
            return (decl.pos < pos + length) && (pos < decl.pos + decl.length);
        });
        if (covered) continue;
        const std::string name = m[1].str();
        if (!scanForDynamicUse(essl, name, pos + length).dynamic) continue;
        LOG_W_FORCE("[Shader] Fragment output array %s is indexed dynamically but its declaration is not in a shape "
                    "this translator can rewrite; the driver will reject it (ESSL 4.3.6)",
                    name.c_str())
    }
}

std::string flattenDynamicFragmentOutputArrays(const std::string& essl, GLenum shaderType, int& rewritten) {
    rewritten = 0;
    if (shaderType != GL_FRAGMENT_SHADER) return essl;
    if (essl.find("out") == std::string::npos) return essl;

    const std::vector<ArrayOutputDecl> decls = collectRewritableArrayOutputs(essl);
    warnAboutUnhandledArrayOutputs(essl, decls);
    if (decls.empty()) return essl;

    // The wrapper needs a name of its own. Bail out rather than shadow an
    // existing definition, however unlikely a collision is in generated code.
    if (essl.find(kOriginalEntryPoint) != std::string::npos) {
        LOG_W_FORCE("[Shader] Not flattening fragment output arrays: '%s' already exists in the translated shader",
                    kOriginalEntryPoint)
        return essl;
    }

    std::string result;
    result.reserve(essl.length() + decls.size() * 128);
    std::string copy_back;
    size_t cursor = 0;
    for (const ArrayOutputDecl& decl : decls) {
        result.append(essl, cursor, decl.pos - cursor);

        const std::string qualifiers = decl.qualifiers.empty() ? "" : decl.qualifiers + " ";
        const std::string precision = decl.precision.empty() ? "" : decl.precision + " ";
        for (int k = 0; k < decl.count; ++k) {
            const std::string element = decl.name + "__mg_out_" + std::to_string(k);
            result += "layout(location = " + std::to_string(decl.location + k) + ") " + qualifiers + "out " +
                      precision + decl.type + " " + element + ";\n";
            copy_back += "    " + element + " = " + decl.name + "[" + std::to_string(k) + "];\n";
        }
        // The name the shader body already uses now denotes an ordinary global
        // array, which may be indexed with anything.
        result += precision + decl.type + " " + decl.name + "[" + std::to_string(decl.count) + "];";

        cursor = decl.pos + decl.length;
    }
    result.append(essl, cursor, std::string::npos);

    // Move the original entry point aside and copy the staging arrays out after
    // it returns. If the entry point is not in the expected shape, abandon the
    // whole rewrite: rewritten declarations with no copy-back emitted would be
    // worse than not touching the shader at all.
    static const std::regex main_pattern(R"(\bvoid\s+main\s*\(\s*\))");
    std::smatch main_match;
    if (!std::regex_search(result, main_match, main_pattern)) {
        LOG_W_FORCE("[Shader] Cannot flatten dynamically indexed fragment outputs: no 'void main()' found; leaving "
                    "the shader untouched")
        return essl;
    }
    // Read both out of the match before mutating `result`: the match object
    // holds iterators into the very string that replace() is about to move.
    const auto main_pos = static_cast<size_t>(main_match.position(0));
    const auto main_length = static_cast<size_t>(main_match.length(0));
    result.replace(main_pos, main_length, std::string("void ") + kOriginalEntryPoint + "()");
    result += "\nvoid main()\n{\n    ";
    result += kOriginalEntryPoint;
    result += "();\n";
    result += copy_back;
    result += "}\n";

    // Logged only once the rewrite is known to have been completed. Reporting a
    // flattened declaration and then bailing out above would leave a log that
    // contradicts the shader that was actually handed to the driver.
    for (const ArrayOutputDecl& decl : decls) {
        // Named locals, not temporaries: LOG_I expands to more than one
        // statement, so a `(s + " ").c_str()` argument would already be dangling
        // by the time the second one runs.
        const std::string qualifiers = decl.qualifiers.empty() ? std::string() : decl.qualifiers + " ";
        const std::string precision = decl.precision.empty() ? std::string() : decl.precision + " ";
        LOG_I("[Shader] Flattened dynamically indexed fragment output array: %s%s%s %s[%d] @location=%d",
              qualifiers.c_str(), precision.c_str(), decl.type.c_str(), decl.name.c_str(), decl.count, decl.location)
    }
    rewritten = static_cast<int>(decls.size());
    return result;
}

}  // namespace

std::string GLSLtoGLSLES(const char* glsl_code, GLenum glsl_type, uint essl_version, uint glsl_version,
                         int& return_code) {
    std::string sha256_string(glsl_code);
    sha256_string += "\n//" + std::to_string(MAJOR) + "." + std::to_string(MINOR) + "." + std::to_string(REVISION) +
                     "|" + std::to_string(essl_version) + "|t" + std::to_string(GLSL_TRANSLATOR_REVISION);
    const char* cachedESSL = Cache::get_instance().get(sha256_string.c_str());
    if (cachedESSL) {
        LOG_D("GLSL Hit Cache:\n%s\n-->\n%s", glsl_code, cachedESSL)
        return_code = 0;
        return (char*)cachedESSL;
    }

    return_code = -1;
    // std::string converted = glsl_version<140? GLSLtoGLSLES_1(glsl_code, glsl_type, essl_version,
    // return_code):GLSLtoGLSLES_2(glsl_code, glsl_type, essl_version, return_code);
    std::string converted = GLSLtoGLSLES_2(glsl_code, glsl_type, essl_version, return_code);
    if (return_code >= 0 && !converted.empty()) {
        converted = process_uniform_declarations(converted);
        Cache::get_instance().put(sha256_string.c_str(), converted.c_str());
    }

    return (return_code >= 0) ? converted : glsl_code;
}

std::string replace_line_starting_with(const std::string& glslCode, const std::string& starting,
                                       const std::string& substitution = "") {
    std::string result;
    size_t length = glslCode.size();
    size_t start = 0;
    size_t current = 0;

    auto append_chunk = [&](size_t end) {
        if (end > start) {
            result.append(glslCode, start, end - start);
        }
    };

    while (current < length) {
        // Skip whitespace at line begin
        size_t lineStart = current;
        while (current < length && (glslCode[current] == ' ' || glslCode[current] == '\t')) {
            current++;
        }

        // Check whether #line directive
        bool isLineDirective = false;
        if (current + 5 <= length && glslCode.compare(current, 5, "#line") == 0) {
            isLineDirective = true;
        }

        // Move to line end
        while (current < length && glslCode[current] != '\r' && glslCode[current] != '\n') {
            current++;
        }

        // Handle carriage return
        size_t newlineLength = 0;
        if (current < length) {
            if (glslCode[current] == '\r') {
                newlineLength = (current + 1 < length && glslCode[current + 1] == '\n') ? 2 : 1;
            } else {
                newlineLength = 1;
            }
        }

        if (isLineDirective) {
            // Find #line directive ->
            //  1. Append chunk
            append_chunk(lineStart); // from chunk_begin to before `#line`
            // 2. Skip this line (incl. \n)
            current += newlineLength;
            start = current; // 3. Starting from next line

            result += substitution;
        } else {
            // move to a new line
            current += newlineLength;
        }
    }

    // append last block
    append_chunk(current);
    return result;
}

static inline void replace_all(std::string& str, const std::string& from, const std::string& to) {
    size_t start_pos = 0;
    while ((start_pos = str.find(from, start_pos)) != std::string::npos) {
        str.replace(start_pos, from.length(), to);
        start_pos += to.length(); // Handles case where 'to' is a substring of 'from'
    }
}

static size_t find_insertion_point(const std::string& glsl) {
    size_t pos = 0;
    size_t insertion_point = 0;

    size_t version_pos = glsl.find("#version");
    if (version_pos != std::string::npos) {
        size_t version_end = glsl.find('\n', version_pos);
        if (version_end == std::string::npos) {
            version_end = glsl.length();
        } else {
            version_end++;
        }
        insertion_point = version_end;
        pos = version_end;
    } else {
        insertion_point = 0;
        pos = 0;
    }

    while (pos < glsl.length()) {
        size_t line_begin = pos;
        while (pos < glsl.length() && std::isspace(glsl[pos])) {
            pos++;
        }
        if (pos >= glsl.length()) break;

        if (glsl[pos] == '#') {
            pos++;
            while (pos < glsl.length() && std::isspace(glsl[pos])) {
                pos++;
            }
            if (glsl.compare(pos, 9, "extension") == 0) {
                size_t ext_end = glsl.find('\n', pos);
                if (ext_end == std::string::npos) {
                    ext_end = glsl.length();
                } else {
                    ext_end++;
                }
                insertion_point = ext_end;
                pos = ext_end;
            } else {
                break;
            }
        } else {
            break;
        }
    }

    return insertion_point;
}

void process_sampler_buffer(std::string& source) { // a simplized version, should be rewritten in the future
    if (source.find("isamplerBuffer") == std::string::npos) {
        return;
    }

    size_t pos = 0;
    while ((pos = source.find("isamplerBuffer", pos)) != std::string::npos) {
        source.replace(pos, 14, "isampler2D");
        pos += 11;
    }

    std::regex pattern(R"(texelFetch\s*\(\s*(\w+)\s*,\s*([^)]+?)\s*\))");
    source = std::regex_replace(source, pattern,
                                "texelFetch($1, ivec2(($2) % u_BufferTexWidth, ($2) / u_BufferTexWidth), 0)");

    const char* boundaryProtection = R"(
ivec2 bufferCoords(int index) {
    int width = u_BufferTexWidth;
    int x = index % width;
    int y = index / width;
    if (y >= u_BufferTexHeight) {
        y = u_BufferTexHeight - 1;
        x = width - 1;
    }
    return ivec2(x, y);
}
)";

    source = std::regex_replace(source, std::regex("texelFetch\\((\\w+)\\s*,\\s*ivec2\\(([^)]+)\\)\\s*,\\s*0\\)"),
                                "texelFetch($1, bufferCoords($2), 0)");

    size_t insertion_point = find_insertion_point(source);
    if (insertion_point != std::string::npos) {
        source.insert(insertion_point, boundaryProtection);
    }

    const char* uniformDecl = R"(
uniform int u_BufferTexWidth;
uniform int u_BufferTexHeight;
)";

    insertion_point = find_insertion_point(source);
    if (insertion_point != std::string::npos) {
        insertion_point = source.find('\n', insertion_point);
        if (insertion_point != std::string::npos) {
            source.insert(insertion_point + 1, uniformDecl);
        }
    }
}

static void inject_textureQueryLod(std::string& glsl) {
    const std::regex defRegex(R"(vec2\s+mg_textureQueryLod\s*\()", std::regex::ECMAScript);

    if (glsl.find("textureQueryLod") == std::string::npos) {
        return;
    }
    if (std::regex_search(glsl, defRegex)) {
        return;
    }

    const std::string textureQueryLodImpl = R"(
#define textureQueryLod mg_textureQueryLod

vec2 mg_textureQueryLod(sampler2D tex, vec2 uv) {
    vec2 texSizeF = vec2(textureSize(tex, 0));
    vec2 dFdx_uv = dFdx(uv * texSizeF);
    vec2 dFdy_uv = dFdy(uv * texSizeF);
    float maxDerivative = max(length(dFdx_uv), length(dFdy_uv));
    float lod = log2(maxDerivative);
    return vec2(lod);
}
)";

    size_t insertPos = find_insertion_point(glsl);
    glsl.insert(insertPos, "\n" + textureQueryLodImpl + "\n");
}

static inline void inject_temporal_filter(std::string& glsl) {
    const std::regex defRegex(R"(vec4\s+GI_TemporalFilter\s*\()", std::regex::ECMAScript);

    if (glsl.find("GI_TemporalFilter") == std::string::npos) {
        return;
    }
    if (std::regex_search(glsl, defRegex)) {
        return;
    }

    const std::regex uniformRegex(
        R"(^\s*(?:layout\s*\([^)]*\)\s*)?uniform\s+\w+(?:\s*\[\s*\d+\s*\])?\s+\w+(?:\s*\[\s*\d+\s*\])?\s*;.*$)",
        std::regex::ECMAScript | std::regex::multiline);
    std::sregex_iterator it(glsl.begin(), glsl.end(), uniformRegex);
    std::sregex_iterator end;
    size_t insertPos = 0;
    for (; it != end; ++it) {
        insertPos = it->position() + it->length();
    }

    const std::string GI_TemporalFilterImpl = R"(
vec4 GI_TemporalFilter() {
    vec2 uv = gl_FragCoord.xy / screenSize;
    uv += taaJitter * pixelSize;
    vec4 currentGI = texture(colortex0, uv);
    float depth = texture(depthtex0, uv).r;
    vec4 clipPos = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 viewPos = gbufferProjectionInverse * clipPos;
    viewPos /= viewPos.w;
    vec4 worldPos = gbufferModelViewInverse * viewPos;
    vec4 prevClipPos = gbufferPreviousProjection * (gbufferPreviousModelView * worldPos);
    prevClipPos /= prevClipPos.w;
    vec2 prevUV = prevClipPos.xy * 0.5 + 0.5;
    vec4 historyGI = texture(colortex1, prevUV);
    float difference = length(currentGI.rgb - historyGI.rgb);
    float thresholdValue = 0.1;
    float adaptiveBlend = mix(0.9, 0.0, smoothstep(thresholdValue, thresholdValue * 2.0, difference));
    vec4 filteredGI = mix(currentGI, historyGI, adaptiveBlend);
    if (difference > thresholdValue * 2.0) {
        filteredGI = currentGI;
    }
    return filteredGI;
}
)";
    glsl.insert(insertPos, "\n" + GI_TemporalFilterImpl + "\n");
}
#define xstr(s) str(s)
#define str(s) #s

void inject_mg_macro_definition(std::string& glslCode) {
    std::string macro_definitions =
        "\n#define MG_MOBILEGLUES\n"
        "#define MG_MOBILEGLUES_VERSION " xstr(MAJOR) xstr(MINOR) xstr(REVISION) xstr(PATCH) "\n";

    size_t versionPos = glslCode.rfind("#version");
    size_t insertionPos = 0;

    if (versionPos != std::string::npos) {
        size_t nextNewline = glslCode.find('\n', versionPos);
        insertionPos = (nextNewline != std::string::npos) ? nextNewline + 1 : glslCode.length();
    } else {
        size_t firstNewline = glslCode.find('\n');
        insertionPos = (firstNewline != std::string::npos) ? firstNewline + 1 : 0;
    }

    glslCode.insert(insertionPos, macro_definitions);
}

std::string preprocess_glsl(const std::string& glsl, GLenum shaderType) {
    std::string ret = glsl;
    // Remove lines beginning with `#line`
    ret = replace_line_starting_with(ret, "#line");
    // Act as if disable_GL_ARB_derivative_control is false
    replace_all(ret, "#ifdef GL_ARB_derivative_control", "#if 0");
    replace_all(ret, "#ifndef GL_ARB_derivative_control", "#if 1");

    // Polyfill transpose()
    replace_all(ret, "const mat3 rotInverse = transpose(rot);",
                "const mat3 rotInverse = mat3(rot[0][0], rot[1][0], rot[2][0], rot[0][1], rot[1][1], rot[2][1], "
                "rot[0][2], rot[1][2], rot[2][2]);");

    // GI_TemporalFilter injection
    inject_temporal_filter(ret);

    // textureQueryLod injection
    if (!g_gles_caps.GL_EXT_texture_query_lod) {
        inject_textureQueryLod(ret);
    }

    // MobileGlues macros injection
    inject_mg_macro_definition(ret);

    if (hardware->emulate_texture_buffer) {
        // Sampler buffer processing
        process_sampler_buffer(ret);
    }

    return ret;
}

int get_or_add_glsl_version(std::string& glsl) {
    int glsl_version = getGLSLVersion(glsl.c_str());
    if (glsl_version == -1) {
        glsl_version = 150;
        glsl.insert(0, "#version 150\n");
    } else if (glsl_version < 140) {
        // force upgrade glsl version
        glsl = replace_line_starting_with(glsl, "#version", "#version 150 compatibility\n");
        glsl_version = 150;
    }

    LOG_D("GLSL version: %d", glsl_version)
    return glsl_version;
}

std::vector<unsigned int> glsl_to_spirv(GLenum shader_type, int glsl_version, const char* const* shader_src,
                                        int& errc) {
    EShLanguage shader_language;
    switch (shader_type) {
    case GL_VERTEX_SHADER:
        shader_language = EShLanguage::EShLangVertex;
        break;
    case GL_FRAGMENT_SHADER:
        shader_language = EShLanguage::EShLangFragment;
        break;
    case GL_COMPUTE_SHADER:
        shader_language = EShLanguage::EShLangCompute;
        break;
    case GL_TESS_CONTROL_SHADER:
        shader_language = EShLanguage::EShLangTessControl;
        break;
    case GL_TESS_EVALUATION_SHADER:
        shader_language = EShLanguage::EShLangTessEvaluation;
        break;
    case GL_GEOMETRY_SHADER:
        shader_language = EShLanguage::EShLangGeometry;
        break;
    default:
        LOG_D("GLSL type not supported!")
        errc = -1;
        return {};
    }

    glslang::TShader shader(shader_language);
    shader.setStrings(shader_src, 1);

    using namespace glslang;
    shader.setEnvInput(EShSourceGlsl, shader_language, EShClientVulkan, glsl_version);
    shader.setEnvClient(EShClientOpenGL, EShTargetOpenGL_450);
    shader.setEnvTarget(EShTargetSpv, EShTargetSpv_1_5);
    shader.setAutoMapLocations(true);
    shader.setPreamble("#undef VULKAN\n");
    shader.setAutoMapBindings(true);

    TBuiltInResource TBuiltInResource_resources = InitResources();

    if (!shader.parse(&TBuiltInResource_resources, glsl_version, true, EShMsgDefault)) {
        LOG_D("GLSL Compiling ERROR: \n%s", shader.getInfoLog())
        errc = -1;
        return {};
    }
    LOG_D("GLSL Compiled.")

    glslang::TProgram program;
    program.addShader(&shader);

    if (!program.link(EShMsgDefault)) {
        LOG_D("Shader Linking ERROR: %s", program.getInfoLog())
        errc = -1;
        return {};
    }
    LOG_D("Shader Linked.")
    std::vector<unsigned int> spirv_code;
    glslang::SpvOptions spvOptions;
    spvOptions.disableOptimizer = false;
    glslang::GlslangToSpv(*program.getIntermediate(shader_language), spirv_code, &spvOptions);
    errc = 0;
    return spirv_code;
}

// The context owns the ParsedIR, the compiler and every string they hand back, and the only
// destroy used to sit past the early return. A shader the ES backend rejects is a normal
// outcome and failed translations are not cached, so that leaked the lot again on every
// resource-pack reload. Scoped so no exit can skip it.
namespace {
struct spvc_context_guard_t {
    spvc_context context = nullptr;
    spvc_context_guard_t() = default;
    ~spvc_context_guard_t() {
        if (context) spvc_context_destroy(context);
    }
    spvc_context_guard_t(const spvc_context_guard_t&) = delete;
    spvc_context_guard_t& operator=(const spvc_context_guard_t&) = delete;
};
} // namespace

// SPIRV-Cross throws internally and turns that into a result code at its C boundary; on failure
// it leaves the out-parameter untouched. Dropping the code therefore hands the next call a
// handle that was never written, which crashes rather than reporting anything.
static bool spvc_ok(spvc_context context, spvc_result res, const char* what) {
    if (res == SPVC_SUCCESS) {
        return true;
    }
    LOG_E("Error: %s failed in spirv-cross: %s", what, spvc_context_get_last_error_string(context))
    return false;
}

std::string spirv_to_essl(std::vector<unsigned int> spirv, uint essl_version, int& errc) {
    spvc_parsed_ir ir = nullptr;
    spvc_compiler compiler_glsl = nullptr;
    spvc_compiler_options options = nullptr;
    const char* result = nullptr;

    const SpvId* p_spirv = spirv.data();
    size_t word_count = spirv.size();

    LOG_D("spirv_code.size(): %d", spirv.size())

    // Declared before 'essl': the compiled source lives in context-owned memory and is only
    // copied out when the std::string is constructed, so the guard has to outlive it.
    spvc_context_guard_t guard;
    if (spvc_context_create(&guard.context) != SPVC_SUCCESS || !guard.context) {
        LOG_E("Error: could not create a spirv-cross context.")
        errc = -1;
        return "";
    }
    spvc_context context = guard.context;

    if (!spvc_ok(context, spvc_context_parse_spirv(context, p_spirv, word_count, &ir), "spvc_context_parse_spirv") ||
        !ir) {
        errc = -1;
        return "";
    }
    if (!spvc_ok(context,
                 spvc_context_create_compiler(context, SPVC_BACKEND_GLSL, ir, SPVC_CAPTURE_MODE_TAKE_OWNERSHIP,
                                              &compiler_glsl),
                 "spvc_context_create_compiler") ||
        !compiler_glsl) {
        errc = -1;
        return "";
    }
    if (!spvc_ok(context, spvc_compiler_create_compiler_options(compiler_glsl, &options),
                 "spvc_compiler_create_compiler_options") ||
        !options) {
        errc = -1;
        return "";
    }
    // A silently dropped GLSL_ES option would emit desktop GLSL and hand it straight to the
    // driver, so these are checked too.
    if (!spvc_ok(context,
                 spvc_compiler_options_set_uint(options, SPVC_COMPILER_OPTION_GLSL_VERSION,
                                                essl_version >= 300 ? essl_version : 300),
                 "spvc_compiler_options_set_uint") ||
        !spvc_ok(context, spvc_compiler_options_set_bool(options, SPVC_COMPILER_OPTION_GLSL_ES, SPVC_TRUE),
                 "spvc_compiler_options_set_bool") ||
        !spvc_ok(context, spvc_compiler_install_compiler_options(compiler_glsl, options),
                 "spvc_compiler_install_compiler_options")) {
        errc = -1;
        return "";
    }
    if (!spvc_ok(context, spvc_compiler_compile(compiler_glsl, &result), "spvc_compiler_compile") || !result) {
        errc = -1;
        return "";
    }

    std::string essl = result;

    errc = 0;
    return essl;
}

static bool glslang_inited = false;
std::string GLSLtoGLSLES_2(const char* glsl_code, GLenum glsl_type, uint essl_version, int& return_code) {
    std::string correct_glsl_str = preprocess_glsl(glsl_code, glsl_type);
    LOG_D("Firstly converted GLSL:\n%s", correct_glsl_str.c_str())
    int glsl_version = get_or_add_glsl_version(correct_glsl_str);

    if (!glslang_inited) {
        glslang::InitializeProcess();
        glslang_inited = true;
    }
    const char* s[] = {correct_glsl_str.c_str()};
    int errc = 0;
    std::vector<unsigned int> spirv_code = glsl_to_spirv(glsl_type, glsl_version, s, errc);
    if (errc != 0) {
        return_code = -1;
        return "";
    }
    errc = 0;
    std::string essl = spirv_to_essl(spirv_code, essl_version, errc);
    if (errc != 0) {
        return_code = -2;
        return "";
    }

    // Post-processing ESSL

    if (glsl_type != GL_COMPUTE_SHADER) {
        essl = removeLayoutBinding(essl);
    }
    essl = processOutColorLocations(essl);
    // Must run before forceSupporterOutput(): that pass inserts precision
    // statements relative to the shader header, and the rewrite below adds
    // output declarations those statements have to cover.
    {
        int flattened_outputs = 0;
        essl = flattenDynamicFragmentOutputArrays(essl, glsl_type, flattened_outputs);
        if (flattened_outputs > 0) {
            LOG_I("[Shader] Flattened %d dynamically indexed fragment output array declaration(s) for ESSL",
                  flattened_outputs)
        }
    }
    essl = forceSupporterOutput(essl);

    LOG_D("Originally GLSL to GLSL ES Complete: \n%s", essl.c_str())
    return_code = errc;
    return essl;
}

std::string GLSLtoGLSLES_1(const char* glsl_code, GLenum glsl_type, uint esversion, int& return_code) { // useless now
    /*
#if !defined(__APPLE__)
    LOG_W("Warning: use glsl optimizer to convert shader.")
    if (esversion < 300) esversion = 300;
    std::string result = MesaConvertShader(glsl_code, glsl_type == GL_VERTEX_SHADER ? GL_VERTEX_SHADER :
GL_FRAGMENT_SHADER, 460LL, esversion);

    return_code = 0;
    return result;
#else
    LOG_W_FORCE("Cannot convert glsl with version %d in MacOS/iOS", esversion);
    return std::string(glsl_code);
#endif
    */
}

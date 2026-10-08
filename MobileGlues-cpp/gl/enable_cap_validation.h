// Scalar enable参数分类数据；不添加backing或伪tracked状态。
// Khronos gl.xml EnableCap分组结合gl2.1 Enable/IsEnabled命令规范；client query值分开。
// gl.xml SHA b9ca2cfa5c676e901c20d34af3407f1687cde0f1336a5ff7a8974d04c7494ad3。
// 来源：https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/main/xml/gl.xml
// https://raw.githubusercontent.com/KhronosGroup/OpenGL-Refpages/main/gl2.1/glEnable.xml
// https://raw.githubusercontent.com/KhronosGroup/OpenGL-Refpages/main/gl2.1/glIsEnabled.xml
// 另含MG已宣告ARB_fragment_program规范的FRAGMENT_PROGRAM_ARB（0x8804）。
// 这里只区分可能合法参数与完全未知枚举，不增加版本/扩展承诺或实现白名单。
#pragma once
#include <cstdint>
namespace mg::enable_detail {
inline bool known_possible_scalar_cap(std::uint32_t cap, bool query) {
    switch(cap) {
    case 0xb10: // GL_POINT_SMOOTH
    case 0xb20: // GL_LINE_SMOOTH
    case 0xb24: // GL_LINE_STIPPLE
    case 0xb41: // GL_POLYGON_SMOOTH
    case 0xb42: // GL_POLYGON_STIPPLE
    case 0xb44: // GL_CULL_FACE
    case 0xb50: // GL_LIGHTING
    case 0xb57: // GL_COLOR_MATERIAL
    case 0xb60: // GL_FOG
    case 0xb71: // GL_DEPTH_TEST
    case 0xb90: // GL_STENCIL_TEST
    case 0xba1: // GL_NORMALIZE
    case 0xbc0: // GL_ALPHA_TEST
    case 0xbd0: // GL_DITHER
    case 0xbe2: // GL_BLEND
    case 0xbf1: // GL_INDEX_LOGIC_OP
    case 0xbf2: // GL_COLOR_LOGIC_OP
    case 0xc11: // GL_SCISSOR_TEST
    case 0xc60: // GL_TEXTURE_GEN_S
    case 0xc61: // GL_TEXTURE_GEN_T
    case 0xc62: // GL_TEXTURE_GEN_R
    case 0xc63: // GL_TEXTURE_GEN_Q
    case 0xd80: // GL_AUTO_NORMAL
    case 0xd90: // GL_MAP1_COLOR_4
    case 0xd91: // GL_MAP1_INDEX
    case 0xd92: // GL_MAP1_NORMAL
    case 0xd93: // GL_MAP1_TEXTURE_COORD_1
    case 0xd94: // GL_MAP1_TEXTURE_COORD_2
    case 0xd95: // GL_MAP1_TEXTURE_COORD_3
    case 0xd96: // GL_MAP1_TEXTURE_COORD_4
    case 0xd97: // GL_MAP1_VERTEX_3
    case 0xd98: // GL_MAP1_VERTEX_4
    case 0xdb0: // GL_MAP2_COLOR_4
    case 0xdb1: // GL_MAP2_INDEX
    case 0xdb2: // GL_MAP2_NORMAL
    case 0xdb3: // GL_MAP2_TEXTURE_COORD_1
    case 0xdb4: // GL_MAP2_TEXTURE_COORD_2
    case 0xdb5: // GL_MAP2_TEXTURE_COORD_3
    case 0xdb6: // GL_MAP2_TEXTURE_COORD_4
    case 0xdb7: // GL_MAP2_VERTEX_3
    case 0xdb8: // GL_MAP2_VERTEX_4
    case 0xde0: // GL_TEXTURE_1D
    case 0xde1: // GL_TEXTURE_2D
    case 0x2a01: // GL_POLYGON_OFFSET_POINT
    case 0x2a02: // GL_POLYGON_OFFSET_LINE
    case 0x3000: // GL_CLIP_DISTANCE0,GL_CLIP_PLANE0
    case 0x3001: // GL_CLIP_DISTANCE1,GL_CLIP_PLANE1
    case 0x3002: // GL_CLIP_DISTANCE2,GL_CLIP_PLANE2
    case 0x3003: // GL_CLIP_DISTANCE3,GL_CLIP_PLANE3
    case 0x3004: // GL_CLIP_DISTANCE4,GL_CLIP_PLANE4
    case 0x3005: // GL_CLIP_DISTANCE5,GL_CLIP_PLANE5
    case 0x3006: // GL_CLIP_DISTANCE6
    case 0x3007: // GL_CLIP_DISTANCE7
    case 0x4000: // GL_LIGHT0
    case 0x4001: // GL_LIGHT1
    case 0x4002: // GL_LIGHT2
    case 0x4003: // GL_LIGHT3
    case 0x4004: // GL_LIGHT4
    case 0x4005: // GL_LIGHT5
    case 0x4006: // GL_LIGHT6
    case 0x4007: // GL_LIGHT7
    case 0x8010: // GL_CONVOLUTION_1D,GL_CONVOLUTION_1D_EXT
    case 0x8011: // GL_CONVOLUTION_2D,GL_CONVOLUTION_2D_EXT
    case 0x8012: // GL_SEPARABLE_2D,GL_SEPARABLE_2D_EXT
    case 0x8024: // GL_HISTOGRAM,GL_HISTOGRAM_EXT
    case 0x802e: // GL_MINMAX,GL_MINMAX_EXT
    case 0x8037: // GL_POLYGON_OFFSET_FILL
    case 0x803a: // GL_RESCALE_NORMAL,GL_RESCALE_NORMAL_EXT
    case 0x806f: // GL_TEXTURE_3D,GL_TEXTURE_3D_EXT
    case 0x8094: // GL_INTERLACE_SGIX
    case 0x809d: // GL_MULTISAMPLE,GL_MULTISAMPLE_SGIS
    case 0x809e: // GL_SAMPLE_ALPHA_TO_COVERAGE,GL_SAMPLE_ALPHA_TO_MASK_SGIS
    case 0x809f: // GL_SAMPLE_ALPHA_TO_ONE,GL_SAMPLE_ALPHA_TO_ONE_SGIS
    case 0x80a0: // GL_SAMPLE_COVERAGE,GL_SAMPLE_MASK_SGIS
    case 0x80bc: // GL_TEXTURE_COLOR_TABLE_SGI
    case 0x80d0: // GL_COLOR_TABLE,GL_COLOR_TABLE_SGI
    case 0x80d1: // GL_POST_CONVOLUTION_COLOR_TABLE,GL_POST_CONVOLUTION_COLOR_TABLE_SGI
    case 0x80d2: // GL_POST_COLOR_MATRIX_COLOR_TABLE,GL_POST_COLOR_MATRIX_COLOR_TABLE_SGI
    case 0x8134: // GL_TEXTURE_4D_SGIS
    case 0x8139: // GL_PIXEL_TEX_GEN_SGIX
    case 0x8148: // GL_SPRITE_SGIX
    case 0x817d: // GL_REFERENCE_PLANE_SGIX
    case 0x817f: // GL_IR_INSTRUMENT1_SGIX
    case 0x8183: // GL_CALLIGRAPHIC_FRAGMENT_SGIX
    case 0x818b: // GL_FRAMEZOOM_SGIX
    case 0x8198: // GL_FOG_OFFSET_SGIX
    case 0x81fb: // GL_SHARED_TEXTURE_PALETTE_EXT
    case 0x8242: // GL_DEBUG_OUTPUT_SYNCHRONOUS
    case 0x832c: // GL_ASYNC_HISTOGRAM_SGIX
    case 0x8353: // GL_PIXEL_TEXTURE_SGIS
    case 0x835c: // GL_ASYNC_TEX_IMAGE_SGIX
    case 0x835d: // GL_ASYNC_DRAW_PIXELS_SGIX
    case 0x835e: // GL_ASYNC_READ_PIXELS_SGIX
    case 0x8400: // GL_FRAGMENT_LIGHTING_SGIX
    case 0x8401: // GL_FRAGMENT_COLOR_MATERIAL_SGIX
    case 0x840c: // GL_FRAGMENT_LIGHT0_SGIX
    case 0x840d: // GL_FRAGMENT_LIGHT1_SGIX
    case 0x840e: // GL_FRAGMENT_LIGHT2_SGIX
    case 0x840f: // GL_FRAGMENT_LIGHT3_SGIX
    case 0x8410: // GL_FRAGMENT_LIGHT4_SGIX
    case 0x8411: // GL_FRAGMENT_LIGHT5_SGIX
    case 0x8412: // GL_FRAGMENT_LIGHT6_SGIX
    case 0x8413: // GL_FRAGMENT_LIGHT7_SGIX
    case 0x8458: // GL_COLOR_SUM
    case 0x84f5: // GL_TEXTURE_RECTANGLE,GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_RECTANGLE_NV
    case 0x8513: // GL_TEXTURE_CUBE_MAP,GL_TEXTURE_CUBE_MAP_ARB,GL_TEXTURE_CUBE_MAP_EXT,GL_TEXTURE_CUBE_MAP_OES
    case 0x8642: // GL_PROGRAM_POINT_SIZE,GL_VERTEX_PROGRAM_POINT_SIZE
    case 0x8643: // GL_VERTEX_PROGRAM_TWO_SIDE
    case 0x864f: // GL_DEPTH_CLAMP
    case 0x8804: // GL_FRAGMENT_PROGRAM_ARB
    case 0x884f: // GL_TEXTURE_CUBE_MAP_SEAMLESS
    case 0x8861: // GL_POINT_SPRITE
    case 0x8c36: // GL_SAMPLE_SHADING
    case 0x8c89: // GL_RASTERIZER_DISCARD
    case 0x8d60: // GL_TEXTURE_GEN_STR_OES
    case 0x8d69: // GL_PRIMITIVE_RESTART_FIXED_INDEX
    case 0x8db9: // GL_FRAMEBUFFER_SRGB
    case 0x8e51: // GL_SAMPLE_MASK
    case 0x8f65: // GL_FETCH_PER_SAMPLE_ARM
    case 0x8f9d: // GL_PRIMITIVE_RESTART
    case 0x92e0: // GL_DEBUG_OUTPUT
    case 0x95b1: // GL_SHADING_RATE_IMAGE_PER_PRIMITIVE_NV
    case 0x96a2: // GL_FRAMEBUFFER_FETCH_NONCOHERENT_QCOM
    case 0x96a5: // GL_SHADING_RATE_PRESERVE_ASPECT_RATIO_QCOM
        return true;
    case 0x8074: // query only: GL_VERTEX_ARRAY
    case 0x8075: // query only: GL_NORMAL_ARRAY
    case 0x8076: // query only: GL_COLOR_ARRAY
    case 0x8077: // query only: GL_INDEX_ARRAY
    case 0x8078: // query only: GL_TEXTURE_COORD_ARRAY
    case 0x8079: // query only: GL_EDGE_FLAG_ARRAY
    case 0x8457: // query only: GL_FOG_COORD_ARRAY
    case 0x845e: // query only: GL_SECONDARY_COLOR_ARRAY
        return query;
    default: return false;
    }
}
}

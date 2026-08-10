// MobileGlues - explicit initialization API
// SPDX-License-Identifier: LGPL-2.1-only

#ifndef MOBILEGLUES_INIT_H
#define MOBILEGLUES_INIT_H

#include <stdint.h>

#if defined(_WIN32)
#define MG_INIT_API __declspec(dllexport)
#elif defined(__GNUC__) || defined(__clang__)
#define MG_INIT_API __attribute__((visibility("default")))
#else
#define MG_INIT_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MG_INIT_ABI_VERSION 1u

typedef enum mg_init_state_v1 {
    MG_INIT_STATE_COLD = 0,
    MG_INIT_STATE_INITIALIZING = 1,
    MG_INIT_STATE_READY = 2,
    MG_INIT_STATE_FAILED = 3
} mg_init_state_v1;

typedef enum mg_init_error_v1 {
    MG_INIT_ERROR_NONE = 0,
    MG_INIT_ERROR_REENTRANT = 1,
    MG_INIT_ERROR_CONFIG_PATH = 2,
    MG_INIT_ERROR_BACKEND_LOAD = 3,
    MG_INIT_ERROR_EGL_BOOTSTRAP = 4,
    MG_INIT_ERROR_GLES_BOOTSTRAP = 5,
    MG_INIT_ERROR_EXCEPTION = 6
} mg_init_error_v1;

typedef struct mg_init_report_v1 {
    uint32_t struct_size;
    uint32_t abi_version;
    int32_t state;
    int32_t error;
    char stage[64];
} mg_init_report_v1;

// Initializes one loaded MobileGlues image exactly once. Concurrent callers
// wait for the same result; failure is sticky for the lifetime of that image.
// Returns non-zero only when the image is READY.
MG_INIT_API int mg_initialize_v1(mg_init_report_v1* report);

// Reads the state without starting initialization. Returns the state value.
MG_INIT_API int mg_get_init_report_v1(mg_init_report_v1* report);

#ifdef __cplusplus
}
#endif

#endif // MOBILEGLUES_INIT_H

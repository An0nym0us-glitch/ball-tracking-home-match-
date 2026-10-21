/* Stable C API for dart:ffi (Flutter) and JNI. All strings are UTF-8 JSON. */
#ifndef POCKETEYE_C_H
#define POCKETEYE_C_H

#include <stdint.h>

#if defined(_WIN32)
#if defined(PE_BUILDING_C_API)
#define PE_API __declspec(dllexport)
#else
#define PE_API __declspec(dllimport)
#endif
#else
#define PE_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pe_engine pe_engine;

PE_API const char* pe_version(void);

/* Creates an engine from a session config (see config.hpp). Returns NULL on
 * error and writes the message to err (if err_len > 0). */
PE_API pe_engine* pe_create(const char* config_json, char* err, int32_t err_len);
PE_API void pe_destroy(pe_engine* e);

/* Calibration result of the engine's config as JSON. Returns the full length
 * needed (excluding the terminator); copies at most buf_len - 1 bytes. */
PE_API int32_t pe_calibration_json(pe_engine* e, char* buf, int32_t buf_len);

/* Feeds one 8-bit luma frame (e.g. the Y plane of NV12/NV21/I420) with its
 * capture time in seconds. Returns 0 on success. */
PE_API int32_t pe_push_luma(pe_engine* e, const uint8_t* data, int32_t width, int32_t height,
                            int32_t stride, double t_seconds);

/* End of stream: closes open tracks. */
PE_API void pe_finish(pe_engine* e);

/* Next delivery result as JSON. Returns 0 when there is none; otherwise the
 * full length needed. If buf_len is too small, nothing is consumed: call again
 * with a bigger buffer. */
PE_API int32_t pe_poll_result(pe_engine* e, char* buf, int32_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* POCKETEYE_C_H */

/* Stable C API for dart:ffi (Flutter) and JNI. All strings are UTF-8 JSON.
 *
 * Functions that return text take (buf, buf_len) and return the full length
 * needed (excluding the terminator); they copy at most buf_len - 1 bytes. */
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
typedef struct pe_video pe_video;

PE_API const char* pe_version(void);

/* Checks a session config (taps, pitch, ...) without creating an engine.
 * Returns the calibration JSON: ok, error, warnings, rms_px, camera, and
 * "overlay": projected stump lines and pitch outline (image pixels) to draw. */
PE_API int32_t pe_calibrate(const char* config_json, char* buf, int32_t buf_len);

/* Creates an engine from a session config. Returns NULL on error and writes
 * the message to err (if err_len > 0). */
PE_API pe_engine* pe_create(const char* config_json, char* err, int32_t err_len);
PE_API void pe_destroy(pe_engine* e);
PE_API int32_t pe_calibration_json(pe_engine* e, char* buf, int32_t buf_len);

/* Frames, in capture order, with their capture time in seconds. Return 0 on
 * success, -2 if the size does not match the calibration, other < 0 on error. */
PE_API int32_t pe_push_luma(pe_engine* e, const uint8_t* data, int32_t width, int32_t height,
                            int32_t stride, double t_seconds);
PE_API int32_t pe_push_bgr(pe_engine* e, const uint8_t* data, int32_t width, int32_t height,
                           int32_t stride, double t_seconds);
PE_API int32_t pe_push_jpeg(pe_engine* e, const uint8_t* data, int32_t len, double t_seconds);
/* Android YUV_420_888 (Flutter CameraImage): three planes; u/v may be
 * interleaved (uv_pixel_stride 2). rotation (0, 90, 180, 270, clockwise) turns
 * the sensor picture upright; width/height are before rotation, and the
 * calibration must be for the rotated picture. */
PE_API int32_t pe_push_yuv420(pe_engine* e, const uint8_t* y, const uint8_t* u, const uint8_t* v,
                              int32_t width, int32_t height, int32_t y_stride, int32_t uv_stride,
                              int32_t uv_pixel_stride, int32_t rotation, double t_seconds);

/* The last pushed frame as JPEG (scaled down to max_width; 0 = full size), for
 * replay buffers. Returns the size, or -needed if buf_len is too small. */
PE_API int32_t pe_last_frame_jpeg(pe_engine* e, int32_t max_width, int32_t quality, uint8_t* buf, int32_t buf_len);

/* Encodes an Android YUV_420_888 frame as JPEG (calibration snapshots). */
PE_API int32_t pe_yuv420_to_jpeg(const uint8_t* y, const uint8_t* u, const uint8_t* v, int32_t width,
                                 int32_t height, int32_t y_stride, int32_t uv_stride, int32_t uv_pixel_stride,
                                 int32_t rotation, int32_t quality, uint8_t* buf, int32_t buf_len);

/* What the detector sees, for a live debugging view: {"width", "height",
 * "corridor": [[u, v], ...] (outline of the watched area), "candidates":
 * [[t, u, v, radius, strength], ...] (ball candidates of the last second)}. */
PE_API int32_t pe_debug_json(pe_engine* e, char* buf, int32_t buf_len);

/* End of stream: closes open tracks. */
PE_API void pe_finish(pe_engine* e);

/* Next delivery result as JSON. Returns 0 when there is none; otherwise the
 * full length needed. If buf_len is too small, nothing is consumed: call again
 * with a bigger buffer. */
PE_API int32_t pe_poll_result(pe_engine* e, char* buf, int32_t buf_len);

/* Engine statistics (frames, per-step milliseconds) as JSON. */
PE_API int32_t pe_stats_json(pe_engine* e, char* buf, int32_t buf_len);

/* Synthetic speed test (see runBenchmark): JSON with engine_ms_per_frame, p90_ms
 * and a suggestion: "phone_only_60fps", "phone_only_30fps" or "laptop". */
PE_API int32_t pe_benchmark(int32_t width, int32_t height, int32_t frames, char* buf, int32_t buf_len);

/* Finds the stumps in a still picture (JPEG bytes). config_json is a session
 * config (pitch and stump sizes, camera hint, optional pitch length and
 * camera measurements; taps are ignored). Writes JSON: {"ok", "message",
 * "taps": {name: [u, v]}, "batter_found", "bowler_found", "length_estimated",
 * "pitch_length_m", "pitch_length_sigma_m", "calibration"}. Returns the JSON length. */
PE_API int32_t pe_auto_calibrate(const char* config_json, const uint8_t* jpeg, int32_t jpeg_len, char* buf,
                                 int32_t buf_len);

/* ---- Video files (desktop builds; return NULL / < 0 where unsupported) ---- */

/* Analyses a whole video. Writes the result JSON to out_json_path and, if
 * overlay_path is non-empty, a replay video. progress (may be NULL) receives
 * 0..1000 while running. Returns 0 on success. */
PE_API int32_t pe_analyze_video(const char* config_json, const char* video_path, const char* out_json_path,
                                const char* overlay_path, int32_t* progress, char* err, int32_t err_len);

PE_API pe_video* pe_video_open(const char* path);
/* {"width","height","fps","frames","duration_s"} */
PE_API int32_t pe_video_info(pe_video* v, char* buf, int32_t buf_len);
PE_API int32_t pe_video_seek(pe_video* v, double t_seconds);
/* Reads the next frame as JPEG (scaled to max_width if smaller than the
 * frame; 0 = full size). Returns the JPEG size, 0 at the end, or -needed if
 * buf_len is too small (the frame is kept: call again with a bigger buffer).
 * *t_out receives the frame time in seconds. */
PE_API int32_t pe_video_read_jpeg(pe_video* v, int32_t max_width, int32_t quality, uint8_t* buf,
                                  int32_t buf_len, double* t_out);
PE_API void pe_video_close(pe_video* v);

#ifdef __cplusplus
}
#endif

#endif /* POCKETEYE_C_H */

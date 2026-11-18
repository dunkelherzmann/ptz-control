/* Headless check of the core C API against tools/mock-camera.py.
 *   python3 tools/mock-camera.py --port 8080 &
 *   python3 tools/mock-camera.py --port 8081 --plain-png &
 *   scripts/test-core.sh
 */
#include "ptzcore.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
                              else { printf("ok:   "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void on_log(void *context, const char *line) { (void)context; printf("      log: %s\n", line); }

static int camera_port = 8080;

static ptz_session *open_camera(ptz_vendor vendor, const char *password, char *error) {
    ptz_connect_params p = {0};
    p.route = PTZ_ROUTE_LOCAL;
    p.vendor = vendor;
    p.camera_host = "127.0.0.1";
    p.camera_port = camera_port;
    p.camera_user = "admin";
    p.camera_password = password;
    p.https_port = 8443;
    p.timeout = 5;
    ptz_callbacks cb = {0};
    cb.log = on_log;
    return ptz_connect(&p, &cb, NULL, error, 512);
}

/* Live video: frames arrive on the stream's thread. */
typedef struct {
    volatile int frames, keyframes, first_key, first_has_parameters, codec, ended, bad_order;
    volatile double last_time;
} live_seen;

static void on_frame(void *context, const ptz_frame *frame) {
    live_seen *seen = context;
    if (seen->frames == 0) {
        seen->first_key = frame->keyframe;
        int type = frame->size > 4 ? frame->data[4] : 0;
        /* H.264 SPS is NAL type 7; H.265 VPS is type 32. */
        seen->first_has_parameters = frame->size > 4 && memcmp(frame->data, "\0\0\0\1", 4) == 0 &&
                                     (frame->codec == PTZ_CODEC_H264 ? (type & 0x1f) == 7 : ((type >> 1) & 0x3f) == 32);
    } else if (frame->time < seen->last_time) {
        seen->bad_order = 1;
    }
    seen->last_time = frame->time;
    seen->codec = frame->codec;
    seen->keyframes += frame->keyframe;
    seen->frames++;
}

static void on_ended(void *context, const char *reason) {
    ((live_seen *)context)->ended = 1;
    printf("      live ended: %s\n", reason);
}

static void exercise_live(ptz_session *s, ptz_codec codec, const char *name) {
    char error[512] = {0}, description[64] = {0};
    live_seen seen = {0};
    ptz_live_callbacks cb = {&seen, on_frame, on_ended};
    ptz_live *live = ptz_live_start(s, &cb, description, sizeof description, error, sizeof error);
    CHECK(live != NULL, "%s live video starts: %s (%s)", name, description, error);
    if (!live) return;
    sleep(4);  /* past the mock's first keep-alive, 3 s in */
    int frames = seen.frames;
    ptz_live_stop(live);
    int after_stop = seen.frames;
    usleep(300000);
    CHECK(frames >= 15 && seen.codec == (int)codec, "%s: %d frames in 4 s, codec %d", name, frames, seen.codec);
    CHECK(seen.first_key && seen.first_has_parameters, "%s: first frame is a key frame with parameter sets", name);
    CHECK(seen.keyframes >= 2 && !seen.bad_order, "%s: %d key frames, times in order", name, seen.keyframes);
    CHECK(!seen.ended && seen.frames == after_stop, "%s: nothing arrives after stopping", name);
}

static void exercise(ptz_vendor vendor, double pan, double tilt, double zoom, double bad_pan) {
    char error[512] = {0};
    ptz_session *s = open_camera(vendor, "secret", error);
    CHECK(s != NULL, "%s connect (%s)", vendor == PTZ_VENDOR_AXIS ? "axis" : "onvif", error);
    if (!s) return;
    ptz_info info;
    ptz_info_get(s, &info);
    CHECK(info.pan.available && info.tilt.available && info.zoom.available, "ranges pan %g..%g tilt %g..%g zoom %g..%g",
          info.pan.min, info.pan.max, info.tilt.min, info.tilt.max, info.zoom.min, info.zoom.max);

    ptz_position pos;
    CHECK(ptz_get_position(s, &pos, error, sizeof error), "position pan=%s tilt=%s zoom=%s status=%s",
          pos.pan.text, pos.tilt.text, pos.zoom.text, pos.move_status);

    ptz_target t = {1, 1, 1, pan, tilt, zoom};
    CHECK(ptz_move(s, &t, error, sizeof error), "absolute move (%s)", error);
    sleep(3);
    CHECK(ptz_get_position(s, &pos, error, sizeof error) && pos.pan.available &&
              (pos.pan.value - pan) < 1e-3 && (pan - pos.pan.value) < 1e-3, "arrived pan=%s tilt=%s zoom=%s %s",
          pos.pan.text, pos.tilt.text, pos.zoom.text, pos.move_status);

    ptz_target only_tilt = {0, 1, 0, 0, tilt / 2, 0};
    CHECK(ptz_move(s, &only_tilt, error, sizeof error), "tilt-only move keeps pan (%s)", error);

    ptz_target bad = {1, 0, 0, bad_pan, 0, 0};
    CHECK(!ptz_move(s, &bad, error, sizeof error), "out-of-range refused: %s", error);
    if (vendor == PTZ_VENDOR_AXIS) {
        ptz_target fractional = {0, 0, 1, 0, 0, 2.5};
        CHECK(!ptz_move(s, &fractional, error, sizeof error), "fractional Axis zoom refused: %s", error);
    }
    CHECK(ptz_stop(s, error, sizeof error), "stop (%s)", error);

    unsigned char *picture = NULL;
    size_t picture_bytes = 0;
    int width = 0, height = 0;
    CHECK(ptz_picture(s, &picture, &picture_bytes, &width, &height, error, sizeof error) && picture_bytes > 100 &&
              width > 0 && height > 0, "picture %dx%d, %zu bytes (%s)", width, height, picture_bytes, error);
    ptz_free(picture);

    exercise_live(s, PTZ_CODEC_H264, vendor == PTZ_VENDOR_AXIS ? "axis" : "onvif");

    char url[512], notice[256];
    CHECK(ptz_web_url(s, url, sizeof url, notice, sizeof notice, error, sizeof error), "web url %s %s", url, error);
    ptz_close(s);

    s = open_camera(vendor, "wrong-password", error);
    CHECK(s == NULL && strstr(error, "wrong-password") == NULL, "wrong password refused, redacted: %s", error);
    if (s) ptz_close(s);
}

static unsigned big_endian(const unsigned char *p) { return (unsigned)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* The mock's --plain-png pictures (RGB, stored deflate blocks, no row filters) as grey pixels: enough to test
 * measuring without an image library. */
static int grey_plain_png(void *context, const unsigned char *data, size_t size, unsigned char *grey, int width,
                          int height) {
    (void)context;
    if (size < 33) return 0;
    unsigned pw = big_endian(data + 16), ph = big_endian(data + 20);
    size_t row = 1 + 3 * (size_t)pw, need = row * ph, have = 0;
    unsigned char *zlib = malloc(size), *raw = malloc(need);
    size_t zlen = 0;
    for (size_t at = 8; at + 12 <= size;) {
        unsigned length = big_endian(data + at);
        if (!memcmp(data + at + 4, "IDAT", 4)) { memcpy(zlib + zlen, data + at + 8, length); zlen += length; }
        at += 12 + length;
    }
    for (size_t at = 2; at + 5 <= zlen && have < need;) {  /* after the 2-byte zlib header: stored blocks */
        int last = zlib[at] & 1;
        unsigned len = zlib[at + 1] | zlib[at + 2] << 8;
        if (have + len > need) len = (unsigned)(need - have);
        memcpy(raw + have, zlib + at + 5, len);
        have += len;
        at += 5 + len;
        if (last) break;
    }
    int ok = have == need;
    for (int y = 0; ok && y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const unsigned char *px = raw + (size_t)(y * ph / height) * row + 1 + 3 * (size_t)(x * pw / width);
            grey[y * width + x] = (unsigned char)((299 * px[0] + 587 * px[1] + 114 * px[2]) / 1000);
        }
    free(zlib);
    free(raw);
    return ok;
}

/* Measures the view of a mock camera that sends plain pictures, checks the result is kept for that zoom only, saved
 * and loaded, and that the camera is turned back. */
static void measure(ptz_vendor vendor, double pan, double tilt, double want_x, double want_y) {
    const char *name = vendor == PTZ_VENDOR_AXIS ? "axis" : "onvif";
    char error[512] = {0};
    camera_port = 8081;
    ptz_session *s = open_camera(vendor, "secret", error);
    camera_port = 8080;
    CHECK(s != NULL, "%s plain-picture camera connects (%s)", name, error);
    if (!s) return;
    ptz_info info;
    ptz_info_get(s, &info);
    CHECK(info.degrees == (vendor == PTZ_VENDOR_AXIS), "%s: pan and tilt %s", name, info.degrees ? "in degrees" : "in its own units");
    ptz_target t = {1, 1, 0, pan, tilt, 0};
    ptz_move(s, &t, error, sizeof error);
    sleep(3);
    ptz_position start, end;
    ptz_get_position(s, &start, error, sizeof error);
    ptz_view_scale scale, kept;
    CHECK(!ptz_view_scale_at(s, &start, &kept), "%s: no view before measuring", name);
    int ok = ptz_measure_view(s, grey_plain_png, NULL, &scale, error, sizeof error);
    CHECK(ok && fabs(scale.half_x / want_x - 1) < 0.015 && fabs(scale.half_y / want_y - 1) < 0.02,
          "%s: measured the view %.4f x %.4f (want %.4f x %.4f) (%s)", name, scale.half_x, scale.half_y, want_x, want_y,
          error);
    CHECK(ptz_view_scale_at(s, &start, &kept) && kept.half_x == scale.half_x, "%s: the view is kept for this zoom", name);
    sleep(3);
    CHECK(ptz_get_position(s, &end, error, sizeof error) && fabs(end.pan.value - start.pan.value) < 0.01 &&
              fabs(end.tilt.value - start.tilt.value) < 0.01,
          "%s: turned back to where it was: pan %s, tilt %s", name, end.pan.text, end.tilt.text);
    ptz_position other = end;
    other.zoom.value += info.zoom.max - info.zoom.min;
    CHECK(!ptz_view_scale_at(s, &other, &kept), "%s: no view at another zoom", name);
    char saved[256];
    ptz_view_scales_save(s, saved, sizeof saved);
    ptz_close(s);
    camera_port = 8081;
    s = open_camera(vendor, "secret", error);
    camera_port = 8080;
    if (!s) return;
    ptz_view_scales_load(s, saved);
    ptz_view_scales_load(s, "garbage;1:x");
    CHECK(ptz_view_scale_at(s, &end, &kept) && fabs(kept.half_x - scale.half_x) < 1e-6, "%s: saved and loaded: %s",
          name, saved);

    ptz_close(s);
}

/* Turning points of the picture into pan and tilt and back, and the plane's lines. */
static void projection(void) {
    ptz_range pan = {1, -180, 180}, tilt = {1, -90, 0};
    ptz_view v = {{0.57735, 0.32476}, 1, -156.55, -18.5, pan, tilt};
    double p, t, x, y;
    ptz_view_point(&v, 0, 0, &p, &t);
    CHECK(fabs(p - v.pan) < 1e-9 && fabs(t - v.tilt) < 1e-9, "the centre is where the camera points");
    ptz_view_point(&v, 1, 0, &p, &t);
    CHECK(p > v.pan + 30 && p < v.pan + 33 && t > v.tilt, "the right edge is a little over 30 degrees right, nearer the horizon (%.2f, %.2f)", p, t);
    CHECK(ptz_view_locate(&v, p, t, &x, &y) && fabs(x - 1) < 1e-9 && fabs(y) < 1e-9, "and back (%.9f, %.9f)", x, y);
    ptz_view_point(&v, -0.7, 0.9, &p, &t);
    CHECK(ptz_view_locate(&v, p, t, &x, &y) && fabs(x + 0.7) < 1e-9 && fabs(y - 0.9) < 1e-9, "round trip (%.9f, %.9f)", x, y);
    CHECK(!ptz_view_locate(&v, v.pan + 180, 0, &x, &y), "behind the camera isn't in the picture");
    ptz_view edge = v;
    edge.pan = 175;
    ptz_view_point(&edge, 0.5, 0, &p, &t);
    CHECK(p < -160 && p >= -180, "a point past pan 180 comes back within the range (%.2f)", p);

    ptz_plane_line lines[160];
    int n = ptz_view_lines(&v, lines, 160);
    int pan_lines = 0, tilt_lines = 0, limits = 0, labelled = 0;
    for (int i = 0; i < n; ++i) {
        if (lines[i].kind == PTZ_LINE_PAN || lines[i].kind == PTZ_LINE_PAN_ZERO) pan_lines++;
        if (lines[i].kind == PTZ_LINE_TILT || lines[i].kind == PTZ_LINE_TILT_ZERO) tilt_lines++;
        if (lines[i].kind == PTZ_LINE_TILT_LIMIT) limits++;
        labelled += lines[i].labelled;
    }
    CHECK(pan_lines >= 4 && tilt_lines >= 2 && limits >= 1 && labelled >= 5,
          "plane lines: %d pan, %d tilt, %d tilt limits, %d labelled", pan_lines, tilt_lines, limits, labelled);
    ptz_view onvif = {{0.25, 0.140625}, 0, 0.5, 0.2, {1, -1, 1}, {1, -1, 1}};
    ptz_view_point(&onvif, 1, -1, &p, &t);
    CHECK(fabs(p - 0.75) < 1e-9 && fabs(t - 0.059375) < 1e-9, "linear view: right bottom corner at (%.4f, %.4f)", p, t);
    n = ptz_view_lines(&onvif, lines, 160);
    CHECK(n >= 4 && n < 40, "linear view: %d lines", n);
}

int main(void) {
    double v;
    CHECK(ptz_parse_number("0.55", &v) && v == 0.55, "parse 0.55");
    CHECK(!ptz_parse_number("1,5", &v), "comma decimal refused");
    CHECK(!ptz_parse_number("nan", &v) && !ptz_parse_number("inf", &v), "non-finite refused");

    char error[512];
    ptz_connect_params p = {0};
    p.camera_host = "http://bad/url";
    p.camera_port = 80;
    p.camera_user = "admin";
    p.https_port = 443;
    CHECK(ptz_connect(&p, NULL, NULL, error, sizeof error) == NULL, "URL as host refused: %s", error);

    exercise(PTZ_VENDOR_ONVIF, 0.55, 0.22, 0.4, 1.5);
    exercise(PTZ_VENDOR_AXIS, 45.5, -30, 2000, 270);
    projection();
    measure(PTZ_VENDOR_ONVIF, 0.3, 0.1, 0.25, 0.140625);
    measure(PTZ_VENDOR_AXIS, -156.55, -18.5, 0.57735, 0.32476);
    measure(PTZ_VENDOR_AXIS, 0, 0, 0.57735, 0.32476);

    /* A second mock camera on port 8090 sends H.265. */
    camera_port = 8090;
    ptz_session *s = open_camera(PTZ_VENDOR_ONVIF, "secret", error);
    CHECK(s != NULL, "h265 camera connects (%s)", error);
    if (s) {
        exercise_live(s, PTZ_CODEC_H265, "onvif h265");
        ptz_close(s);
    }
    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures != 0;
}

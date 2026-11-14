/* Headless check of the core C API against tools/mock-camera.py.
 *   python3 tools/mock-camera.py --port 8080 &
 *   scripts/test-core.sh
 */
#include "ptzcore.h"

#include <stdio.h>
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

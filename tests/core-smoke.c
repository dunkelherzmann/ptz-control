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

static ptz_session *open_camera(ptz_vendor vendor, const char *password, char *error) {
    ptz_connect_params p = {0};
    p.route = PTZ_ROUTE_LOCAL;
    p.vendor = vendor;
    p.camera_host = "127.0.0.1";
    p.camera_port = 8080;
    p.camera_user = "admin";
    p.camera_password = password;
    p.https_port = 8443;
    p.timeout = 5;
    ptz_callbacks cb = {0};
    cb.log = on_log;
    return ptz_connect(&p, &cb, NULL, error, 512);
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
    CHECK(info.view_moves, "camera aims within the view");
    ptz_position before, after;
    ptz_get_position(s, &before, error, sizeof error);
    CHECK(ptz_move_in_view(s, 0.5, 0.5, error, sizeof error), "aim at the picture's upper right (%s)", error);
    sleep(3);
    CHECK(ptz_get_position(s, &after, error, sizeof error) && after.pan.value > before.pan.value &&
              after.tilt.value > before.tilt.value, "moved right and up: pan %s -> %s, tilt %s -> %s",
          before.pan.text, after.pan.text, before.tilt.text, after.tilt.text);
    CHECK(!ptz_move_in_view(s, 1.5, 0, error, sizeof error), "point outside the picture refused: %s", error);

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
    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures != 0;
}

/* Headless check of the core C API against tools/mock-camera.py.
 *   python3 tools/mock-camera.py --port 8080 &
 *   python3 tools/mock-camera.py --port 8081 --no-fov --plain-png &
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

/* An ONVIF camera without the field-of-view space: the view is measured from its pictures. The mock's picture spans
 * 0.25 pan and 0.140625 tilt from its centre to its edges at zoom 0. */
static void measure(void) {
    char error[512] = {0};
    camera_port = 8081;
    ptz_session *s = open_camera(PTZ_VENDOR_ONVIF, "secret", error);
    camera_port = 8080;
    CHECK(s != NULL, "onvif camera without field-of-view moves: connect (%s)", error);
    if (!s) return;
    ptz_info info;
    ptz_info_get(s, &info);
    CHECK(!info.view_moves, "it can't aim within its view by itself");
    ptz_position start, end;
    ptz_get_position(s, &start, error, sizeof error);
    ptz_view_scale scale, kept;
    CHECK(!ptz_view_scale_at(s, &start, &kept), "no scale before measuring");
    int ok = ptz_measure_view(s, grey_plain_png, NULL, &scale, error, sizeof error);
    CHECK(ok && fabs(scale.pan_per_x - 0.25) < 0.03 && fabs(scale.tilt_per_y - 0.140625) < 0.02,
          "measured the view: %.4f pan, %.4f tilt per half picture (%s)", scale.pan_per_x, scale.tilt_per_y, error);
    CHECK(ptz_view_scale_at(s, &start, &kept) && kept.pan_per_x == scale.pan_per_x, "the scale is kept for this zoom");
    sleep(3);
    CHECK(ptz_get_position(s, &end, error, sizeof error) && fabs(end.pan.value - start.pan.value) < 0.01 &&
              fabs(end.tilt.value - start.tilt.value) < 0.01,
          "turned back to where it was: pan %s, tilt %s", end.pan.text, end.tilt.text);
    end.zoom.value += 0.5;
    CHECK(!ptz_view_scale_at(s, &end, &kept), "no scale at another zoom");
    ptz_close(s);
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
    measure();
    printf(failures ? "\n%d FAILED\n" : "\nall passed\n", failures);
    return failures != 0;
}

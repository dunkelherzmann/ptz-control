// The camera's view: measuring how wide its picture is (turn a little, compare pictures), and turning points of the
// picture into pan and tilt and back, with the plane's lines drawn over it.
#include "ptzcore.h"
#include "internal.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

namespace ptz {

namespace {

struct Grey {
    int width = 0, height = 0;
    std::vector<float> pixels;  // zero mean, unit spread
    float at(int x, int y) const { return pixels[static_cast<size_t>(y) * width + x]; }
};

Grey normalised(const unsigned char *data, int width, int height) {
    Grey g{width, height, std::vector<float>(data, data + static_cast<size_t>(width) * height)};
    double mean = 0, spread = 0;
    for (float p : g.pixels) mean += p;
    mean /= static_cast<double>(g.pixels.size());
    for (float &p : g.pixels) spread += (p - mean) * (p - mean);
    spread = std::sqrt(spread / static_cast<double>(g.pixels.size()));
    if (spread < 1) spread = 1;  // a flat picture: nothing to match, the score says so
    for (float &p : g.pixels) p = static_cast<float>((p - mean) / spread);
    return g;
}

Grey half(const Grey &g) {
    Grey h{g.width / 2, g.height / 2, {}};
    h.pixels.resize(static_cast<size_t>(h.width) * h.height);
    for (int y = 0; y < h.height; ++y)
        for (int x = 0; x < h.width; ++x)
            h.pixels[static_cast<size_t>(y) * h.width + x] =
                (g.at(2 * x, 2 * y) + g.at(2 * x + 1, 2 * y) + g.at(2 * x, 2 * y + 1) + g.at(2 * x + 1, 2 * y + 1)) / 4;
    return h;
}

// Mean difference between b(x, y) and a(x + sx, y + sy) where both exist; infinity when they barely overlap.
double score(const Grey &a, const Grey &b, int sx, int sy) {
    int x0 = std::max(0, -sx), x1 = std::min(b.width, a.width - sx);
    int y0 = std::max(0, -sy), y1 = std::min(b.height, a.height - sy);
    if ((x1 - x0) * 3 < b.width || (y1 - y0) * 3 < b.height) return INFINITY;
    double sum = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) sum += std::fabs(b.at(x, y) - a.at(x + sx, y + sy));
    return sum / ((x1 - x0) * (y1 - y0));
}

constexpr double kRadian = 3.14159265358979323846 / 180;

struct Vec {
    double x, y, z;
    double dot(const Vec &o) const { return x * o.x + y * o.y + z * o.z; }
};
// A direction from the camera: x east (pan's positive turn), y north (pan 0), z up.
Vec direction(double pan, double tilt) {
    double p = pan * kRadian, t = tilt * kRadian;
    return {std::cos(t) * std::sin(p), std::cos(t) * std::cos(p), std::sin(t)};
}
// The camera's forward, right and up directions when it points at (pan, tilt).
struct Basis {
    Vec f, r, u;
};
Basis basis(double pan, double tilt) {
    double p = pan * kRadian, t = tilt * kRadian;
    return {direction(pan, tilt), {std::cos(p), -std::sin(p), 0},
            {-std::sin(t) * std::sin(p), -std::sin(t) * std::cos(p), std::cos(t)}};
}
// Where (pan, tilt) shows from a camera pointing at (at_pan, at_tilt), in tangents of the angle off its axis.
std::optional<std::pair<double, double>> tangents(double at_pan, double at_tilt, double pan, double tilt) {
    Basis b = basis(at_pan, at_tilt);
    Vec d = direction(pan, tilt);
    double forward = d.dot(b.f);
    if (forward <= 1e-6) return std::nullopt;
    return std::make_pair(d.dot(b.r) / forward, d.dot(b.u) / forward);
}

}  // namespace

// How well `after` matches `before` turned by `turned` on `axis`, for a picture of half_x by half_y tangents: each
// point of `after` is traced through the lens to the point of `before` showing the same direction, and the mean
// difference of the normalised pictures is the score (infinity when they barely overlap).
double warp_score(const Grey &before, const Grey &after, int axis, double pan, double tilt, double turned, double half_x,
                  double half_y) {
    ptz_view was{{half_x, half_y}, 1, pan, tilt, {}, {}};
    ptz_view now = was;
    (axis == 0 ? now.pan : now.tilt) += turned;
    const int w = after.width, h = after.height;
    double sum = 0;
    int used = 0, tried = 0;
    for (int py = 1; py < h - 1; py += 2)
        for (int px = 1; px < w - 1; px += 2) {
            ++tried;
            double x = 2 * (px + 0.5) / w - 1, y = 1 - 2 * (py + 0.5) / h, p, t, x0, y0;
            ptz_view_point(&now, x, y, &p, &t);
            if (!ptz_view_locate(&was, p, t, &x0, &y0)) continue;
            double fx = (x0 + 1) * w / 2 - 0.5, fy = (1 - y0) * h / 2 - 0.5;
            int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
            if (ix < 0 || iy < 0 || ix + 1 >= w || iy + 1 >= h) continue;
            double ax = fx - ix, ay = fy - iy;
            double v = before.at(ix, iy) * (1 - ax) * (1 - ay) + before.at(ix + 1, iy) * ax * (1 - ay) +
                       before.at(ix, iy + 1) * (1 - ax) * ay + before.at(ix + 1, iy + 1) * ax * ay;
            sum += std::fabs(after.at(px, py) - v);
            ++used;
        }
    return used * 3 < tried ? INFINITY : sum / used;
}

// The half width (axis 0) or height (axis 1) that best warps `before` onto `after`, searched within a third either way
// of `guess`.
double fit_half(const Grey &before, const Grey &after, int axis, double pan, double tilt, double turned, double other,
                double guess) {
    auto score = [&](double log_half) {
        double h = std::copysign(std::exp(log_half), guess);
        return warp_score(before, after, axis, pan, tilt, turned, axis == 0 ? h : other, axis == 0 ? other : h);
    };
    // A coarse scan first (the score has small local dips), then golden-section search around the best.
    double centre = std::log(std::fabs(guess)), best = centre, best_score = INFINITY;
    for (int i = -12; i <= 12; ++i) {
        double k = centre + i * 0.025, sc = score(k);
        if (sc < best_score) best_score = sc, best = k;
    }
    double lo = best - 0.03, hi = best + 0.03;
    const double g = (std::sqrt(5.0) - 1) / 2;
    double a = hi - g * (hi - lo), b = lo + g * (hi - lo), sa = score(a), sb = score(b);
    for (int i = 0; i < 25; ++i) {
        if (sa < sb) hi = b, b = a, sb = sa, a = hi - g * (hi - lo), sa = score(a);
        else lo = a, a = b, sa = sb, b = lo + g * (hi - lo), sb = score(b);
    }
    return std::copysign(std::exp((lo + hi) / 2), guess);
}

double view_half(bool degrees, int axis, double pan, double tilt, double turned, double shift) {
    if (!degrees) return -turned / shift;
    // The old centre, seen after the turn, lies `shift` view units from the new one.
    auto t = axis == 0 ? tangents(pan + turned, tilt, pan, tilt) : tangents(pan, tilt + turned, pan, tilt);
    if (!t) return 0;
    return (axis == 0 ? t->first : t->second) / shift;
}

std::optional<std::pair<double, double>> picture_shift(const unsigned char *before, const unsigned char *after, int width,
                                                      int height, double expect_x, double expect_y, double reach) {
    Grey a = normalised(before, width, height), b = normalised(after, width, height);
    Grey ha = half(a), hb = half(b);
    // b shows at x what a showed at x + sx: a scene that moved by (dx, dy) view units (x right, y up) means
    // sx = -dx * width / 2 and sy = dy * height / 2. Search the shifts within `reach` of the expected one at half
    // size, then refine the best at full size. Looking only near the expected shift keeps a repeating scene (a
    // fence, a row of windows) from matching a whole period away.
    auto px = [&](double v) { return static_cast<int>(std::lround(v * ha.width / 2)); };
    auto py = [&](double v) { return static_cast<int>(std::lround(v * ha.height / 2)); };
    int cx = px(-expect_x), cy = py(expect_y), rx = std::max(2, px(reach)), ry = std::max(2, py(reach));
    int best_x = 0, best_y = 0;
    double best = INFINITY;
    for (int sy = cy - ry; sy <= cy + ry; ++sy)
        for (int sx = cx - rx; sx <= cx + rx; ++sx) {
            double s = score(ha, hb, sx, sy);
            if (s < best) best = s, best_x = sx, best_y = sy;
        }
    // Unrelated pictures differ by about 1.1 (both normalised); a match of the same scene comes well under half that.
    if (!std::isfinite(best) || best > 0.5) return std::nullopt;
    double fine = INFINITY;
    int fx = 0, fy = 0;
    for (int sy = best_y * 2 - 2; sy <= best_y * 2 + 2; ++sy)
        for (int sx = best_x * 2 - 2; sx <= best_x * 2 + 2; ++sx) {
            double s = score(a, b, sx, sy);
            if (s < fine) fine = s, fx = sx, fy = sy;
        }
    return std::make_pair(-2.0 * fx / width, 2.0 * fy / height);
}

ViewScale measure_view(Camera &camera, const Hooks &hooks, const GreyDecoder &grey) {
    if (!camera.pan_range.available || !camera.tilt_range.available)
        throw Error("Measuring the view needs the camera's pan and tilt ranges");
    Position start = camera.position();
    if (!start.pan.available || !start.tilt.available)
        throw Error("Measuring the view needs pan and tilt readings from the camera");

    auto picture = [&] {
        std::string bytes = camera.picture();
        auto size = picture_size(bytes);
        if (!size || size->first <= 0 || size->second <= 0) throw Error("The camera's picture could not be read");
        int width = 320, height = std::max(8, static_cast<int>(std::lround(320.0 * size->second / size->first)));
        std::vector<unsigned char> pixels(static_cast<size_t>(width) * height);
        if (!grey(bytes, pixels.data(), width, height)) throw Error("The camera's picture could not be read");
        return std::make_tuple(pixels, width, height);
    };
    // Waits until the camera has stopped near `target` (or stays put), then a little longer, so the next picture
    // shows where it is.
    auto settle = [&](double pan, double tilt) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        double pan_slack = (camera.pan_range.max - camera.pan_range.min) * 0.002;
        double tilt_slack = (camera.tilt_range.max - camera.tilt_range.min) * 0.002;
        Position last;
        bool have_last = false;
        Position now;
        while (std::chrono::steady_clock::now() < deadline) {
            if (hooks.cancelled()) throw Error("Cancelled");
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            now = camera.position();
            if (!now.pan.available || !now.tilt.available) continue;
            bool arrived = std::fabs(now.pan.value - pan) <= pan_slack && std::fabs(now.tilt.value - tilt) <= tilt_slack;
            bool still = have_last && std::fabs(now.pan.value - last.pan.value) <= pan_slack / 4 &&
                         std::fabs(now.tilt.value - last.tilt.value) <= tilt_slack / 4;
            bool moving = lower(now.move_status).find("moving") != std::string::npos;
            if (!moving && (arrived || still)) break;
            last = now;
            have_last = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        return camera.position();
    };
    auto go = [&](double pan, double tilt) {
        Target t;
        t.pan = pan;
        t.tilt = tilt;
        camera.move(t);
        return settle(pan, tilt);
    };

    ViewScale scale;
    double turns[2] = {0, 0};             // each axis's last turn
    std::vector<unsigned char> turned_pictures[2];  // and the picture after it
    std::vector<unsigned char> start_picture, last_after;
    int picture_width = 0, picture_height = 0;
    try {
        auto [before, width, height] = picture();
        start_picture = before, picture_width = width, picture_height = height;
        for (int axis = 0; axis < 2; ++axis) {
            const Range &range = axis == 0 ? camera.pan_range : camera.tilt_range;
            double origin = axis == 0 ? start.pan.value : start.tilt.value, span = range.max - range.min;
            // Turns by `delta` and measures how far the scene moved, looking within `reach` of `expect`.
            auto trial = [&](double delta, double expect, double reach) -> std::optional<std::pair<double, double>> {
                double to = origin + delta <= range.max ? origin + delta : origin - delta;
                Position at = axis == 0 ? go(to, start.tilt.value) : go(start.pan.value, to);
                auto [after, w, h] = picture();
                last_after = after;
                double turned = (axis == 0 ? at.pan.value : at.tilt.value) - origin;
                if (w != width || h != height || turned == 0) return std::nullopt;
                if (to < origin) expect = -expect;
                auto shift = picture_shift(before.data(), after.data(), width, height, axis == 0 ? expect : 0,
                                           axis == 0 ? 0 : expect, reach);
                if (!shift) return std::nullopt;
                return std::make_pair(turned, axis == 0 ? shift->first : shift->second);
            };
            // A small turn first, looking only for a small shift: a rough scale. Then a turn that should move the
            // scene about 0.4 of the half picture, looking near where the rough scale says it lands.
            std::optional<double> rough;
            std::optional<std::pair<double, double>> found;  // the turn and the shift it made
            for (double delta = span * 0.01; !rough && delta >= span * 0.0002 && delta <= span * 0.2;) {
                auto t = trial(delta, 0, 0.3);
                if (!t) delta /= 4;                                    // no match close by: the view is narrow
                else if (std::fabs(t->second) < 0.03) delta *= 4;      // too little to measure well
                else rough = t->first / -t->second;
            }
            if (rough) {
                double delta = std::min(std::fabs(*rough) * 0.4, span * 0.25);
                if (auto t = trial(delta, -delta / *rough, 0.1 + delta / std::fabs(*rough) * 0.25); t && t->second != 0)
                    found = t;
                else
                    found = std::make_pair(*rough, -1.0);
            }
            if (!found)
                throw Error(std::string("Couldn't measure the view: the pictures showed no clear ") +
                            (axis == 0 ? "sideways" : "up-and-down") + " shift (a plain or changing scene?)");
            double half = view_half(camera.degrees(), axis, start.pan.value, start.tilt.value, found->first, found->second);
            if (!std::isfinite(half) || half == 0) throw Error("Couldn't measure the view: the turn showed no usable shift");
            (axis == 0 ? scale.half_x : scale.half_y) = half;
            turns[axis] = found->second == -1 ? 0 : found->first;
            turned_pictures[axis] = last_after;
            go(start.pan.value, start.tilt.value);  // back where it was, for the next axis's pictures
        }
    } catch (...) {
        try {
            Target back;
            back.pan = start.pan.value;
            back.tilt = start.tilt.value;
            camera.move(back);
        } catch (...) {
        }
        throw;
    }
    if (camera.degrees() && turns[0] != 0 && turns[1] != 0) {
        // Through a lens a turn doesn't slide the picture evenly (points away from the centre move further), so the
        // shift found is only close: fit each width by tracing one picture onto the other through the lens.
        Grey first = normalised(start_picture.data(), picture_width, picture_height);
        Grey after_pan = normalised(turned_pictures[0].data(), picture_width, picture_height);
        Grey after_tilt = normalised(turned_pictures[1].data(), picture_width, picture_height);
        for (int round = 0; round < 2; ++round) {
            scale.half_x = fit_half(first, after_pan, 0, start.pan.value, start.tilt.value, turns[0], scale.half_y,
                                    scale.half_x);
            scale.half_y = fit_half(first, after_tilt, 1, start.pan.value, start.tilt.value, turns[1], scale.half_x,
                                    scale.half_y);
        }
    }
    scale.zoom_available = start.zoom.available;
    scale.zoom = start.zoom.value;
    return scale;
}

}  // namespace ptz

// ---- the plane over the picture ----------------------------------------------

namespace {

using ptz::Basis;
using ptz::basis;
using ptz::kRadian;
using ptz::tangents;
using ptz::Vec;

bool usable(const ptz_view *v) {
    return std::isfinite(v->scale.half_x) && std::isfinite(v->scale.half_y) && v->scale.half_x != 0 &&
           v->scale.half_y != 0;
}

// A pan within the pan range when a whole turn brings it there.
double into_range(const ptz_view *v, double pan) {
    if (!v->degrees || !v->pan_range.available) return pan;
    for (double p : {pan, pan - 360, pan + 360})
        if (p >= v->pan_range.min - 1e-9 && p <= v->pan_range.max + 1e-9) return p;
    return pan;
}

bool pan_in_range(const ptz_view *v, double pan) {
    if (!v->pan_range.available) return true;
    double p = into_range(v, pan);
    return p >= v->pan_range.min - 1e-9 && p <= v->pan_range.max + 1e-9;
}

bool tilt_in_range(const ptz_view *v, double tilt) {
    if (v->degrees && std::fabs(tilt) > 90 + 1e-9) return false;
    return !v->tilt_range.available || (tilt >= v->tilt_range.min - 1e-9 && tilt <= v->tilt_range.max + 1e-9);
}

// The smallest of 1, 2 or 5 times a power of ten at least `v`.
double round_step(double v) {
    if (!(v > 0) || !std::isfinite(v)) return 1;
    double base = std::pow(10.0, std::floor(std::log10(v)));
    for (double m : {1.0, 2.0, 5.0, 10.0})
        if (m * base >= v * (1 - 1e-9)) return m * base;
    return 10 * base;
}

}  // namespace

extern "C" void ptz_view_point(const ptz_view *v, double x, double y, double *pan, double *tilt) {
    if (!v->degrees || !usable(v)) {
        *pan = v->pan + x * v->scale.half_x;
        *tilt = v->tilt + y * v->scale.half_y;
        return;
    }
    Basis b = basis(v->pan, v->tilt);
    double tx = x * v->scale.half_x, ty = y * v->scale.half_y;
    Vec ray{b.f.x + tx * b.r.x + ty * b.u.x, b.f.y + tx * b.r.y + ty * b.u.y, b.f.z + tx * b.r.z + ty * b.u.z};
    double p = std::atan2(ray.x, ray.y) / kRadian;
    *tilt = std::atan2(ray.z, std::hypot(ray.x, ray.y)) / kRadian;
    *pan = into_range(v, v->pan + std::remainder(p - v->pan, 360.0));
}

extern "C" int ptz_view_locate(const ptz_view *v, double pan, double tilt, double *x, double *y) {
    if (!usable(v)) return 0;
    if (!v->degrees) {
        *x = (pan - v->pan) / v->scale.half_x;
        *y = (tilt - v->tilt) / v->scale.half_y;
        return 1;
    }
    auto t = tangents(v->pan, v->tilt, pan, tilt);
    if (!t) return 0;
    *x = t->first / v->scale.half_x;
    *y = t->second / v->scale.half_y;
    return 1;
}

extern "C" int ptz_view_lines(const ptz_view *v, ptz_plane_line *lines, int max) {
    if (!usable(v) || max <= 0) return 0;
    // What the picture spans (a little beyond its edges): pan as turns from the centre's, tilt as it is.
    double pan_lo = INFINITY, pan_hi = -INFINITY, tilt_lo = INFINITY, tilt_hi = -INFINITY;
    for (int i = 0; i <= 8; ++i)
        for (int j = 0; j <= 8; ++j) {
            double pan, tilt;
            ptz_view_point(v, -1.2 + 2.4 * i / 8, -1.2 + 2.4 * j / 8, &pan, &tilt);
            double d = v->degrees ? std::remainder(pan - v->pan, 360.0) : pan - v->pan;
            pan_lo = std::min(pan_lo, d), pan_hi = std::max(pan_hi, d);
            tilt_lo = std::min(tilt_lo, tilt), tilt_hi = std::max(tilt_hi, tilt);
        }
    if (v->degrees) {
        double x, y;
        for (double pole : {90.0, -90.0})
            if (ptz_view_locate(v, v->pan, pole, &x, &y) && std::fabs(x) <= 1.3 && std::fabs(y) <= 1.3) {
                pan_lo = -180, pan_hi = 180;
                (pole > 0 ? tilt_hi : tilt_lo) = pole;
            }
        tilt_lo = std::max(tilt_lo, -90.0), tilt_hi = std::min(tilt_hi, 90.0);
    }
    pan_lo += v->pan, pan_hi += v->pan;

    // One step for both axes in degrees (square cells at the centre); per axis in the camera's own units.
    double width = v->degrees ? 2 * std::atan(std::fabs(v->scale.half_x)) / kRadian : 2 * std::fabs(v->scale.half_x);
    double pan_step = round_step(width / 8);
    double tilt_step = v->degrees ? pan_step : round_step(2 * std::fabs(v->scale.half_y) / 5);

    int count = 0;
    auto label = [](double value, double step, char *out, size_t size) {
        int decimals = std::max(0, std::min(4, static_cast<int>(std::ceil(-std::log10(step) - 1e-9))));
        std::snprintf(out, size, "%.*f", decimals, std::fabs(value) < step * 1e-6 ? 0.0 : value);
    };
    // Adds the line through `at` points (pan, tilt), split where it leaves the range or goes behind the camera.
    auto add = [&](int kind, double value, double step, auto &&at, bool (*keep)(const ptz_view *, double, double)) {
        ptz_plane_line *line = nullptr;
        for (int i = 0; i < PTZ_LINE_POINTS * 2; ++i) {
            double s = static_cast<double>(i) / (PTZ_LINE_POINTS * 2 - 1);
            auto [pan, tilt] = at(s);
            double x = 0, y = 0;
            bool ok = keep(v, pan, tilt) && ptz_view_locate(v, pan, tilt, &x, &y) && std::fabs(x) < 6 && std::fabs(y) < 6;
            if (!ok) {
                line = nullptr;
                continue;
            }
            if (line && line->count == PTZ_LINE_POINTS) {  // full: carry on from its last point
                ptz_plane_line *next = count < max ? &lines[count++] : nullptr;
                if (next) {
                    *next = *line;
                    next->count = 1, next->labelled = 0;
                    next->x[0] = line->x[PTZ_LINE_POINTS - 1], next->y[0] = line->y[PTZ_LINE_POINTS - 1];
                }
                line = next;
            }
            if (!line) {
                if (count >= max) return;
                line = &lines[count++];
                *line = {};
                line->kind = kind;
                line->value = value;
                label(value, step, line->label, sizeof line->label);
            }
            line->x[line->count] = x, line->y[line->count] = y;
            ++line->count;
        }
    };
    auto keep_both = [](const ptz_view *view, double pan, double tilt) {
        return pan_in_range(view, pan) && tilt_in_range(view, tilt);
    };
    double t_lo = tilt_lo, t_hi = tilt_hi;
    if (v->tilt_range.available) t_lo = std::max(t_lo, v->tilt_range.min), t_hi = std::min(t_hi, v->tilt_range.max);
    auto pan_line = [&](int kind, double value, double step) {
        if (t_hi <= t_lo) return;
        add(kind, v->degrees ? into_range(v, std::remainder(value, 360.0)) : value, step,
            [&](double s) { return std::make_pair(value, t_lo + (t_hi - t_lo) * s); }, keep_both);
    };
    auto tilt_line = [&](int kind, double value, double step) {
        if (v->degrees && std::fabs(value) >= 90 - 1e-9) return;  // a pole is a point
        add(kind, value, step, [&](double s) { return std::make_pair(pan_lo + (pan_hi - pan_lo) * s, value); },
            keep_both);
    };

    auto same = [](double a, double b, double step) { return std::fabs(a - b) < step * 1e-6; };
    // Limits first, so a grid line on a limit isn't drawn twice.
    bool full_turn = v->degrees && v->pan_range.available && v->pan_range.max - v->pan_range.min >= 359.9;
    std::vector<double> pan_limits, tilt_limits;
    if (v->pan_range.available && !full_turn)
        for (double limit : {v->pan_range.min, v->pan_range.max}) {
            // The limit's turn nearest the centre, if it shows.
            double u = v->degrees ? v->pan + std::remainder(limit - v->pan, 360.0) : limit;
            if (u >= pan_lo && u <= pan_hi) {
                pan_line(PTZ_LINE_PAN_LIMIT, u, pan_step);
                pan_limits.push_back(u);
            }
        }
    if (v->tilt_range.available)
        for (double limit : {v->tilt_range.min, v->tilt_range.max})
            if (limit >= tilt_lo && limit <= tilt_hi) {
                tilt_line(PTZ_LINE_TILT_LIMIT, limit, tilt_step);
                tilt_limits.push_back(limit);
            }
    for (double k = std::ceil(pan_lo / pan_step); k * pan_step <= pan_hi && count < max; ++k) {
        double u = k * pan_step;
        if (std::any_of(pan_limits.begin(), pan_limits.end(), [&](double l) { return same(l, u, pan_step); })) continue;
        double shown = v->degrees ? std::remainder(u, 360.0) : u;
        pan_line(same(shown, 0, pan_step) ? PTZ_LINE_PAN_ZERO : PTZ_LINE_PAN, u, pan_step);
    }
    for (double k = std::ceil(tilt_lo / tilt_step); k * tilt_step <= tilt_hi && count < max; ++k) {
        double t = k * tilt_step;
        if (std::any_of(tilt_limits.begin(), tilt_limits.end(), [&](double l) { return same(l, t, tilt_step); })) continue;
        tilt_line(same(t, 0, tilt_step) ? PTZ_LINE_TILT_ZERO : PTZ_LINE_TILT, t, tilt_step);
    }

    // Labels where pan lines cross the top edge and tilt lines the right edge; a line that ends inside the picture
    // (at a limit) is labelled at its top (right) end instead.
    auto is_pan = [](const ptz_plane_line &line) {
        return line.kind == PTZ_LINE_PAN || line.kind == PTZ_LINE_PAN_ZERO || line.kind == PTZ_LINE_PAN_LIMIT;
    };
    for (int n = 0; n < count; ++n) {
        ptz_plane_line &line = lines[n];
        bool pan = line.kind == PTZ_LINE_PAN || line.kind == PTZ_LINE_PAN_ZERO || line.kind == PTZ_LINE_PAN_LIMIT;
        for (int i = 1; i < line.count && !line.labelled; ++i) {
            double a = pan ? line.y[i - 1] : line.x[i - 1], b = pan ? line.y[i] : line.x[i];
            if ((a - 1) * (b - 1) > 0 || a == b) continue;
            double f = (1 - a) / (b - a);
            double other = pan ? line.x[i - 1] + (line.x[i] - line.x[i - 1]) * f
                               : line.y[i - 1] + (line.y[i] - line.y[i - 1]) * f;
            if (std::fabs(other) > 1) continue;
            line.labelled = 1;
            (pan ? line.label_x : line.label_y) = other;
            (pan ? line.label_y : line.label_x) = 1;
        }
    }
    for (int n = 0; n < count; ++n) {
        bool pan = is_pan(lines[n]), done = false;
        int best = -1, best_i = 0;
        for (int m = 0; m < count; ++m) {
            if (is_pan(lines[m]) != pan || lines[m].value != lines[n].value) continue;
            done = done || lines[m].labelled;
            for (int i = 0; i < lines[m].count; ++i) {
                double x = lines[m].x[i], y = lines[m].y[i];
                if (std::fabs(x) > 1 || std::fabs(y) > 1) continue;
                if (best < 0 || (pan ? y > lines[best].y[best_i] : x > lines[best].x[best_i])) best = m, best_i = i;
            }
        }
        if (done || best < 0) continue;
        lines[best].labelled = 1;
        lines[best].label_x = lines[best].x[best_i], lines[best].label_y = lines[best].y[best_i];
    }
    return count;
}

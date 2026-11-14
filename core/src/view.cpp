// Measuring how much of the pan/tilt range the camera's picture spans: turn a little, compare pictures.
#include "internal.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
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

}  // namespace

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
        int width = 160, height = std::max(8, static_cast<int>(std::lround(160.0 * size->second / size->first)));
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
    try {
        auto [before, width, height] = picture();
        for (int axis = 0; axis < 2; ++axis) {
            const Range &range = axis == 0 ? camera.pan_range : camera.tilt_range;
            double origin = axis == 0 ? start.pan.value : start.tilt.value, span = range.max - range.min;
            // Turns by `delta` and measures how far the scene moved, looking within `reach` of `expect`.
            auto trial = [&](double delta, double expect, double reach) -> std::optional<std::pair<double, double>> {
                double to = origin + delta <= range.max ? origin + delta : origin - delta;
                Position at = axis == 0 ? go(to, start.tilt.value) : go(start.pan.value, to);
                auto [after, w, h] = picture();
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
            std::optional<double> rough, found;
            for (double delta = span * 0.01; !rough && delta >= span * 0.0002 && delta <= span * 0.2;) {
                auto t = trial(delta, 0, 0.3);
                if (!t) delta /= 4;                                    // no match close by: the view is narrow
                else if (std::fabs(t->second) < 0.03) delta *= 4;      // too little to measure well
                else rough = t->first / -t->second;
            }
            if (rough) {
                double delta = std::min(std::fabs(*rough) * 0.4, span * 0.25);
                if (auto t = trial(delta, -delta / *rough, 0.1 + delta / std::fabs(*rough) * 0.25); t && t->second != 0)
                    found = t->first / -t->second;
                else
                    found = rough;
            }
            if (!found)
                throw Error(std::string("Couldn't measure the view: the pictures showed no clear ") +
                            (axis == 0 ? "sideways" : "up-and-down") + " shift (a plain or changing scene?)");
            (axis == 0 ? scale.pan_per_x : scale.tilt_per_y) = *found;
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
    scale.zoom_available = start.zoom.available;
    scale.zoom = start.zoom.value;
    return scale;
}

}  // namespace ptz

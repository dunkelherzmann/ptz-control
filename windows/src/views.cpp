// Position plane and travel meters, drawn with GDI+ (anti-aliased, part of Windows).
#include "app.h"

#include <windowsx.h>
#include <gdiplus.h>
#include <shlwapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <memory>
#include <vector>

using namespace Gdiplus;

namespace {

Theme g_theme = make_theme(Phosphor::System);
UINT g_dpi = 96;

float S(float v) { return v * static_cast<float>(g_dpi) / 96.0f; }
Color C(COLORREF c, BYTE alpha = 255) { return Color(alpha, GetRValue(c), GetGValue(c), GetBValue(c)); }

const wchar_t *mono_family() {
    static const wchar_t *family = [] {
        FontFamily cascadia(L"Cascadia Mono");
        return cascadia.IsAvailable() ? L"Cascadia Mono" : L"Consolas";
    }();
    return family;
}

void draw_text(Graphics &g, const std::wstring &text, float x, float y, float size, COLORREF color,
               bool bold = false, bool right = false) {
    Font font(mono_family(), S(size), bold ? FontStyleBold : FontStyleRegular, UnitPixel);
    SolidBrush brush(C(color));
    StringFormat format;
    format.SetAlignment(right ? StringAlignmentFar : StringAlignmentNear);
    g.DrawString(text.c_str(), -1, &font, PointF(x, y), &format, &brush);
}

SizeF measure(Graphics &g, const std::wstring &text, float size, bool bold = false) {
    Font font(mono_family(), S(size), bold ? FontStyleBold : FontStyleRegular, UnitPixel);
    RectF box;
    g.MeasureString(text.c_str(), -1, &font, PointF(0, 0), &box);
    return SizeF(box.Width, box.Height);
}

// Paint into an off-screen bitmap, then blit: no flicker. With `print_dc` (WM_PRINTCLIENT, for pictures
// taken without a compositor) it paints into that DC instead of the window.
template <typename F>
void buffered_paint(HWND hwnd, F &&paint, HDC print_dc = nullptr) {
    PAINTSTRUCT ps;
    HDC dc = print_dc ? print_dc : BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom));
    HGDIOBJ old = SelectObject(mem, bitmap);
    {
        Graphics g(mem);
        g.SetSmoothingMode(SmoothingModeAntiAlias);
        g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
        g.Clear(C(g_theme.panel));
        paint(g, static_cast<float>(rc.right), static_cast<float>(rc.bottom));
    }
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bitmap);
    DeleteDC(mem);
    if (!print_dc) EndPaint(hwnd, &ps);
}

void track_leave(HWND hwnd) {
    TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd, 0};
    TrackMouseEvent(&t);
}

// ---- position plane: the camera picture with the pan/tilt range over it ------------------

// One of the plane's lines from the core, in picture points (-1…1, up positive).
struct PlaneLine {
    enum Kind { Grid, Zero, Limit } kind;
    bool pan;
    std::wstring label;
    bool labelled;
    double label_x, label_y;
    std::vector<std::pair<double, double>> points;
};

struct Plane {
    PlaneState state;
    std::optional<PointF> hover;  // client pixels
    bool dragging = false;
    std::vector<double> lines_key;  // the view the lines were worked out for
    std::vector<PlaneLine> lines;
};

// The picture's frame: as large as fits, in the picture's shape (16:9 until there is
// one), centred.
RectF picture_rect(const PlaneState &s, float w, float h) {
    RectF area(1, 1, std::max(1.0f, w - 2), std::max(1.0f, h - 2));
    float pw = s.picture ? static_cast<float>(s.picture_width) : 16, ph = s.picture ? static_cast<float>(s.picture_height) : 9;
    if (pw <= 0 || ph <= 0) return area;
    float scale = std::min(area.Width / pw, area.Height / ph), pw2 = std::round(pw * scale), ph2 = std::round(ph * scale);
    return RectF(std::round(area.X + (area.Width - pw2) / 2), std::round(area.Y + (area.Height - ph2) / 2), pw2, ph2);
}

// A client point as a point of the picture, -1…1 with up positive, and back.
PointF picture_point(const RectF &r, float x, float y) {
    return PointF((x - (r.X + r.Width / 2)) / (r.Width / 2), ((r.Y + r.Height / 2) - y) / (r.Height / 2));
}
PointF client_point(const RectF &r, double x, double y) {
    return PointF(r.X + r.Width / 2 + static_cast<float>(x) * r.Width / 2, r.Y + r.Height / 2 - static_cast<float>(y) * r.Height / 2);
}

ptz_range core_range(const AxisRange &r) { return ptz_range{r.available ? 1 : 0, r.min, r.max}; }

// Where the camera points and how wide the picture is, once both are known.
std::optional<ptz_view> view_of(const PlaneState &s) {
    const auto &c = s.current;
    if (!s.scale || !c || !c->pan || !c->tilt) return std::nullopt;
    return ptz_view{{s.scale->half_x, s.scale->half_y}, s.degrees ? 1 : 0, c->pan->value, c->tilt->value,
                    core_range(s.pan_range), core_range(s.tilt_range)};
}

// Pan and tilt -> client pixels (possibly outside the picture); none behind the camera or before the view is measured.
std::optional<PointF> locate(const PlaneState &s, const RectF &r, double pan, double tilt) {
    auto v = view_of(s);
    double x = 0, y = 0;
    if (!v || !ptz_view_locate(&*v, pan, tilt, &x, &y)) return std::nullopt;
    return client_point(r, x, y);
}

// Client pixels -> pan and tilt.
std::optional<std::pair<double, double>> camera_point(const PlaneState &s, const RectF &r, float x, float y) {
    auto v = view_of(s);
    if (!v) return std::nullopt;
    PointF q = picture_point(r, x, y);
    double pan = 0, tilt = 0;
    ptz_view_point(&*v, q.X, q.Y, &pan, &tilt);
    return std::make_pair(pan, tilt);
}

float plate_height(Graphics &g, float size = 10) { return measure(g, L"PICTURE", size, true).Height + S(4); }

// A label on a dark plate, readable on any picture; `x`, `y` is its top-left (or top-right) corner.
void plate(Graphics &g, const std::wstring &text, float x, float y, COLORREF color, bool align_right = false, float size = 10) {
    SizeF box = measure(g, text, size, true);
    float w = box.Width + S(8), h = box.Height + S(4);
    if (align_right) x -= w;
    GraphicsPath path;
    float d = S(6);
    path.AddArc(x, y, d, d, 180, 90);
    path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    path.AddArc(x, y + h - d, d, d, 90, 90);
    path.CloseFigure();
    SolidBrush fill(Color(140, 0, 0, 0));
    g.FillPath(&fill, &path);
    draw_text(g, text, x + S(4), y + S(2), size, color, true);
}

// A pen dashed in pixels (GDI+ dash lengths are in pen widths).
void dash(Pen &pen, float on, float off) {
    REAL pattern[] = {S(on) / pen.GetWidth(), S(off) / pen.GetWidth()};
    pen.SetDashPattern(pattern, 2);
}

// The plate in the lower left: live, when the picture was taken, or that it is out of date.
void draw_status(Graphics &g, const PlaneState &s, const RectF &r) {
    if (!s.picture) return;
    std::wstring status = !s.live.empty() ? L"● LIVE · " + s.live
                        : s.picture_stale ? L"MOVING · NEW PICTURE WHEN IT STOPS"
                        : s.picture_taken.empty() ? L"PICTURE" : L"PICTURE " + s.picture_taken;
    plate(g, status, r.X + S(6), r.GetBottom() - S(6) - plate_height(g), s.picture_stale ? g_theme.warning : RGB(255, 255, 255));
}

// The plane's lines for the current view, worked out by the core and kept until the view changes.
const std::vector<PlaneLine> &plane_lines(Plane &p) {
    auto v = view_of(p.state);
    if (!v) {
        p.lines_key.clear();
        p.lines.clear();
        return p.lines;
    }
    std::vector<double> key{v->scale.half_x, v->scale.half_y, double(v->degrees), v->pan, v->tilt,
                            v->pan_range.min, v->pan_range.max, double(v->pan_range.available),
                            v->tilt_range.min, v->tilt_range.max, double(v->tilt_range.available)};
    if (key == p.lines_key) return p.lines;
    std::vector<ptz_plane_line> raw(160);
    int count = ptz_view_lines(&*v, raw.data(), static_cast<int>(raw.size()));
    p.lines.clear();
    for (int i = 0; i < count; ++i) {
        const ptz_plane_line &l = raw[i];
        PlaneLine line;
        line.kind = l.kind == PTZ_LINE_PAN_LIMIT || l.kind == PTZ_LINE_TILT_LIMIT ? PlaneLine::Limit
                    : l.kind == PTZ_LINE_PAN_ZERO || l.kind == PTZ_LINE_TILT_ZERO ? PlaneLine::Zero
                                                                                  : PlaneLine::Grid;
        line.pan = l.kind == PTZ_LINE_PAN || l.kind == PTZ_LINE_PAN_ZERO || l.kind == PTZ_LINE_PAN_LIMIT;
        line.label = W(std::string(l.label, strnlen(l.label, sizeof l.label)));
        line.labelled = l.labelled != 0;
        line.label_x = l.label_x;
        line.label_y = l.label_y;
        for (int j = 0; j < l.count && j < PTZ_LINE_POINTS; ++j) line.points.emplace_back(l.x[j], l.y[j]);
        p.lines.push_back(std::move(line));
    }
    p.lines_key = key;
    return p.lines;
}

void paint_plane(Plane &p, Graphics &g, float w, float h) {
    const PlaneState &s = p.state;
    RectF r = picture_rect(s, w, h);
    const COLORREF white = RGB(255, 255, 255);

    // The picture, or why there is none.
    SolidBrush black(Color(255, 0, 0, 0));
    g.FillRectangle(&black, r);
    if (s.picture) {
        g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
        if (s.picture_stale) {
            ColorMatrix dim = {{{1, 0, 0, 0, 0}, {0, 1, 0, 0, 0}, {0, 0, 1, 0, 0}, {0, 0, 0, 0.45f, 0}, {0, 0, 0, 0, 1}}};
            ImageAttributes attributes;
            attributes.SetColorMatrix(&dim);
            g.DrawImage(s.picture.get(), r, 0, 0, static_cast<REAL>(s.picture_width), static_cast<REAL>(s.picture_height),
                        UnitPixel, &attributes);
        } else {
            g.DrawImage(s.picture.get(), r);
        }
    } else {
        std::wstring title = !s.enabled ? L"NO CAMERA LINK" : s.picture_note.empty() ? L"TAKING A PICTURE…" : L"NO PICTURE";
        SizeF size = measure(g, title, 12, true);
        draw_text(g, title, r.X + (r.Width - size.Width) / 2, r.Y + r.Height / 2 - size.Height - S(2), 12, g_theme.secondary, true);
        if (s.enabled && !s.picture_note.empty()) {
            Font font(L"Segoe UI", S(11), FontStyleRegular, UnitPixel);
            SolidBrush brush(C(g_theme.secondary));
            StringFormat format;
            format.SetAlignment(StringAlignmentCenter);
            RectF box(r.X + S(24), r.Y + r.Height / 2 + S(4), r.Width - S(48), S(40));
            g.DrawString(s.picture_note.c_str(), -1, &font, box, &format, &brush);
        }
    }
    Pen frame(C(white, 64), 1);
    g.DrawRectangle(&frame, r.X + 0.5f, r.Y + 0.5f, r.Width - 1, r.Height - 1);

    if (!s.pan_range.available || !s.tilt_range.available) {
        if (s.enabled)
            plate(g, L"PAN/TILT RANGE UNAVAILABLE", r.GetRight() - S(6), r.GetBottom() - S(6) - plate_height(g), g_theme.warning, true);
        draw_status(g, s, r);
        return;
    }

    // Everything on the plane stays inside the picture. Each stroke is drawn first wide and dark, a halo that keeps
    // it readable on any picture.
    g.SetClip(r);
    const Color halo(200, 0, 0, 0);
    const float rim = S(2.5f);

    // The grid, the zero axes and the range's limits, where they fall in the picture.
    static const std::vector<PlaneLine> no_lines;
    const std::vector<PlaneLine> &lines = s.enabled ? plane_lines(p) : no_lines;
    if (s.show_grid) {
        for (const PlaneLine &line : lines) {
            if (line.points.size() < 2) continue;
            std::vector<PointF> points;
            for (auto [x, y] : line.points) points.push_back(client_point(r, x, y));
            float width = line.kind == PlaneLine::Grid ? S(0.75f) : line.kind == PlaneLine::Zero ? S(1) : S(1.5f);
            Color ink = line.kind == PlaneLine::Grid ? C(white, 115) : line.kind == PlaneLine::Zero ? C(white, 178)
                                                                                                    : C(g_theme.warning, 230);
            Pen dark(Color(110, 0, 0, 0), width + rim), pen(ink, width);
            for (Pen *each : {&dark, &pen}) {
                each->SetLineJoin(LineJoinRound);
                if (line.kind == PlaneLine::Limit) dash(*each, 6, 3);
                g.DrawLines(each, points.data(), static_cast<INT>(points.size()));
            }
        }
    }

    // Recent measured path; gaps stay gaps.
    {
        Pen dark(halo, S(1.5f) + rim), ink(C(g_theme.primary, 153), S(1.5f));
        for (Pen *pen : {&dark, &ink}) {
            pen->SetLineJoin(LineJoinRound);
            std::optional<PointF> last;
            for (const auto &sample : s.trail) {
                std::optional<PointF> point = sample ? locate(s, r, sample->first, sample->second) : std::nullopt;
                if (point && last) g.DrawLine(pen, *last, *point);
                last = point;
            }
        }
    }

    // Starting position: hollow square marked S.
    if (s.starting && s.starting->pan && s.starting->tilt) {
        if (auto sp = locate(s, r, s.starting->pan->value, s.starting->tilt->value)) {
            Pen dark(halo, S(1.5f) + rim), ink(C(g_theme.start), S(1.5f));
            for (Pen *pen : {&dark, &ink}) g.DrawRectangle(pen, sp->X - S(5), sp->Y - S(5), S(10), S(10));
            GraphicsPath letter;
            FontFamily family(mono_family());
            letter.AddString(L"S", -1, &family, FontStyleBold, S(9), PointF(sp->X + S(7), sp->Y - S(16)), nullptr);
            Pen outline(halo, rim);
            outline.SetLineJoin(LineJoinRound);
            g.DrawPath(&outline, &letter);
            SolidBrush fill(C(g_theme.start));
            g.FillPath(&fill, &letter);
        }
    }

    // Target: dashed crosshair and ring (an axis left blank keeps the current value), with a dotted line from the
    // picture's centre, where the camera points.
    const PointF centre(r.X + r.Width / 2, r.Y + r.Height / 2);
    if (s.target_pan || s.target_tilt) {
        std::optional<double> pan = s.target_pan, tilt = s.target_tilt;
        if (!pan && s.current && s.current->pan) pan = s.current->pan->value;
        if (!tilt && s.current && s.current->tilt) tilt = s.current->tilt->value;
        if (auto t = pan && tilt ? locate(s, r, *pan, *tilt) : std::nullopt) {
            Pen dark(halo, S(1.5f) + rim), ink(C(g_theme.target), S(1.5f));
            for (Pen *pen : {&dark, &ink}) {
                dash(*pen, 4, 2);
                g.DrawLine(pen, t->X - S(14), t->Y, t->X + S(14), t->Y);
                g.DrawLine(pen, t->X, t->Y - S(14), t->X, t->Y + S(14));
                g.DrawEllipse(pen, t->X - S(8), t->Y - S(8), S(16), S(16));
            }
            Pen dark_line(Color(140, 0, 0, 0), S(1) + rim), line(C(g_theme.target, 178), S(1));
            for (Pen *pen : {&dark_line, &line}) {
                dash(*pen, 2, 3);
                g.DrawLine(pen, centre, *t);
            }
        }
    }

    // Where the camera points: the picture's centre, a solid reticle once the view is measured (dashed until then).
    const bool positioned = s.current && s.current->pan && s.current->tilt;
    if (s.enabled && positioned) {
        Pen dark(halo, S(1.5f) + rim), ink(C(g_theme.primary), S(1.5f));
        for (Pen *pen : {&dark, &ink}) {
            if (!s.scale) dash(*pen, 3, 3);
            g.DrawEllipse(pen, centre.X - S(10), centre.Y - S(10), S(20), S(20));
        }
        SolidBrush shade(halo), dot(C(g_theme.primary));
        g.FillEllipse(&shade, centre.X - S(2.5f) - rim / 2, centre.Y - S(2.5f) - rim / 2, S(5) + rim, S(5) + rim);
        g.FillEllipse(&dot, centre.X - S(2.5f), centre.Y - S(2.5f), S(5), S(5));
    }
    g.ResetClip();

    // The lines' values: pan along the top edge, tilt down the right one, each where its line crosses the edge (or at
    // the end of a line that ends inside the picture).
    if (s.show_grid) {
        std::vector<const PlaneLine *> pans, tilts;
        for (const PlaneLine &line : lines)
            if (line.labelled) (line.pan ? pans : tilts).push_back(&line);
        std::sort(pans.begin(), pans.end(), [](auto a, auto b) { return a->label_x < b->label_x; });
        std::sort(tilts.begin(), tilts.end(), [](auto a, auto b) { return a->label_y > b->label_y; });
        const float lh = plate_height(g, 9);
        auto width = [&](const std::wstring &text) { return measure(g, text, 9, true).Width + S(8); };
        float last_pan = -1e9f;
        std::vector<RectF> pan_plates;
        for (const PlaneLine *line : pans) {
            std::wstring text = L"PAN " + line->label + (line->kind == PlaneLine::Limit ? L" LIMIT" : L"");
            float tw = width(text);
            PointF at = client_point(r, line->label_x, line->label_y);
            float x = std::min(std::max(at.X - tw / 2, r.X + S(4)), r.GetRight() - S(4) - tw);
            if (x <= last_pan + S(4)) continue;
            float y = std::min(std::max(at.Y, r.Y) + S(19), r.GetBottom() - S(26)) - lh;
            plate(g, text, x, y, line->kind == PlaneLine::Limit ? g_theme.warning : white, false, 9);
            pan_plates.emplace_back(x, y, tw, lh);
            last_pan = x + tw;
        }
        float last_tilt = -1e9f;
        for (const PlaneLine *line : tilts) {
            std::wstring text = L"TILT " + line->label + (line->kind == PlaneLine::Limit ? L" LIMIT" : L"");
            float tw = width(text);
            PointF at = client_point(r, line->label_x, line->label_y);
            float right = std::min(at.X, r.GetRight()) - S(4);
            float y = std::max(std::min(at.Y - lh / 2, r.GetBottom() - S(26) - lh), r.Y + S(24));
            // Below any pan label it would cover.
            for (bool moved = true; moved;) {
                moved = false;
                for (const RectF &hit : pan_plates) {
                    RectF box(right - tw, y, tw, lh);
                    if (hit.IntersectsWith(box)) {
                        y = hit.GetBottom() + S(2);
                        moved = true;
                    }
                }
            }
            if (y < last_tilt + lh + S(2) || y > r.GetBottom() - S(26) - lh) continue;
            plate(g, text, right, y, line->kind == PlaneLine::Limit ? g_theme.warning : white, true, 9);
            last_tilt = y;
        }
    }
    if (s.enabled && s.current && !positioned) {
        plate(g, L"POSITION UNAVAILABLE", r.GetRight() - S(6), r.GetBottom() - S(6) - plate_height(g), g_theme.warning, true);
    } else if (s.enabled && s.measuring) {
        plate(g, L"MEASURING THE VIEW · THE CAMERA TURNS A LITTLE AND BACK", r.X + S(6), r.Y + S(26), g_theme.warning);
    } else if (s.enabled && !s.scale && positioned) {
        plate(g, L"VIEW NOT MEASURED AT THIS ZOOM · DOUBLE-CLICK TO MEASURE AND MOVE (Ctrl+Shift+M)", r.X + S(6), r.Y + S(26),
              g_theme.warning);
    }

    draw_status(g, s, r);

    // Hover: guides and the pan and tilt under the pointer.
    if (p.hover && s.enabled && r.Contains(*p.hover)) {
        PointF hv = *p.hover;
        if (auto camera = camera_point(s, r, hv.X, hv.Y)) {
            g.SetClip(r);
            Pen guide(C(white, 89), S(0.5f));
            g.DrawLine(&guide, hv.X, r.Y, hv.X, r.GetBottom());
            g.DrawLine(&guide, r.X, hv.Y, r.GetRight(), hv.Y);
            std::wstring text = L"PAN " + format_value(camera->first) + L"  TILT " + format_value(camera->second);
            float ph = plate_height(g);
            float width = measure(g, text, 10, true).Width + S(18);
            float x = hv.X + S(10), y = hv.Y - S(8) - ph;
            if (x + width > r.GetRight()) x = hv.X - width - S(10);
            if (y < r.Y) y = hv.Y + S(8);
            plate(g, text, x, y, white);
            g.ResetClip();
        }
    }
}

void plane_aim(HWND hwnd, Plane &p, int x, int y, bool commit) {
    const PlaneState &s = p.state;
    if (!s.enabled || s.measuring || !s.pan_range.available || !s.tilt_range.available) return;
    RECT rc;
    GetClientRect(hwnd, &rc);
    RectF r = picture_rect(s, static_cast<float>(rc.right), static_cast<float>(rc.bottom));
    if (r.IsEmptyArea()) return;
    float px = std::clamp(static_cast<float>(x), r.X, r.GetRight()), py = std::clamp(static_cast<float>(y), r.Y, r.GetBottom());
    if (auto camera = camera_point(s, r, px, py)) {
        if (commit) {
            if (s.on_aim_and_move) s.on_aim_and_move(camera->first, camera->second);
        } else if (s.on_aim) {
            s.on_aim(camera->first, camera->second);
        }
    } else if (commit && s.on_measure_and_move) {
        PointF q = picture_point(r, px, py);
        s.on_measure_and_move(q.X, q.Y);
    }
}

LRESULT CALLBACK plane_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto *p = reinterpret_cast<Plane *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new Plane));
            break;
        case WM_NCDESTROY:
            delete p;
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            buffered_paint(hwnd, [&](Graphics &g, float w, float h) { paint_plane(*p, g, w, h); });
            return 0;
        case WM_PRINTCLIENT:
            buffered_paint(hwnd, [&](Graphics &g, float w, float h) { paint_plane(*p, g, w, h); }, reinterpret_cast<HDC>(wp));
            return 0;
        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            RECT rc;
            GetClientRect(hwnd, &rc);
            RectF r = picture_rect(p->state, static_cast<float>(rc.right), static_cast<float>(rc.bottom));
            PointF point(static_cast<float>(x), static_cast<float>(y));
            p->hover = p->state.enabled && r.Contains(point) ? std::optional<PointF>(point) : std::nullopt;
            if (p->dragging) plane_aim(hwnd, *p, x, y, false);
            track_leave(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSELEAVE:
            p->hover.reset();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            SetCapture(hwnd);
            p->dragging = true;
            plane_aim(hwnd, *p, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), false);
            return 0;
        case WM_LBUTTONUP:
            p->dragging = false;
            ReleaseCapture();
            return 0;
        case WM_LBUTTONDBLCLK:
            plane_aim(hwnd, *p, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), true);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---- travel meter ----------------------------------------------------------------

struct Meter {
    MeterState state;
    std::wstring title;
    std::optional<double> hover;
    bool dragging = false;
};

RectF bar_rect(float w) { return RectF(S(44), S(4), std::max(1.0f, w - S(44) - S(50)), S(10)); }

std::optional<double> meter_value(HWND hwnd, Meter &m, int x) {
    if (!m.state.range.available) return std::nullopt;
    RECT rc;
    GetClientRect(hwnd, &rc);
    RectF bar = bar_rect(static_cast<float>(rc.right));
    double v = m.state.range.value_at((x - bar.X) / bar.Width);
    if (m.state.integer)
        v = std::min(std::max(std::round(v), std::ceil(m.state.range.min)), std::floor(m.state.range.max));
    return v;
}

void paint_meter(Meter &m, Graphics &g, float w, float) {
    const MeterState &s = m.state;
    draw_text(g, m.title, 0, S(1), 10, g_theme.secondary, true);
    RectF bar = bar_rect(w);
    SolidBrush track(C(g_theme.grid));
    g.FillRectangle(&track, bar);
    Pen frame(C(g_theme.border), 1);
    g.DrawRectangle(&frame, bar);

    std::wstring percent = L" N/A";
    std::optional<double> f = s.value ? s.range.fraction(*s.value) : std::nullopt;
    if (f) {
        SolidBrush fill(C(g_theme.primary));
        g.FillRectangle(&fill, bar.X, bar.Y, std::max(S(2), bar.Width * static_cast<float>(*f)), bar.Height);
        wchar_t text[16];
        swprintf(text, 16, L"%3.0f%%", *f * 100);
        percent = text;
    } else {
        // Unavailable is hatched, never an empty (zero) bar.
        HatchBrush hatch(HatchStyleWideUpwardDiagonal, C(g_theme.secondary, 110), C(g_theme.grid));
        g.FillRectangle(&hatch, bar);
    }
    auto tick = [&](std::optional<double> v, COLORREF color, bool dashed) {
        if (!v) return;
        auto fr = s.range.fraction(*v);
        if (!fr) return;
        float x = bar.X + bar.Width * static_cast<float>(*fr);
        Pen pen(C(color), S(1.5f));
        if (dashed) pen.SetDashStyle(DashStyleDash);
        g.DrawLine(&pen, x, bar.Y - S(3), x, bar.GetBottom() + S(3));
    };
    tick(s.start, g_theme.start, false);
    tick(s.target, g_theme.target, true);
    if (s.enabled && s.target) {
        if (auto fr = s.range.fraction(*s.target)) {
            float x = bar.X + bar.Width * static_cast<float>(*fr);
            PointF handle[3] = {PointF(x - S(4), bar.Y - S(6)), PointF(x + S(4), bar.Y - S(6)), PointF(x, bar.Y - S(1))};
            SolidBrush brush(C(g_theme.target));
            g.FillPolygon(&brush, handle, 3);
        }
    }
    draw_text(g, percent, bar.GetRight() + S(8), bar.Y - S(2), 10, g_theme.text);
    if (s.range.available) {
        draw_text(g, format_value(s.range.min), bar.X, bar.GetBottom() + S(3), 9, g_theme.secondary);
        draw_text(g, format_value(s.range.max), bar.GetRight(), bar.GetBottom() + S(3), 9, g_theme.secondary, false, true);
    }
    if (m.hover && s.enabled) {
        if (auto fr = s.range.fraction(*m.hover)) {
            float x = bar.X + bar.Width * static_cast<float>(*fr);
            Pen guide(C(g_theme.axis), 1);
            g.DrawLine(&guide, x, bar.Y - S(2), x, bar.GetBottom() + S(2));
            std::wstring text = format_value(*m.hover);
            float tw = measure(g, text, 9, true).Width;
            draw_text(g, text, std::clamp(x - tw / 2, bar.X + S(30), bar.GetRight() - tw - S(30)), bar.GetBottom() + S(3), 9,
                      g_theme.text, true);
        }
    }
}

LRESULT CALLBACK meter_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto *m = reinterpret_cast<Meter *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_NCCREATE: {
            auto *meter = new Meter;
            auto *create = reinterpret_cast<CREATESTRUCTW *>(lp);
            meter->title = create->lpszName ? create->lpszName : L"";
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(meter));
            break;
        }
        case WM_NCDESTROY:
            delete m;
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            buffered_paint(hwnd, [&](Graphics &g, float w, float h) { paint_meter(*m, g, w, h); });
            return 0;
        case WM_PRINTCLIENT:
            buffered_paint(hwnd, [&](Graphics &g, float w, float h) { paint_meter(*m, g, w, h); }, reinterpret_cast<HDC>(wp));
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lp) == HTCLIENT && m->state.enabled && m->state.range.available) {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return TRUE;
            }
            break;
        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lp);
            m->hover = meter_value(hwnd, *m, x);
            if (m->dragging && m->hover && m->state.on_aim) m->state.on_aim(*m->hover);
            track_leave(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSELEAVE:
            m->hover.reset();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN:
            if (m->state.enabled) {
                SetCapture(hwnd);
                m->dragging = true;
                if (auto v = meter_value(hwnd, *m, GET_X_LPARAM(lp)); v && m->state.on_aim) m->state.on_aim(*v);
            }
            return 0;
        case WM_LBUTTONUP:
            m->dragging = false;
            ReleaseCapture();
            return 0;
        case WM_LBUTTONDBLCLK:
            if (m->state.enabled)
                if (auto v = meter_value(hwnd, *m, GET_X_LPARAM(lp)); v && m->state.on_aim_and_move) m->state.on_aim_and_move(*v);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

void register_instrument_classes(HINSTANCE instance) {
    WNDCLASSW wc{};
    wc.style = CS_DBLCLKS | CS_HREDRAW | CS_VREDRAW;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    wc.lpfnWndProc = plane_proc;
    wc.lpszClassName = L"PTZPlane";
    RegisterClassW(&wc);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = meter_proc;
    wc.lpszClassName = L"PTZMeter";
    RegisterClassW(&wc);
}

HWND create_plane(HWND parent, int id) {
    return CreateWindowExW(0, L"PTZPlane", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
}

HWND create_meter(HWND parent, int id, const wchar_t *title) {
    return CreateWindowExW(0, L"PTZMeter", title, WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
}

PlaneState &plane_state(HWND plane) { return reinterpret_cast<Plane *>(GetWindowLongPtrW(plane, GWLP_USERDATA))->state; }

void PlaneState::record_trail() {
    if (!current || !current->pan || !current->tilt) {
        if (!trail.empty() && trail.back()) trail.emplace_back(std::nullopt);
        return;
    }
    std::pair<double, double> point{current->pan->value, current->tilt->value};
    if (!trail.empty() && trail.back() && *trail.back() == point) return;
    trail.emplace_back(point);
    if (trail.size() > 120) trail.erase(trail.begin(), trail.end() - 120);
}

bool plane_set_picture(HWND plane, const std::string &bytes) {
    PlaneState &s = plane_state(plane);
    IStream *stream = SHCreateMemStream(reinterpret_cast<const BYTE *>(bytes.data()), static_cast<UINT>(bytes.size()));
    if (!stream) return false;
    auto bitmap = std::make_shared<Bitmap>(stream);
    stream->Release();
    if (bitmap->GetLastStatus() != Ok || bitmap->GetWidth() == 0) return false;
    s.picture = bitmap;
    s.picture_width = static_cast<int>(bitmap->GetWidth());
    s.picture_height = static_cast<int>(bitmap->GetHeight());
    return true;
}

bool plane_set_frame(HWND plane, const LiveFrame &frame) {
    if (frame.width <= 0 || frame.height <= 0) return false;
    PlaneState &s = plane_state(plane);
    auto bitmap = std::make_shared<Bitmap>(frame.width, frame.height, PixelFormat32bppRGB);
    Rect all(0, 0, frame.width, frame.height);
    BitmapData data;
    if (bitmap->GetLastStatus() != Ok || bitmap->LockBits(&all, ImageLockModeWrite, PixelFormat32bppRGB, &data) != Ok) return false;
    const size_t row = static_cast<size_t>(frame.width) * 4;
    for (int y = 0; y < frame.height; ++y)
        std::memcpy(static_cast<BYTE *>(data.Scan0) + static_cast<ptrdiff_t>(y) * data.Stride, frame.bgra.data() + y * row, row);
    bitmap->UnlockBits(&data);
    s.picture = bitmap;
    s.picture_width = frame.width;
    s.picture_height = frame.height;
    return true;
}

MeterState &meter_state(HWND meter) { return reinterpret_cast<Meter *>(GetWindowLongPtrW(meter, GWLP_USERDATA))->state; }

void instruments_set_theme(const Theme &theme, UINT dpi) {
    g_theme = theme;
    g_dpi = dpi;
}

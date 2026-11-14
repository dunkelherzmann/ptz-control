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

// ---- position plane: the camera picture -----------------------------------------------

struct Plane {
    PlaneState state;
    std::optional<PointF> hover;  // client pixels
    bool dragging = false;
};

// The picture's frame: as large as fits, in the picture's shape (16:9 until there is one), centred.
RectF picture_rect(const PlaneState &s, float w, float h) {
    RectF area(1, 1, std::max(1.0f, w - 2), std::max(1.0f, h - 2));
    float pw = s.picture ? static_cast<float>(s.picture_width) : 16, ph = s.picture ? static_cast<float>(s.picture_height) : 9;
    if (pw <= 0 || ph <= 0) return area;
    float scale = std::min(area.Width / pw, area.Height / ph), pw2 = std::round(pw * scale), ph2 = std::round(ph * scale);
    return RectF(std::round(area.X + (area.Width - pw2) / 2), std::round(area.Y + (area.Height - ph2) / 2), pw2, ph2);
}

PointF picture_point(const RectF &r, float x, float y) {  // client pixels -> view units (up positive)
    return PointF((x - r.X) / r.Width * 2 - 1, 1 - (y - r.Y) / r.Height * 2);
}

// A label on a dark plate, readable on any picture; `x`, `y` is its top-left (or top-right) corner. Returns its height.
float plate(Graphics &g, const std::wstring &text, float x, float y, COLORREF color, bool align_right = false) {
    SizeF size = measure(g, text, 10, true);
    float w = size.Width + S(8), h = size.Height + S(4);
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
    draw_text(g, text, x + S(4), y + S(2), 10, color, true);
    return h;
}

float plate_height(Graphics &g) { return measure(g, L"PICTURE", 10, true).Height + S(4); }

PointF widget_point(const RectF &r, double vx, double vy) {  // view units -> client pixels
    return PointF(r.X + static_cast<float>((vx + 1) / 2) * r.Width, r.Y + static_cast<float>((1 - vy) / 2) * r.Height);
}

// The point of the picture (view units) showing pan and tilt, with a measured view.
std::optional<PointF> view_point(const PlaneState &s, double pan, double tilt) {
    const auto &o = s.overlay;
    if (!o || o->scale.pan_per_x == 0 || o->scale.tilt_per_y == 0) return std::nullopt;
    return PointF(static_cast<float>((pan - o->pan) / o->scale.pan_per_x), static_cast<float>((tilt - o->tilt) / o->scale.tilt_per_y));
}

bool inside(const std::optional<PointF> &v) { return v && std::fabs(v->X) <= 1 && std::fabs(v->Y) <= 1; }

// A value with as many decimals as a step of `step` needs.
std::wstring label_value(double value, double step) {
    int decimals = std::clamp(static_cast<int>(std::ceil(-std::log10(std::max(std::fabs(step), 1e-9)))) + 1, 0, 4);
    wchar_t text[64];
    swprintf(text, 64, L"%.*f", decimals, value);
    return text;
}

void paint_plane(Plane &p, Graphics &g, float w, float h) {
    const PlaneState &s = p.state;
    RectF r = picture_rect(s, w, h);
    const COLORREF white = RGB(255, 255, 255);
    SolidBrush black(Color(255, 0, 0, 0));
    g.FillRectangle(&black, r);
    if (!s.picture) {
        Pen grid(C(g_theme.grid), 1);
        for (int i = 0; i <= 10; ++i) {
            float x = r.X + r.Width * i / 10, y = r.Y + r.Height * i / 10;
            g.DrawLine(&grid, x, r.Y, x, r.GetBottom());
            g.DrawLine(&grid, r.X, y, r.GetRight(), y);
        }
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
        return;
    }
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

    // View grid: tenths of the picture, the centre lines stronger.
    Pen grid(C(white, 41), S(0.5f));
    for (int i = 1; i <= 9; ++i) {
        if (i == 5) continue;
        float x = r.X + r.Width * i / 10, y = r.Y + r.Height * i / 10;
        g.DrawLine(&grid, x, r.Y, x, r.GetBottom());
        g.DrawLine(&grid, r.X, y, r.GetRight(), y);
    }
    PointF c(r.X + r.Width / 2, r.Y + r.Height / 2);
    Pen axes(C(white, 77), S(0.75f));
    g.DrawLine(&axes, c.X, r.Y, c.X, r.GetBottom());
    g.DrawLine(&axes, r.X, c.Y, r.GetRight(), c.Y);

    // Pan and tilt over the picture, once its view is measured: pan along the top edge and tilt along the right one
    // (the centre values named), and the starting position as a square marked S.
    if (const auto &o = s.overlay) {
        Font font(mono_family(), S(9), FontStyleBold, UnitPixel);
        SolidBrush ink(Color(255, 255, 255, 255)), shade(Color(128, 0, 0, 0));
        for (int i = -4; i <= 4; i += 2) {
            float f = i / 5.0f;
            std::wstring pan = label_value(o->pan + f * o->scale.pan_per_x, std::fabs(o->scale.pan_per_x) * 0.4);
            std::wstring tilt = label_value(o->tilt + f * o->scale.tilt_per_y, std::fabs(o->scale.tilt_per_y) * 0.4);
            if (i == 0) pan = L"PAN " + pan, tilt = L"TILT " + tilt;
            for (int which = 0; which < 2; ++which) {
                const std::wstring &text = which ? tilt : pan;
                SizeF size = measure(g, text, 9, true);
                float w = size.Width + S(4), h = size.Height + S(1);
                RectF box = which ? RectF(r.GetRight() - S(3) - w, c.Y - f * r.Height / 2 - h / 2, w, h)
                                  : RectF(c.X + f * r.Width / 2 - w / 2, r.Y + S(3), w, h);
                g.FillRectangle(&shade, box);
                g.DrawString(text.c_str(), -1, &font, PointF(box.X + S(2), box.Y), &ink);
            }
        }
        if (s.starting && s.starting->pan && s.starting->tilt) {
            auto v = view_point(s, s.starting->pan->value, s.starting->tilt->value);
            if (inside(v)) {
                PointF sp = widget_point(r, v->X, v->Y);
                Pen start(C(g_theme.start), S(1.5f));
                g.DrawRectangle(&start, sp.X - S(5), sp.Y - S(5), S(10), S(10));
                draw_text(g, L"S", sp.X + S(7), sp.Y - S(16), 9, g_theme.start, true);
            }
        }
    }

    // Where the camera points: the picture's centre, or with a measured view, its latest reading (it may have moved
    // since the picture).
    if (s.current && s.current->pan && s.current->tilt) {
        auto v = view_point(s, s.current->pan->value, s.current->tilt->value);
        if (inside(v)) c = widget_point(r, v->X, v->Y);
    }
    Pen ring(C(g_theme.primary), S(1.5f));
    g.DrawEllipse(&ring, c.X - S(10), c.Y - S(10), S(20), S(20));
    SolidBrush dot(C(g_theme.primary));
    g.FillEllipse(&dot, c.X - S(3), c.Y - S(3), S(6), S(6));

    // Target: dashed crosshair, with a line from where the camera points. With a measured view it is the absolute
    // target (an axis left blank keeps the current value).
    std::optional<PointF> aimed;
    if (s.picture_target) aimed = PointF(s.picture_target->first, s.picture_target->second);
    if (s.overlay && (s.target_pan || s.target_tilt)) {
        std::optional<double> pan = s.target_pan, tilt = s.target_tilt;
        if (!pan && s.current && s.current->pan) pan = s.current->pan->value;
        if (!tilt && s.current && s.current->tilt) tilt = s.current->tilt->value;
        if (pan && tilt) {
            auto v = view_point(s, *pan, *tilt);
            if (inside(v)) aimed = v;
        }
    }
    if (aimed) {
        PointF t = widget_point(r, aimed->X, aimed->Y);
        Pen dashed(C(g_theme.target), S(1.5f));
        REAL dash[] = {4 / 1.5f, 2 / 1.5f};
        dashed.SetDashPattern(dash, 2);
        g.DrawLine(&dashed, t.X - S(14), t.Y, t.X + S(14), t.Y);
        g.DrawLine(&dashed, t.X, t.Y - S(14), t.X, t.Y + S(14));
        g.DrawEllipse(&dashed, t.X - S(8), t.Y - S(8), S(16), S(16));
        Pen dotted(C(g_theme.target, 178), S(1));
        REAL dots[] = {2, 3};
        dotted.SetDashPattern(dots, 2);
        g.DrawLine(&dotted, c, t);
    }

    // Status plates: the live video, when the picture was taken, or that it is out of date.
    float ph = plate_height(g);
    std::wstring status = s.measuring ? L"MEASURING THE VIEW · THE CAMERA TURNS A LITTLE AND BACK"
                        : !s.live.empty() ? L"\u25cf LIVE · " + s.live
                          : s.picture_stale ? L"MOVING · NEW PICTURE WHEN IT STOPS"
                          : s.picture_taken.empty() ? L"PICTURE" : L"PICTURE " + s.picture_taken;
    plate(g, status, r.X + S(6), r.GetBottom() - S(6) - ph, s.picture_stale || s.measuring ? g_theme.warning : white);
    if (!s.can_aim) plate(g, L"THIS CAMERA CAN'T AIM FROM ITS PICTURE", r.GetRight() - S(6), r.GetBottom() - S(6) - ph, g_theme.warning, true);

    // Hover: guides and the point in view units.
    if (p.hover && s.can_aim && r.Contains(*p.hover)) {
        PointF hv = *p.hover;
        Pen guide(C(white, 89), S(0.5f));
        g.DrawLine(&guide, hv.X, r.Y, hv.X, r.GetBottom());
        g.DrawLine(&guide, r.X, hv.Y, r.GetRight(), hv.Y);
        PointF v = picture_point(r, hv.X, hv.Y);
        wchar_t buffer[64];
        swprintf(buffer, 64, L"%s %.2f  %s %.2f", v.X < 0 ? L"◀" : L"▶", std::fabs(v.X), v.Y < 0 ? L"▼" : L"▲", std::fabs(v.Y));
        std::wstring text = buffer;
        if (const auto &o = s.overlay)
            text = L"PAN " + label_value(o->pan + v.X * o->scale.pan_per_x, std::fabs(o->scale.pan_per_x) * 0.02) + L"  TILT " +
                   label_value(o->tilt + v.Y * o->scale.tilt_per_y, std::fabs(o->scale.tilt_per_y) * 0.02);
        float width = measure(g, text, 10, true).Width + S(18);
        float x = hv.X + S(10), y = hv.Y - S(8) - ph;
        if (x + width > r.GetRight()) x = hv.X - width - S(10);
        if (y < r.Y) y = hv.Y + S(8);
        plate(g, text, x, y, white);
    }
}

void plane_aim(HWND hwnd, Plane &p, int x, int y, bool commit) {
    const PlaneState &s = p.state;
    RECT rc;
    GetClientRect(hwnd, &rc);
    RectF r = picture_rect(s, static_cast<float>(rc.right), static_cast<float>(rc.bottom));
    if (!s.enabled || !s.picture || !s.can_aim || s.measuring || !r.Contains(static_cast<float>(x), static_cast<float>(y))) return;
    PointF point = picture_point(r, static_cast<float>(x), static_cast<float>(y));
    if (const auto &o = s.overlay) {
        double pan = o->pan + point.X * o->scale.pan_per_x, tilt = o->tilt + point.Y * o->scale.tilt_per_y;
        if (commit) {
            if (s.on_aim_and_move) s.on_aim_and_move(pan, tilt);
        } else if (s.on_aim) {
            s.on_aim(pan, tilt);
        }
        return;
    }
    if (commit) {
        if (s.on_picture_aim_and_move) s.on_picture_aim_and_move(point.X, point.Y);
    } else if (s.on_picture_aim) {
        s.on_picture_aim(point.X, point.Y);
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
            p->hover = r.Contains(static_cast<float>(x), static_cast<float>(y)) ? std::optional<PointF>(PointF(
                                                                                    static_cast<float>(x), static_cast<float>(y)))
                                                                              : std::nullopt;
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

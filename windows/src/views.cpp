// Position plane and travel meters, drawn with GDI+ (anti-aliased, part of Windows).
#include "app.h"

#include <windowsx.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
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

// ---- position plane ------------------------------------------------------------

struct Plane {
    PlaneState state;
    std::vector<std::optional<PointF>> trail;  // camera units; nullopt marks a gap
    std::optional<PointF> hover;               // client pixels
    bool dragging = false;
};

RectF plot_rect(float w, float h) { return RectF(S(46), S(10), std::max(1.0f, w - S(58)), std::max(1.0f, h - S(34))); }

std::optional<PointF> to_view(const PlaneState &s, const RectF &r, double pan, double tilt) {
    auto fx = s.pan.fraction(pan), fy = s.tilt.fraction(tilt);
    if (!fx || !fy) return std::nullopt;
    return PointF(r.X + r.Width * static_cast<float>(*fx), r.GetBottom() - r.Height * static_cast<float>(*fy));
}

void paint_plane(Plane &p, Graphics &g, float w, float h) {
    const PlaneState &s = p.state;
    RectF r = plot_rect(w, h);
    Pen grid(C(g_theme.grid), 1);
    for (int i = 0; i <= 10; ++i) {
        float x = r.X + r.Width * i / 10, y = r.Y + r.Height * i / 10;
        g.DrawLine(&grid, x, r.Y, x, r.GetBottom());
        g.DrawLine(&grid, r.X, y, r.GetRight(), y);
    }
    Pen frame(C(g_theme.border), 1);
    g.DrawRectangle(&frame, r);

    if (!s.pan.available || !s.tilt.available) {
        std::wstring text = s.enabled ? L"PAN/TILT RANGE UNAVAILABLE" : L"NO CAMERA LINK";
        SizeF size = measure(g, text, 12, true);
        draw_text(g, text, r.X + (r.Width - size.Width) / 2, r.Y + (r.Height - size.Height) / 2, 12, g_theme.secondary, true);
        return;
    }

    Pen axis(C(g_theme.axis), 1);
    if (auto zero = to_view(s, r, 0, s.tilt.min)) g.DrawLine(&axis, zero->X, r.Y, zero->X, r.GetBottom());
    if (auto zero = to_view(s, r, s.pan.min, 0)) g.DrawLine(&axis, r.X, zero->Y, r.GetRight(), zero->Y);

    float label = 9;
    draw_text(g, format_value(s.pan.min), r.X, r.GetBottom() + S(4), label, g_theme.secondary);
    draw_text(g, format_value(s.pan.max), r.GetRight(), r.GetBottom() + S(4), label, g_theme.secondary, false, true);
    draw_text(g, L"PAN", r.X + r.Width / 2 - S(9), r.GetBottom() + S(4), label, g_theme.secondary);
    draw_text(g, format_value(s.tilt.max), r.X - S(6), r.Y - S(2), label, g_theme.secondary, false, true);
    draw_text(g, format_value(s.tilt.min), r.X - S(6), r.GetBottom() - S(12), label, g_theme.secondary, false, true);
    draw_text(g, L"TILT", r.X - S(6), r.Y + r.Height / 2 - S(6), label, g_theme.secondary, false, true);

    // Measured path; gaps stay gaps.
    Pen trail(C(g_theme.primary, 90), S(1.5f));
    std::optional<PointF> last;
    for (const auto &sample : p.trail) {
        std::optional<PointF> point = sample ? to_view(s, r, sample->X, sample->Y) : std::nullopt;
        if (point && last) g.DrawLine(&trail, *last, *point);
        last = point;
    }

    if (s.starting && s.starting->pan && s.starting->tilt) {
        if (auto pt = to_view(s, r, s.starting->pan->value, s.starting->tilt->value)) {
            Pen start(C(g_theme.start), S(1.5f));
            g.DrawRectangle(&start, pt->X - S(5), pt->Y - S(5), S(10), S(10));
            draw_text(g, L"S", pt->X + S(7), pt->Y - S(16), label, g_theme.secondary);
        }
    }

    if (s.target_pan || s.target_tilt) {
        std::optional<double> pan = s.target_pan, tilt = s.target_tilt;
        if (!pan && s.current && s.current->pan) pan = s.current->pan->value;
        if (!tilt && s.current && s.current->tilt) tilt = s.current->tilt->value;
        if (pan && tilt) {
            if (auto pt = to_view(s, r, *pan, *tilt)) {
                Pen target(C(g_theme.target), S(1));
                target.SetDashStyle(DashStyleDash);
                g.DrawLine(&target, pt->X - S(12), pt->Y, pt->X + S(12), pt->Y);
                g.DrawLine(&target, pt->X, pt->Y - S(12), pt->X, pt->Y + S(12));
                g.DrawEllipse(&target, pt->X - S(7), pt->Y - S(7), S(14), S(14));
            }
        }
    }

    if (s.current && s.current->pan && s.current->tilt) {
        if (auto pt = to_view(s, r, s.current->pan->value, s.current->tilt->value)) {
            SolidBrush dot(C(g_theme.primary));
            g.FillEllipse(&dot, pt->X - S(4.5f), pt->Y - S(4.5f), S(9), S(9));
            Pen ring(C(g_theme.primary), S(1.5f));
            g.DrawEllipse(&ring, pt->X - S(10), pt->Y - S(10), S(20), S(20));
        }
    } else if (s.current) {
        draw_text(g, L"POSITION UNAVAILABLE", r.X + S(6), r.Y + S(4), label, g_theme.secondary);
    }

    if (p.hover && s.enabled) {
        PointF h = *p.hover;
        Pen guide(C(g_theme.axis), 1);
        g.DrawLine(&guide, h.X, r.Y, h.X, r.GetBottom());
        g.DrawLine(&guide, r.X, h.Y, r.GetRight(), h.Y);
        double pan = s.pan.value_at((h.X - r.X) / r.Width), tilt = s.tilt.value_at((r.GetBottom() - h.Y) / r.Height);
        std::wstring text = format_value(pan) + L", " + format_value(tilt);
        SizeF size = measure(g, text, 10);
        float x = h.X + S(8), y = h.Y - size.Height - S(4);
        if (x + size.Width > r.GetRight()) x = h.X - size.Width - S(8);
        if (y < r.Y) y = h.Y + S(6);
        draw_text(g, text, x, y, 10, g_theme.text);
    }
}

void plane_aim(HWND hwnd, Plane &p, int x, int y, bool commit) {
    if (!p.state.enabled || !p.state.pan.available || !p.state.tilt.available) return;
    RECT rc;
    GetClientRect(hwnd, &rc);
    RectF r = plot_rect(static_cast<float>(rc.right), static_cast<float>(rc.bottom));
    float fx = std::clamp((x - r.X) / r.Width, 0.0f, 1.0f), fy = std::clamp((r.GetBottom() - y) / r.Height, 0.0f, 1.0f);
    double pan = p.state.pan.value_at(fx), tilt = p.state.tilt.value_at(fy);
    if (commit) {
        if (p.state.on_aim_and_move) p.state.on_aim_and_move(pan, tilt);
    } else if (p.state.on_aim) {
        p.state.on_aim(pan, tilt);
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
            RectF r = plot_rect(static_cast<float>(rc.right), static_cast<float>(rc.bottom));
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

void plane_reset_trail(HWND plane) { reinterpret_cast<Plane *>(GetWindowLongPtrW(plane, GWLP_USERDATA))->trail.clear(); }

void plane_record(HWND plane) {
    auto *p = reinterpret_cast<Plane *>(GetWindowLongPtrW(plane, GWLP_USERDATA));
    const auto &current = p->state.current;
    if (!current || !current->pan || !current->tilt) {
        if (!p->trail.empty() && p->trail.back()) p->trail.push_back(std::nullopt);
        return;
    }
    PointF point(static_cast<float>(current->pan->value), static_cast<float>(current->tilt->value));
    if (!p->trail.empty() && p->trail.back() && p->trail.back()->Equals(point)) return;
    p->trail.push_back(point);
    if (p->trail.size() > 120) p->trail.erase(p->trail.begin(), p->trail.end() - 120);
}

MeterState &meter_state(HWND meter) { return reinterpret_cast<Meter *>(GetWindowLongPtrW(meter, GWLP_USERDATA))->state; }

void instruments_set_theme(const Theme &theme, UINT dpi) {
    g_theme = theme;
    g_dpi = dpi;
}

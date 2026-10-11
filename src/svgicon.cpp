// 2026-10-10 - AI - SVG path 解析器 + Lucide 图标表
//
// ┌───────────────────────────────────────────────────────────────────────┐
// │ 图标数据来自 Lucide —— https://lucide.dev —— ISC 许可                 │
// │                                                                       │
// │   ISC License                                                         │
// │   Copyright (c) for portions of Lucide are held by Cole Bemis 2013-2017│
// │   as part of Feather Icons. All other copyright (c) for Lucide are    │
// │   held by Lucide Contributors 2020.                                   │
// │                                                                       │
// │   Permission to use, copy, modify, and/or distribute this software    │
// │   for any purpose with or without fee is hereby granted, provided     │
// │   that the above copyright notice and this permission notice appear    │
// │   in all copies.                                                      │
// │                                                                       │
// │ 下载的源文件：lucide-static（图标名 = 仓库里的文件名）                 │
// │ 没用 SF Symbols：那是 Apple 的版权资产，许可不允许这样跨平台使用。    │
// └───────────────────────────────────────────────────────────────────────┘

#include "svgicon.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace mydock {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// ---------------------------------------------------------------- 数字扫描

// SVG 的数字写得很随意："M12 20h.01"、"m11 7-3 5h4l-3 5"、
// "M2 8.82a15 15 0 0 1 20 0"。负号可以直接当分隔符用（7-3 是两个数）。
struct NumScanner {
    const char* p = nullptr;
    explicit NumScanner(const char* s) : p(s) {}

    void SkipSep() {
        while (*p && (std::isspace(static_cast<unsigned char>(*p)) || *p == ',')) ++p;
    }
    bool AtNum() const {
        const char c = *p;
        return c != '\0' && ((c >= '0' && c <= '9') || c == '.' || c == '+' || c == '-');
    }
    bool Next(float& v) {
        SkipSep();
        if (!AtNum()) return false;
        char* end = nullptr;
        v = std::strtof(p, &end);
        if (end == p) return false;
        p = end;
        return true;
    }
    bool NextFlag(int& v) {
        SkipSep();
        if (*p == '0' || *p == '1') {
            v = *p - '0';
            ++p;
            return true;
        }
        return false;   // 有些写法会把 flag 连着写（"011"），先不处理
    }
};

// ------------------------------------------------------------ arc → 贝塞尔

// SVG 的 A/a 是「端点参数化」（给起点终点和半径，圆心要自己解），
// GDI+ 的 AddArc 是「圆心参数化」。而且 GDI+ 的 AddArc 没法处理椭圆旋转，
// 所以统一走 SVG 规范 F.6.5 那套：解出圆心 → 按每 90° 一段切成三次贝塞尔。
void ArcToCubic(SvgSub& sub, float& cx, float& cy,
                float rx, float ry, float phiDeg,
                bool largeArc, bool sweep, float x2, float y2) {
    if (rx == 0.0f || ry == 0.0f) {                      // 退化成直线
        SvgSeg s;
        s.op = SvgSeg::kLine;
        s.x = x2;
        s.y = y2;
        sub.segs.push_back(s);
        cx = x2;
        cy = y2;
        return;
    }

    const float x1 = cx, y1 = cy;
    const float phi = phiDeg * kPi / 180.0f;
    const float cosp = std::cos(phi), sinp = std::sin(phi);

    const float dx2 = (x1 - x2) * 0.5f;
    const float dy2 = (y1 - y2) * 0.5f;
    const float x1p = cosp * dx2 + sinp * dy2;
    const float y1p = -sinp * dx2 + cosp * dy2;

    rx = std::fabs(rx);
    ry = std::fabs(ry);

    // 半径不够撑起这条弦就按比例放大（规范 F.6.6 的第 2 步）
    const float lambda = (x1p * x1p) / (rx * rx) + (y1p * y1p) / (ry * ry);
    if (lambda > 1.0f) {
        const float sq = std::sqrt(lambda);
        rx *= sq;
        ry *= sq;
    }

    // 圆心（规范 F.6.5 的第 3 步）
    float num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p;
    float den = rx * rx * y1p * y1p + ry * ry * x1p * x1p;
    if (num < 0.0f) num = 0.0f;
    if (den < 1e-9f) den = 1e-9f;
    const float coef = ((largeArc != sweep) ? 1.0f : -1.0f) * std::sqrt(num / den);
    const float cxp = coef * (rx * y1p / ry);
    const float cyp = coef * (-ry * x1p / rx);
    const float ccx = cosp * cxp - sinp * cyp + (x1 + x2) * 0.5f;
    const float ccy = sinp * cxp + cosp * cyp + (y1 + y2) * 0.5f;

    // 起止角
    auto angleBetween = [](float ux, float uy, float vx, float vy) {
        const float n = std::sqrt((ux * ux + uy * uy) * (vx * vx + vy * vy));
        if (n < 1e-9f) return 0.0f;
        float c = (ux * vx + uy * vy) / n;
        if (c > 1.0f) c = 1.0f;
        if (c < -1.0f) c = -1.0f;
        float a = std::acos(c);
        if (ux * vy - uy * vx < 0.0f) a = -a;
        return a;
    };
    const float ux = (x1p - cxp) / rx, uy = (y1p - cyp) / ry;
    const float vx = (-x1p - cxp) / rx, vy = (-y1p - cyp) / ry;
    float th1 = angleBetween(1.0f, 0.0f, ux, uy);
    float dth = angleBetween(ux, uy, vx, vy);
    if (!sweep && dth > 0.0f) dth -= 2.0f * kPi;
    else if (sweep && dth < 0.0f) dth += 2.0f * kPi;

    int n = static_cast<int>(std::ceil(std::fabs(dth) / (kPi * 0.5f)));
    if (n < 1) n = 1;
    const float step = dth / static_cast<float>(n);
    // 每段的贝塞尔逼近系数（规范 F.6.5.2）
    const float alpha = (4.0f / 3.0f) * std::tan(step * 0.25f);

    // 椭圆上的点 / 它的导数
    auto pt = [&](float t, float& ex, float& ey) {
        const float ct = std::cos(t), st = std::sin(t);
        ex = ccx + rx * ct * cosp - ry * st * sinp;
        ey = ccy + rx * ct * sinp + ry * st * cosp;
    };
    auto dpt = [&](float t, float& ex, float& ey) {
        const float ct = std::cos(t), st = std::sin(t);
        ex = -rx * st * cosp - ry * ct * sinp;
        ey = -rx * st * sinp + ry * ct * cosp;
    };

    for (int i = 0; i < n; ++i) {
        const float t1 = th1 + step * static_cast<float>(i);
        const float t2 = t1 + step;
        float ax, ay, adx, ady, bx, by, bdx, bdy;
        pt(t1, ax, ay);
        dpt(t1, adx, ady);
        pt(t2, bx, by);
        dpt(t2, bdx, bdy);
        SvgSeg s;
        s.op  = SvgSeg::kCubic;
        s.c1x = ax + alpha * adx;
        s.c1y = ay + alpha * ady;
        s.c2x = bx - alpha * bdx;
        s.c2y = by - alpha * bdy;
        s.x   = bx;
        s.y   = by;
        sub.segs.push_back(s);
    }
    cx = x2;
    cy = y2;
}

// 二次贝塞尔 → 三次（GDI+ 只有三次）
void QuadToCubic(SvgSub& sub, float qx, float qy, float x, float y,
                 float px, float py) {
    SvgSeg s;
    s.op  = SvgSeg::kCubic;
    s.c1x = px + (2.0f / 3.0f) * (qx - px);
    s.c1y = py + (2.0f / 3.0f) * (qy - py);
    s.c2x = x + (2.0f / 3.0f) * (qx - x);
    s.c2y = y + (2.0f / 3.0f) * (qy - y);
    s.x   = x;
    s.y   = y;
    sub.segs.push_back(s);
}

void PushLine(SvgSub& sub, float x, float y) {
    SvgSeg s;
    s.op = SvgSeg::kLine;
    s.x = x;
    s.y = y;
    sub.segs.push_back(s);
}

void PushMove(SvgSub& sub, float x, float y) {
    SvgSeg s;
    s.op = SvgSeg::kMove;
    s.x = x;
    s.y = y;
    sub.segs.push_back(s);
}

}  // namespace

// ------------------------------------------------------------------ 解析

bool SvgParsePath(const char* d, SvgIcon& out) {
    if (!d) return false;
    NumScanner sc(d);

    float cx = 0.0f, cy = 0.0f;            // 当前点
    float startX = 0.0f, startY = 0.0f;    // 子路径起点（Z 要回到这儿）
    float lastC2x = 0.0f, lastC2y = 0.0f;  // 上一条 C 的第二控制点（S 要镜像）
    float lastQx = 0.0f, lastQy = 0.0f;    // 上一条 Q 的控制点（T 要镜像）
    char  lastCmd = 0;

    SvgSub cur;
    auto flush = [&]() {
        if (!cur.segs.empty()) {
            out.subs.push_back(cur);
            cur = SvgSub();
        }
    };

    while (true) {
        sc.SkipSep();
        const char c = *sc.p;
        if (c == '\0') break;

        char cmd = c;
        const bool isLetter = ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'));
        if (isLetter) {
            lastCmd = c;
            ++sc.p;
        } else {
            // 命令字母后面可以跟多组参数（"M0 0 1 1 2 2" = M + L + L）
            if (lastCmd == 0) return false;
            if (lastCmd == 'M') lastCmd = 'L';
            else if (lastCmd == 'm') lastCmd = 'l';
            else if (lastCmd == 'Z' || lastCmd == 'z') break;   // Z 后面不该再有数
            cmd = lastCmd;
        }

        const bool rel = (cmd >= 'a' && cmd <= 'z');
        const char up = rel ? static_cast<char>(cmd - 32) : cmd;

        if (up == 'Z') {
            if (!cur.segs.empty()) {
                cur.closed = true;
                flush();
            }
            cx = startX;
            cy = startY;
            continue;
        }

        if (up == 'M' || up == 'L' || up == 'T') {
            float x, y;
            if (!sc.Next(x) || !sc.Next(y)) break;
            const float ax = rel ? cx + x : x;
            const float ay = rel ? cy + y : y;
            if (up == 'M') {
                flush();
                startX = ax;
                startY = ay;
                PushMove(cur, ax, ay);
            } else if (up == 'L') {
                PushLine(cur, ax, ay);
            } else {                                    // T：控制点 = 上一条 Q 的镜像
                const float qx = cx + (cx - lastQx);
                const float qy = cy + (cy - lastQy);
                QuadToCubic(cur, qx, qy, ax, ay, cx, cy);
                lastQx = qx;
                lastQy = qy;
            }
            cx = ax;
            cy = ay;
        } else if (up == 'H' || up == 'V') {
            float v;
            if (!sc.Next(v)) break;
            if (up == 'H') {
                cx = rel ? cx + v : v;
            } else {
                cy = rel ? cy + v : v;
            }
            PushLine(cur, cx, cy);
        } else if (up == 'C') {
            float x1, y1, x2, y2, x, y;
            if (!sc.Next(x1) || !sc.Next(y1) || !sc.Next(x2) || !sc.Next(y2) ||
                !sc.Next(x) || !sc.Next(y)) break;
            const float a1x = rel ? cx + x1 : x1, a1y = rel ? cy + y1 : y1;
            const float a2x = rel ? cx + x2 : x2, a2y = rel ? cy + y2 : y2;
            const float ax  = rel ? cx + x  : x,  ay  = rel ? cy + y  : y;
            SvgSeg s;
            s.op  = SvgSeg::kCubic;
            s.c1x = a1x; s.c1y = a1y;
            s.c2x = a2x; s.c2y = a2y;
            s.x = ax;    s.y = ay;
            cur.segs.push_back(s);
            lastC2x = a2x;
            lastC2y = a2y;
            cx = ax;
            cy = ay;
        } else if (up == 'S') {
            float x2, y2, x, y;
            if (!sc.Next(x2) || !sc.Next(y2) || !sc.Next(x) || !sc.Next(y)) break;
            const float a2x = rel ? cx + x2 : x2, a2y = rel ? cy + y2 : y2;
            const float ax  = rel ? cx + x  : x,  ay  = rel ? cy + y  : y;
            SvgSeg s;
            s.op  = SvgSeg::kCubic;
            s.c1x = cx + (cx - lastC2x);            // 第一条控制点 = 上一条的镜像
            s.c1y = cy + (cy - lastC2y);
            s.c2x = a2x; s.c2y = a2y;
            s.x = ax;    s.y = ay;
            cur.segs.push_back(s);
            lastC2x = a2x;
            lastC2y = a2y;
            cx = ax;
            cy = ay;
        } else if (up == 'Q') {
            float x1, y1, x, y;
            if (!sc.Next(x1) || !sc.Next(y1) || !sc.Next(x) || !sc.Next(y)) break;
            const float qx = rel ? cx + x1 : x1, qy = rel ? cy + y1 : y1;
            const float ax = rel ? cx + x  : x,  ay = rel ? cy + y  : y;
            QuadToCubic(cur, qx, qy, ax, ay, cx, cy);
            lastQx = qx;
            lastQy = qy;
            cx = ax;
            cy = ay;
        } else if (up == 'A') {
            float rx, ry, rot, x, y;
            int fa, fs;
            if (!sc.Next(rx) || !sc.Next(ry) || !sc.Next(rot) ||
                !sc.NextFlag(fa) || !sc.NextFlag(fs) || !sc.Next(x) || !sc.Next(y)) break;
            const float ax = rel ? cx + x : x;
            const float ay = rel ? cy + y : y;
            ArcToCubic(cur, cx, cy, rx, ry, rot, fa != 0, fs != 0, ax, ay);
        } else {
            break;      // 不认识的命令，就此打住
        }
    }

    flush();
    return !out.subs.empty();
}

// ------------------------------------------------------- 基本形状归一成 path

void SvgAddRect(SvgIcon& out, float x, float y, float w, float h, float rx) {
    if (w <= 0.0f || h <= 0.0f) return;
    if (rx < 0.0f) rx = 0.0f;
    const float r = (rx * 2.0f > w || rx * 2.0f > h) ? std::min(w, h) * 0.5f : rx;

    SvgSub s;
    s.closed = true;
    // 当前点（ArcToCubic 会边画边更新它）；两个分支一进来都会先赋值
    float cx, cy;

    if (r <= 0.0f) {
        PushMove(s, x, y);
        cx = x;
        cy = y;
        PushLine(s, x + w, y);
        PushLine(s, x + w, y + h);
        PushLine(s, x, y + h);
    } else {
        // 跟 SVG 的 <rect rx> 一个走法：直线 + 四分之一的 A 圆弧
        PushMove(s, x + r, y);
        cx = x + r;
        cy = y;
        PushLine(s, x + w - r, y);      cx = x + w - r; cy = y;
        ArcToCubic(s, cx, cy, r, r, 0.0f, false, true, x + w, y + r);
        PushLine(s, x + w, y + h - r);  cx = x + w; cy = y + h - r;
        ArcToCubic(s, cx, cy, r, r, 0.0f, false, true, x + w - r, y + h);
        PushLine(s, x + r, y + h);      cx = x + r; cy = y + h;
        ArcToCubic(s, cx, cy, r, r, 0.0f, false, true, x, y + h - r);
        PushLine(s, x, y + r);          cx = x; cy = y + r;
        ArcToCubic(s, cx, cy, r, r, 0.0f, false, true, x + r, y);
    }
    out.subs.push_back(s);
}

void SvgAddCircle(SvgIcon& out, float cx, float cy, float r) {
    if (r <= 0.0f) return;
    SvgSub s;
    s.closed = true;
    PushMove(s, cx - r, cy);
    float x = cx - r, y = cy;
    ArcToCubic(s, x, y, r, r, 0.0f, false, false, cx + r, cy);
    ArcToCubic(s, x, y, r, r, 0.0f, false, false, cx - r, cy);
    out.subs.push_back(s);
}

void SvgAddLine(SvgIcon& out, float x1, float y1, float x2, float y2) {
    SvgSub s;
    PushMove(s, x1, y1);
    PushLine(s, x2, y2);
    out.subs.push_back(s);
}

// -------------------------------------------------------------------- 绘制

void SvgMap(float cx, float cy, float box, float sx, float sy,
            float x24, float y24, float& outX, float& outY) {
    const float k = box / 24.0f;
    const float w = box * sx, h = box * sy;
    outX = (cx - w * 0.5f) + x24 * k * sx;
    outY = (cy - h * 0.5f) + y24 * k * sy;
}

void SvgDraw(Gdiplus::Graphics& g, const SvgIcon& ic,
             float cx, float cy, float box, float strokeIn24,
             Gdiplus::Color color, float sx, float sy) {
    if (box <= 0.0f) return;
    const float k = box / 24.0f;
    const float w = box * sx, h = box * sy;
    const float ox = cx - w * 0.5f;
    const float oy = cy - h * 0.5f;
    auto TX = [&](float x) { return ox + x * k * sx; };
    auto TY = [&](float y) { return oy + y * k * sy; };

    Gdiplus::Pen pen(color, strokeIn24 * k);
    pen.SetLineCap(Gdiplus::LineCapRound, Gdiplus::LineCapRound, Gdiplus::DashCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::SolidBrush brush(color);

    for (const auto& sub : ic.subs) {
        if (sub.segs.empty()) continue;
        Gdiplus::GraphicsPath gp;
        bool has = false;       // 路径里已经有内容了？
        bool pending = false;   // 有一个 Move 等着接第一条线
        float lx = 0.0f, ly = 0.0f;
        float px = 0.0f, py = 0.0f;

        for (const auto& s : sub.segs) {
            const float X = TX(s.x), Y = TY(s.y);
            if (s.op == SvgSeg::kMove) {
                if (has) gp.StartFigure();
                pending = true;
                px = X;
                py = Y;
            } else if (s.op == SvgSeg::kLine) {
                if (pending) {
                    gp.AddLine(px, py, X, Y);
                    pending = false;
                } else if (has) {
                    gp.AddLine(lx, ly, X, Y);
                } else {
                    gp.AddLine(X, Y, X, Y);          // 孤立的一条线（不该发生）
                }
                has = true;
            } else {
                const float c1x = TX(s.c1x), c1y = TY(s.c1y);
                const float c2x = TX(s.c2x), c2y = TY(s.c2y);
                if (pending) {
                    gp.AddBezier(px, py, c1x, c1y, c2x, c2y, X, Y);
                    pending = false;
                } else if (has) {
                    gp.AddBezier(lx, ly, c1x, c1y, c2x, c2y, X, Y);
                } else {
                    gp.AddBezier(X, Y, c1x, c1y, c2x, c2y, X, Y);
                }
                has = true;
            }
            lx = X;
            ly = Y;
        }

        if (!has) continue;
        if (sub.closed) gp.CloseFigure();
        // 填实的就不描边了，否则会胖一圈
        if (sub.fill) g.FillPath(&brush, &gp);
        else          g.DrawPath(&pen, &gp);
    }
}

// ---------------------------------------------------------------- 图标表

bool LucideIcon(const char* name, SvgIcon& out) {
    out = SvgIcon();

    auto P = [&](const char* d) { SvgParsePath(d, out); };
    auto PF = [&](const char* d) {                 // 填实的
        const size_t before = out.subs.size();
        SvgParsePath(d, out);
        for (size_t i = before; i < out.subs.size(); ++i) out.subs[i].fill = true;
    };
    auto R  = [&](float x, float y, float w, float h, float r) { SvgAddRect(out, x, y, w, h, r); };
    auto RF = [&](float x, float y, float w, float h, float r) {
        const size_t before = out.subs.size();
        SvgAddRect(out, x, y, w, h, r);
        for (size_t i = before; i < out.subs.size(); ++i) out.subs[i].fill = true;
    };
    auto C = [&](float x, float y, float r) { SvgAddCircle(out, x, y, r); };

    // ---- 蓝牙 ----
    if (!std::strcmp(name, "bluetooth")) {
        P("m7 7 10 10-5 5V2l5 5L7 17");
        return true;
    }

    // ---- Wi-Fi：四档，按真实信号强度选 ----
    if (!std::strcmp(name, "wifi")) {
        P("M12 20h.01");
        P("M2 8.82a15 15 0 0 1 20 0");
        P("M5 12.859a10 10 0 0 1 14 0");
        P("M8.5 16.429a5 5 0 0 1 7 0");
        return true;
    }
    if (!std::strcmp(name, "wifi-high")) {
        P("M12 20h.01");
        P("M5 12.859a10 10 0 0 1 14 0");
        P("M8.5 16.429a5 5 0 0 1 7 0");
        return true;
    }
    if (!std::strcmp(name, "wifi-low")) {
        P("M12 20h.01");
        P("M8.5 16.429a5 5 0 0 1 7 0");
        return true;
    }
    if (!std::strcmp(name, "wifi-zero")) {
        P("M12 20h.01");
        return true;
    }

    // ---- 显示器 ----
    if (!std::strcmp(name, "monitor")) {
        R(2, 3, 20, 14, 2);
        P("M8 21h8");
        P("M12 17v4");
        return true;
    }

    // ---- 电池 ----
    // Lucide 原版是 rect(2,6,16,12)，正极画在 x=22 的一条竖线 —— 正极离框
    // 有 4 格远，散开了。这里把「框 + 正极」当成一个整体重新在 24 格里居中：
    //   框 2.7..18.7，正极 19.3..21.5，整体宽 18.8、高 12 → 1.57:1
    // 用户要「长一点才有参考性」，所以绘制时再靠 sx 横向拉到约 2:1
    // （见 topbar.cpp 里的 kBatterySx）。
    if (!std::strcmp(name, "battery")) {
        R(2.7f, 6.0f, 16.0f, 12.0f, 2.0f);       // 外框
        RF(19.3f, 9.6f, 2.2f, 4.8f, 1.0f);       // 正极（实心小凸起）
        return true;
    }
    // 充电闪电。
    // ★ 不用 battery-charging 里那条折线 —— 那是 4 个点的开放路径，
    //   靠 stroke 描出来，在电池里细得像根铁丝。改用 Lucide 的 zap：
    //   它本身就是一块**闭合的实心闪电**（路径以 z 收尾），填出来饱满，
    //   跟 macOS 电池里那个闪电是一个感觉。它在 24 格里以 (12,12) 居中。
    if (!std::strcmp(name, "bolt")) {
        PF("M15.914 4a1.5 1.5 0 00-2.474-1.561l-9 9A1.5 1.5 0 005.5 14h4.002a.5.5 0 01.471.666"
           "L8.086 20a1.5 1.5 0 002.475 1.56l9-9A1.5 1.5 0 0018.5 10h-3.997a.5.5 0 01-.472-.667z");
        return true;
    }
    // 电量条的内区（上色时用 clip 裁出百分比，见 topbar.cpp）
    if (!std::strcmp(name, "battery-level")) {
        RF(4.8f, 8.2f, 12.4f, 7.6f, 1.2f);
        return true;
    }

    // ---- 音量 ----
    // 喇叭是闭合路径，填实（macOS 的喇叭也是实心的，描边版太轻）
    if (!std::strcmp(name, "volume")) {
        PF("M11 4.702a.705.705 0 0 0-1.203-.498L6.413 7.587A1.4 1.4 0 0 1 5.416 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.416a1.4 1.4 0 0 1 .997.413l3.383 3.384A.705.705 0 0 0 11 19.298z");
        return true;
    }
    if (!std::strcmp(name, "volume-1")) {
        PF("M11 4.702a.705.705 0 0 0-1.203-.498L6.413 7.587A1.4 1.4 0 0 1 5.416 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.416a1.4 1.4 0 0 1 .997.413l3.383 3.384A.705.705 0 0 0 11 19.298z");
        P("M16 9a5 5 0 0 1 0 6");
        return true;
    }
    if (!std::strcmp(name, "volume-2")) {
        PF("M11 4.702a.705.705 0 0 0-1.203-.498L6.413 7.587A1.4 1.4 0 0 1 5.416 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.416a1.4 1.4 0 0 1 .997.413l3.383 3.384A.705.705 0 0 0 11 19.298z");
        P("M16 9a5 5 0 0 1 0 6");
        P("M19.364 18.364a9 9 0 0 0 0-12.728");
        return true;
    }
    if (!std::strcmp(name, "volume-x")) {
        PF("M11 4.702a.7.7 0 0 0-1.203-.498L6.413 7.587A1.4 1.4 0 0 1 5.416 8H3a1 1 0 0 0-1 1v6a1 1 0 0 0 1 1h2.416a1.4 1.4 0 0 1 .997.413l3.383 3.384A.7.7 0 0 0 11 19.298z");
        P("m16.5 14.5 5-5");
        P("m16.5 9.5 5 5");
        return true;
    }

    // ---- 弹出 ----
    if (!std::strcmp(name, "eject")) {
        PF("M4 13a1 1 0 0 1-.72-1.695l7.257-7.668a2 2 0 0 1 2.926 0l7.256 7.668A1 1 0 0 1 20 13z");
        RF(3, 17, 18, 4, 1);
        return true;
    }

    // ---- 搜索 ----
    if (!std::strcmp(name, "search")) {
        C(11, 11, 8);
        P("m21 21-4.34-4.34");
        return true;
    }

    // ---- 控制中心（滑块）----
    if (!std::strcmp(name, "sliders-horizontal")) {
        P("M10 5H3");
        P("M12 19H3");
        P("M14 3v4");
        P("M16 17v4");
        P("M21 12h-9");
        P("M21 19h-5");
        P("M21 5h-7");
        P("M8 10v4");
        P("M8 12H3");
        return true;
    }

    return false;
}

}  // namespace mydock

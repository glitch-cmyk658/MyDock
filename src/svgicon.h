// 2026-10-10 - AI - SVG 图标绘制：解析 Lucide(ISC) 的矢量数据，用 GDI+ 画到顶部状态栏上
//
// 之前那排状态图标是手写的 GDI+ 调用，改一个比例要改一堆数字，而且画不精致。
// 现在改成：图标的矢量数据从 Lucide 拿（ISC 许可，见 svgicon.cpp 顶部的声明），
// 这里只负责把 SVG path 的 d 字符串翻译成 GDI+ 能吃的贝塞尔。
//
// Lucide 的规格统一：viewBox 0 0 24 24、fill=none、stroke-width=2、round cap/join。
// 正好和我们的画法（Pen + round cap）是同一套，所以接起来很干净。

#pragma once

#include <vector>

#include <windows.h>
#include <gdiplus.h>

namespace mydock {

// path 解析后的最小图元。
// GDI+ 的 GraphicsPath 不能拷贝（AddLine 之后就攒在对象里），
// 所以先存成自己的结构，画的时候再一条条喂进去。
struct SvgSeg {
    enum Op { kMove = 0, kLine = 1, kCubic = 2 };
    int   op = kMove;
    float x = 0.0f, y = 0.0f;              // 终点（三种 op 都用）
    float c1x = 0.0f, c1y = 0.0f;          // 三次贝塞尔控制点 1
    float c2x = 0.0f, c2y = 0.0f;          // 三次贝塞尔控制点 2
};

// 一条子路径 ≈ SVG 里的一个 <path>（或 <rect> / <circle> / <line> 归一出来的）
struct SvgSub {
    std::vector<SvgSeg> segs;
    bool closed = false;
    bool fill   = false;   // 填实而不是描边（喇叭、闪电这类实体图形用）
};

struct SvgIcon {
    std::vector<SvgSub> subs;
};

// ---- 构造 ----
bool SvgParsePath(const char* d, SvgIcon& out);          // 解析一段 d 字符串
void SvgAddRect(SvgIcon& out, float x, float y, float w, float h, float rx);
void SvgAddCircle(SvgIcon& out, float cx, float cy, float r);
void SvgAddLine(SvgIcon& out, float x1, float y1, float x2, float y2);

// ---- 画 ----
//   box        : 24 格 viewBox 映射到多大（逻辑像素）
//   strokeIn24 : 线宽，同样按 24 格算。Lucide 默认 2；我们要粗一点给 2.6~3
//   sx, sy     : 额外的非均匀缩放（电池要横向拉长就靠 sx）
void SvgDraw(Gdiplus::Graphics& g, const SvgIcon& ic,
             float cx, float cy, float box, float strokeIn24,
             Gdiplus::Color color, float sx = 1.0f, float sy = 1.0f);

// 把 24 格里的一个点换算成屏幕坐标。
// 需要在图标内部另外补画东西时用它（比如电池里的电量条要跟着外框走）。
void SvgMap(float cx, float cy, float box, float sx, float sy,
            float x24, float y24, float& outX, float& outY);

// ---- 图标表 ----
// 数据来自 Lucide（ISC）。name 就是它在 Lucide 里的文件名，方便溯源。
// 少数几个是我们自己拼的（bolt / battery / battery-level），名字上加前缀区分。
bool LucideIcon(const char* name, SvgIcon& out);

}  // namespace mydock

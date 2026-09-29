#pragma once

#include "Common.h"

namespace Aperture::SvgGeometry {

inline bool ParseFloat(const char*& p, float& outVal) {
    while (*p && (*p == ' ' || *p == ',' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (!*p || (*p != '-' && *p != '+' && *p != '.' && (*p < '0' || *p > '9'))) return false;
    char* endP = nullptr;
    outVal = strtof(p, &endP);
    if (endP == p) return false;
    p = endP;
    return true;
}

inline ComPtr<ID2D1PathGeometry> Build(ID2D1Factory* factory, const char* d) {
    if (!factory || !d) return nullptr;
    ComPtr<ID2D1PathGeometry> path;
    if (FAILED(factory->CreatePathGeometry(&path))) return nullptr;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(path->Open(&sink))) return nullptr;
    const char* p = d;
    char cmd = 0;
    float curX = 0, curY = 0;
    float startX = 0, startY = 0;
    float lastCtrlX = 0, lastCtrlY = 0;
    bool figureOpen = false;
    auto EnsureFigure = [&](float x, float y) {
        if (!figureOpen) { sink->BeginFigure(D2D1::Point2F(x, y), D2D1_FIGURE_BEGIN_FILLED); figureOpen = true; startX = x; startY = y; }
    };
    while (*p) {
        while (*p && (*p == ' ' || *p == ',' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
        if (!*p) break;
        char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) { cmd = c; p++; }
        switch (cmd) {
            case 'M':
            case 'm': {
                float x = 0, y = 0;
                if (!ParseFloat(p, x) || !ParseFloat(p, y)) break;
                if (cmd == 'm') { x += curX; y += curY; }
                if (figureOpen) { sink->EndFigure(D2D1_FIGURE_END_OPEN); figureOpen = false; }
                curX = x; curY = y;
                EnsureFigure(curX, curY);
                lastCtrlX = curX; lastCtrlY = curY;
                cmd = (cmd == 'M') ? 'L' : 'l';
                break;
            }
            case 'L':
            case 'l': {
                float x = 0, y = 0;
                if (!ParseFloat(p, x) || !ParseFloat(p, y)) break;
                if (cmd == 'l') { x += curX; y += curY; }
                EnsureFigure(curX, curY);
                sink->AddLine(D2D1::Point2F(x, y));
                curX = x; curY = y;
                lastCtrlX = curX; lastCtrlY = curY;
                break;
            }
            case 'H':
            case 'h': {
                float x = 0;
                if (!ParseFloat(p, x)) break;
                if (cmd == 'h') x += curX;
                EnsureFigure(curX, curY);
                sink->AddLine(D2D1::Point2F(x, curY));
                curX = x;
                lastCtrlX = curX; lastCtrlY = curY;
                break;
            }
            case 'V':
            case 'v': {
                float y = 0;
                if (!ParseFloat(p, y)) break;
                if (cmd == 'v') y += curY;
                EnsureFigure(curX, curY);
                sink->AddLine(D2D1::Point2F(curX, y));
                curY = y;
                lastCtrlX = curX; lastCtrlY = curY;
                break;
            }
            case 'C':
            case 'c': {
                float x1 = 0, y1 = 0, x2 = 0, y2 = 0, x = 0, y = 0;
                if (!ParseFloat(p, x1) || !ParseFloat(p, y1) || !ParseFloat(p, x2) || !ParseFloat(p, y2) ||
                    !ParseFloat(p, x)  || !ParseFloat(p, y)) break;
                if (cmd == 'c') { x1 += curX; y1 += curY; x2 += curX; y2 += curY; x  += curX; y  += curY; }
                EnsureFigure(curX, curY);
                sink->AddBezier(D2D1::BezierSegment( D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), D2D1::Point2F(x, y) ));
                lastCtrlX = x2; lastCtrlY = y2;
                curX = x; curY = y;
                break;
            }
            case 'S':
            case 's': {
                float x2 = 0, y2 = 0, x = 0, y = 0;
                if (!ParseFloat(p, x2) || !ParseFloat(p, y2) || !ParseFloat(p, x)  || !ParseFloat(p, y)) break;
                if (cmd == 's') { x2 += curX; y2 += curY; x  += curX; y  += curY; }
                float x1 = 2 * curX - lastCtrlX, y1 = 2 * curY - lastCtrlY;
                EnsureFigure(curX, curY);
                sink->AddBezier(D2D1::BezierSegment( D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), D2D1::Point2F(x, y) ));
                lastCtrlX = x2; lastCtrlY = y2;
                curX = x; curY = y;
                break;
            }
            case 'Q':
            case 'q': {
                float x1 = 0, y1 = 0, x = 0, y = 0;
                if (!ParseFloat(p, x1) || !ParseFloat(p, y1) || !ParseFloat(p, x)  || !ParseFloat(p, y)) break;
                if (cmd == 'q') { x1 += curX; y1 += curY; x  += curX; y  += curY; }
                EnsureFigure(curX, curY);
                sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment( D2D1::Point2F(x1, y1), D2D1::Point2F(x, y) ));
                lastCtrlX = x1; lastCtrlY = y1;
                curX = x; curY = y;
                break;
            }
            case 'A':
            case 'a': {
                float rx = 0, ry = 0, angle = 0, largeArc = 0, sweep = 0, x = 0, y = 0;
                if (!ParseFloat(p, rx) || !ParseFloat(p, ry) || !ParseFloat(p, angle) || !ParseFloat(p, largeArc) ||
                    !ParseFloat(p, sweep) || !ParseFloat(p, x) || !ParseFloat(p, y)) break;
                if (cmd == 'a') { x += curX; y += curY; }
                EnsureFigure(curX, curY);
                sink->AddArc(D2D1::ArcSegment( D2D1::Point2F(x, y), D2D1::SizeF(fabsf(rx), fabsf(ry)), angle,
                    sweep > 0.5f ? D2D1_SWEEP_DIRECTION_CLOCKWISE : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE,
                    largeArc > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL ));
                curX = x; curY = y;
                lastCtrlX = curX; lastCtrlY = curY;
                break;
            }
            case 'Z':
            case 'z': {
                if (figureOpen) { sink->EndFigure(D2D1_FIGURE_END_CLOSED); figureOpen = false; }
                curX = startX; curY = startY;
                lastCtrlX = curX; lastCtrlY = curY;
                break;
            }
            default: p++; break;
        }
    }
    if (figureOpen) { sink->EndFigure(D2D1_FIGURE_END_OPEN); }
    sink->Close();
    return path;
}

} // namespace Aperture::SvgGeometry

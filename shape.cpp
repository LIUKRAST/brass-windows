#include "shape.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <sstream>

using namespace Brass;

std::vector<int> Brass::parseCorner(const std::string& str) {
    std::vector<int>  out;
    std::stringstream ss(str);
    std::string       tok;

    while (std::getline(ss, tok, ',')) {
        tok.erase(0, tok.find_first_not_of(" \t"));
        tok.erase(tok.find_last_not_of(" \t") + 1);
        if (tok.empty())
            continue;

        char*      end = nullptr;
        const long v   = std::strtol(tok.c_str(), &end, 10);
        if (*end != '\0' || v < 0 || v > 64)
            return {1};

        out.push_back(static_cast<int>(v));
    }

    // trailing zero rows carry no information
    while (!out.empty() && out.back() == 0)
        out.pop_back();

    return out;
}

int Brass::unitPx(int pixelSize, double scale) {
    return std::max(1, static_cast<int>(std::round(std::max(pixelSize, 1) * scale)));
}

static int cornerSpan(const std::vector<int>& corner, int thickness) {
    int maxCut = 0;
    for (const auto c : corner)
        maxCut = std::max(maxCut, c);

    return std::max(static_cast<int>(corner.size()), maxCut) + std::max(thickness, 0);
}

bool Brass::fits(const CBox& box, int unit, const std::vector<int>& corner, int thickness) {
    const double SPAN = static_cast<double>(cornerSpan(corner, thickness)) * unit;
    return box.w >= SPAN * 2 && box.h >= SPAN * 2;
}

// Maps a run [x0, x1) on row y of a corner-local grid (x grows away from the
// corner horizontally, y vertically) to monitor pixels.
static CBox mapCornerRun(const CBox& box, int unit, int corner, int x0, int x1, int y) {
    const bool RIGHT  = corner & 1;
    const bool BOTTOM = corner & 2;

    const double W = static_cast<double>(x1 - x0) * unit;
    const double X = RIGHT ? box.x + box.w - static_cast<double>(x1) * unit : box.x + static_cast<double>(x0) * unit;
    const double Y = BOTTOM ? box.y + box.h - static_cast<double>(y + 1) * unit : box.y + static_cast<double>(y) * unit;

    return {X, Y, W, static_cast<double>(unit)};
}

CRegion Brass::cutRegion(const CBox& box, int unit, const std::vector<int>& corner) {
    CRegion region;

    for (int c = 0; c < 4; ++c) {
        for (size_t y = 0; y < corner.size(); ++y) {
            if (corner[y] <= 0)
                continue;
            region.add(mapCornerRun(box, unit, c, 0, corner[y], static_cast<int>(y)));
        }
    }

    return region;
}

// whether cell (x, y) of a corner-local grid is part of the outline: an inside
// cell with an outside cell within `thickness` steps (manhattan distance, which
// gives the classic diagonal pixel-art outline around the staircase)
static bool isBorderCell(const std::vector<int>& corner, int thickness, int x, int y) {
    const auto N     = static_cast<int>(corner.size());
    auto       isCut = [&](int cx, int cy) { return cy >= 0 && cy < N && cx >= 0 && cx < corner[cy]; };

    if (isCut(x, y))
        return false;
    if (x < thickness || y < thickness)
        return true;

    for (int cy = 0; cy < N; ++cy) {
        for (int cx = 0; cx < corner[cy]; ++cx) {
            if (std::abs(x - cx) + std::abs(y - cy) <= thickness)
                return true;
        }
    }
    return false;
}

// runs of border cells of the K x K corner square of corner `c`
template <typename F>
static void forEachBorderRun(const CBox& box, int unit, const std::vector<int>& corner, int thickness, int c, F&& fn) {
    const int K = cornerSpan(corner, thickness);
    for (int y = 0; y < K; ++y) {
        int x = 0;
        while (x < K) {
            if (!isBorderCell(corner, thickness, x, y)) {
                ++x;
                continue;
            }
            const int start = x;
            while (x < K && isBorderCell(corner, thickness, x, y))
                ++x;

            fn(mapCornerRun(box, unit, c, start, x, y));
        }
    }
}

std::vector<CBox> Brass::borderRects(const CBox& box, int unit, const std::vector<int>& corner, int thickness) {
    std::vector<CBox> out;
    if (thickness <= 0)
        return out;

    const int K = cornerSpan(corner, thickness);

    for (int c = 0; c < 4; ++c)
        forEachBorderRun(box, unit, corner, thickness, c, [&](const CBox& r) { out.push_back(r); });

    // straight edges between the corner squares
    const double SPAN  = static_cast<double>(K) * unit;
    const double THICK = static_cast<double>(thickness) * unit;

    if (box.w > SPAN * 2) {
        out.push_back({box.x + SPAN, box.y, box.w - SPAN * 2, THICK});
        out.push_back({box.x + SPAN, box.y + box.h - THICK, box.w - SPAN * 2, THICK});
    }
    if (box.h > SPAN * 2) {
        out.push_back({box.x, box.y + SPAN, THICK, box.h - SPAN * 2});
        out.push_back({box.x + box.w - THICK, box.y + SPAN, THICK, box.h - SPAN * 2});
    }

    return out;
}

// a zero-sized box: all four of its corners are at `at`
CRegion Brass::cornerCut(const Hyprutils::Math::Vector2D& at, int which, int unit, const std::vector<int>& corner) {
    const CBox BOX = {at.x, at.y, 0, 0};
    CRegion    out;
    for (size_t y = 0; y < corner.size(); ++y) {
        if (corner[y] > 0)
            out.add(mapCornerRun(BOX, unit, which, 0, corner[y], static_cast<int>(y)));
    }
    return out;
}

CRegion Brass::cornerBorder(const Hyprutils::Math::Vector2D& at, int which, int unit, const std::vector<int>& corner, int thickness) {
    CRegion out;
    if (thickness > 0)
        forEachBorderRun(CBox{at.x, at.y, 0, 0}, unit, corner, thickness, which, [&](const CBox& r) { out.add(r); });
    return out;
}

CRegion Brass::outerRing(const CBox& box, int unit, const std::vector<int>& corner, int thickness) {
    CRegion out;
    if (thickness <= 0)
        return out;

    const auto N = static_cast<int>(corner.size());
    const int  K = cornerSpan(corner, 0); // the corner square inside the box
    const int  T = thickness;

    // corner-local grid, extending to negative coordinates outside the box
    auto inside = [&](int x, int y) { return x >= 0 && y >= 0 && !(y < N && x < corner[y]); };
    auto isRing = [&](int x, int y) {
        if (inside(x, y))
            return false;
        for (int dy = -T; dy <= T; ++dy) {
            const int R = T - std::abs(dy);
            for (int dx = -R; dx <= R; ++dx) {
                if (inside(x + dx, y + dy))
                    return true;
            }
        }
        return false;
    };

    // corners: [-T, K) x [-T, K)
    for (int y = -T; y < K; ++y) {
        int x = -T;
        while (x < K) {
            if (!isRing(x, y)) {
                ++x;
                continue;
            }
            const int start = x;
            while (x < K && isRing(x, y))
                ++x;

            for (int c = 0; c < 4; ++c)
                out.add(mapCornerRun(box, unit, c, start, x, y));
        }
    }

    // straight edges between the corner squares
    const double SPAN  = static_cast<double>(K) * unit;
    const double THICK = static_cast<double>(T) * unit;

    if (box.w > SPAN * 2) {
        out.add(CBox{box.x + SPAN, box.y - THICK, box.w - SPAN * 2, THICK});
        out.add(CBox{box.x + SPAN, box.y + box.h, box.w - SPAN * 2, THICK});
    }
    if (box.h > SPAN * 2) {
        out.add(CBox{box.x - THICK, box.y + SPAN, THICK, box.h - SPAN * 2});
        out.add(CBox{box.x + box.w, box.y + SPAN, THICK, box.h - SPAN * 2});
    }

    return out;
}

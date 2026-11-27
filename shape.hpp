#pragma once

#include <hyprutils/math/Box.hpp>
#include <hyprutils/math/Region.hpp>
#include <string>
#include <vector>

// Pixel-art window shape: a box whose four corners have a staircase of
// "art pixels" removed. One art pixel = `unit` monitor pixels.
//
// The corner pattern lists, row by row starting from the outermost edge,
// how many art pixels are cut away from the corner:
//   "1"     -> single missing pixel          .###
//                                            #...
//   "2,1"   -> two-step staircase            ..###
//                                            .#...
//                                            #....
//   "3,1,1" -> rounder corner
namespace Brass {
    using Hyprutils::Math::CBox;
    using Hyprutils::Math::CRegion;

    std::vector<int> parseCorner(const std::string& str);

    // monitor pixels per art pixel
    int unitPx(int pixelSize, double scale);

    // whether the box is big enough to carry the corner pattern (+ border)
    bool fits(const CBox& box, int unit, const std::vector<int>& corner, int thickness);

    // the area removed from the box at its four corners (monitor px)
    CRegion cutRegion(const CBox& box, int unit, const std::vector<int>& corner);

    // the outline that follows the cut shape, `thickness` art pixels wide,
    // drawn on the inside of the box (monitor px)
    std::vector<CBox> borderRects(const CBox& box, int unit, const std::vector<int>& corner, int thickness);

    // the outline `thickness` art pixels wide just *outside* the cut shape:
    // the cells not in the shape within `thickness` steps of it (monitor px)
    CRegion outerRing(const CBox& box, int unit, const std::vector<int>& corner, int thickness);

    // a single corner whose tip is at `at` (monitor px). `which`: bit 0 = right, bit 1 = bottom.
    // The cut cells and the outline cells of that corner, like cutRegion / borderRects.
    CRegion cornerCut(const Hyprutils::Math::Vector2D& at, int which, int unit, const std::vector<int>& corner);
    CRegion cornerBorder(const Hyprutils::Math::Vector2D& at, int which, int unit, const std::vector<int>& corner, int thickness);
}

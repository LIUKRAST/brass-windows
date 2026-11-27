#pragma once

#define WLR_USE_UNSTABLE

#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/Texture.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>

#include <cairo/cairo.h>

#include <string>
#include <unordered_map>
#include <vector>

inline HANDLE PHANDLE = nullptr;

class CBrassDeco;

// A pixel-art sprite. The PNG is decoded on the CPU when the button is added
// (so its size is known for hit-testing) and uploaded to the GPU lazily on
// the first frame that draws it.
struct SBrassSprite {
    cairo_surface_t*     surface = nullptr;
    SP<Render::ITexture> tex;
    int                  w = 0, h = 0; // in art pixels

    SBrassSprite() = default;
    ~SBrassSprite();
    SBrassSprite(const SBrassSprite&)            = delete;
    SBrassSprite& operator=(const SBrassSprite&) = delete;

    SP<Render::ITexture> texture();
};

struct SBrassButton {
    std::string       action;
    SP<SBrassSprite>  normal, hover, pressed, inactive; // only `normal` is mandatory
};

struct SBrassState {
    std::vector<SBrassButton>   buttons;
    std::vector<WP<CBrassDeco>> decos;

    // parsed `corner` pattern: row i cuts pattern[i] art pixels from the corner
    std::vector<int> corner = {1};

    uint32_t         noBarRuleIdx   = 0;
    uint32_t         noFrameRuleIdx = 0;

    struct {
        SP<Config::Values::CBoolValue>   enabled, barEnabled, barOnTiled, titleEnabled, cutout;
        SP<Config::Values::CIntValue>    pixelSize, borderSize, barHeight, titleSize, buttonsPadding, buttonsSpacing, cutoutGap;
        SP<Config::Values::CStringValue> corner, titleFont, titleAlign, buttonsAlign, onDoubleClick;
        SP<Config::Values::CColorValue>  activeColor, inactiveColor, barColor, titleColor, titleInactiveColor;
    } cfg;
};

inline UP<SBrassState> g_brass;

SP<SBrassSprite> loadSprite(const std::string& path, std::string& err);
void             runBrassAction(const std::string& action, PHLWINDOW window);

// What the windows stacked above `window` on `monitor` take away from it
// (monitor px, before render modifications):
//  - hole: their shape grown by `cutout_gap` art pixels. Nothing of `window`
//    is drawn there, so the wallpaper shows through around them.
//  - ring: the `border_size` outline just outside each hole, where `window`
//    draws its own outline (not clipped to `window` yet).
struct SBrassCover {
    CRegion hole, ring;
};
SBrassCover brassCover(const PHLWINDOW& window, const PHLMONITOR& monitor, bool withRing);

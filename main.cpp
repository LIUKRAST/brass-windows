#define WLR_USE_UNSTABLE

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRuleEffectContainer.hpp>
#include <hyprland/src/config/lua/bindings/LuaBindingsInternal.hpp>
#include <hyprland/src/config/supplementary/executor/Executor.hpp>
#include <hyprland/src/config/shared/actions/ConfigActions.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenTypes.hpp>
#include <hyprland/src/render/ElementRenderer.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/state/MonitorState.hpp>

#include <algorithm>
#include <cstdlib>

#include "deco.hpp"
#include "globals.hpp"
#include "shape.hpp"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

// ---------------------------------------------------------------- sprites / actions

SBrassSprite::~SBrassSprite() {
    if (surface)
        cairo_surface_destroy(surface);
}

SP<Render::ITexture> SBrassSprite::texture() {
    if (!tex && surface)
        tex = g_pHyprRenderer->createTexture(surface);
    return tex;
}

static std::string expandPath(const std::string& path) {
    if (path.starts_with("~/")) {
        if (const char* home = std::getenv("HOME"))
            return std::string(home) + path.substr(1);
    }
    return path;
}

SP<SBrassSprite> loadSprite(const std::string& path, std::string& err) {
    const auto FULL = expandPath(path);
    auto       surf = cairo_image_surface_create_from_png(FULL.c_str());

    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        err = std::string("can't load png ") + FULL + ": " + cairo_status_to_string(cairo_surface_status(surf));
        cairo_surface_destroy(surf);
        return nullptr;
    }

    auto sprite     = makeShared<SBrassSprite>();
    sprite->surface = surf;
    sprite->w       = cairo_image_surface_get_width(surf);
    sprite->h       = cairo_image_surface_get_height(surf);
    return sprite;
}

void runBrassAction(const std::string& action, PHLWINDOW window) {
    namespace CA = Config::Actions;

    if (!window)
        return;

    if (action == "close")
        (void)CA::closeWindow(window);
    else if (action == "maximize")
        (void)CA::fullscreenWindow(Fullscreen::FSMODE_MAXIMIZED, true, window);
    else if (action == "fullscreen")
        (void)CA::fullscreenWindow(Fullscreen::FSMODE_FULLSCREEN, true, window);
    else if (action == "minimize")
        (void)CA::moveToWorkspace("special:minimized", true, window);
    else if (action == "float")
        (void)CA::floatWindow(CA::TOGGLE_ACTION_TOGGLE, window);
    else if (action == "pin")
        (void)CA::pinWindow(CA::TOGGLE_ACTION_TOGGLE, window);
    else if (!action.empty())
        Config::Supplementary::executor()->spawn(action);
}

// ---------------------------------------------------------------- corner cutting

static CBrassDeco* decoFor(const PHLWINDOW& w) {
    if (!w)
        return nullptr;

    for (const auto& d : g_brass->decos) {
        if (d && d->owner() == w)
            return d.get();
    }
    return nullptr;
}

// ---------------------------------------------------------------- cutout

// Paint order of the windows of one monitor, as in IHyprRenderer::renderWorkspaceWindows:
// workspace < special workspace < pinned, then tiled < focused tiled < floating,
// then the order of windowState()->windows().
static std::tuple<int, int, size_t> stackKey(const PHLWINDOW& w, size_t idx) {
    const bool PINNED = w->isFloating() && sc<bool>(w->m_state & Desktop::View::WINDOW_STATE_PINNED);
    const int  GROUP  = PINNED ? 2 : w->onSpecialWorkspace() ? 1 : 0;
    const int  LAYER  = w->isFloating() ? 2 : w == Desktop::focusState()->window() ? 1 : 0;
    return {GROUP, LAYER, idx};
}

static bool isFullscreenAny(const PHLWINDOW& w) {
    return Fullscreen::controller()->getFullscreenModes(w).internal != Fullscreen::FSMODE_NONE;
}

// full shape (bar + window) of a window as it is painted, in monitor px
static CBox paintedShape(const PHLWINDOW& w, CBrassDeco* deco, const PHLMONITOR& monitor, int unit) {
    const auto PRES = w->presentation().renderPresentation();
    CBox       win  = {w->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) + PRES.workspaceOffset + PRES.floatingOffset - monitor->m_position,
                       w->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT)};
    win.scale(monitor->m_scale).round();

    return deco->shapeBox(win, unit, deco->barShown() && w->m_ruleApplicator->decorate().valueOrDefault());
}

// Where an edge of `hole` crosses an edge of `shape`, the part of `shape` that
// stays visible has a new corner: cut it like the real ones (and outline it).
static void addJunctions(SBrassCover& out, const CBox& shape, const CBox& hole, int unit, int thick, bool withRing) {
    const auto& PATTERN = g_brass->corner;
    if (PATTERN.empty())
        return;

    const double SL = shape.x, ST = shape.y, SR = shape.x + shape.w, SB = shape.y + shape.h;
    const double HL = hole.x, HT = hole.y, HR = hole.x + hole.w, HB = hole.y + hole.h;

    auto add = [&](double x, double y, int which) {
        out.hole.add(Brass::cornerCut({x, y}, which, unit, PATTERN));
        if (withRing)
            out.ring.add(Brass::cornerBorder({x, y}, which, unit, PATTERN, thick));
    };

    constexpr int TL = 0, TR = 1, BL = 2, BR = 3;

    // vertical hole edges crossing the top / bottom edge of the shape
    const bool HOLE_OVER_TOP    = HT <= ST && HB > ST;
    const bool HOLE_OVER_BOTTOM = HB >= SB && HT < SB;
    if (HL > SL && HL < SR) { // shape visible on the left of the hole
        if (HOLE_OVER_TOP)
            add(HL, ST, TR);
        if (HOLE_OVER_BOTTOM)
            add(HL, SB, BR);
    }
    if (HR > SL && HR < SR) { // shape visible on the right of the hole
        if (HOLE_OVER_TOP)
            add(HR, ST, TL);
        if (HOLE_OVER_BOTTOM)
            add(HR, SB, BL);
    }

    // horizontal hole edges crossing the left / right edge of the shape
    const bool HOLE_OVER_LEFT  = HL <= SL && HR > SL;
    const bool HOLE_OVER_RIGHT = HR >= SR && HL < SR;
    if (HT > ST && HT < SB) { // shape visible above the hole
        if (HOLE_OVER_LEFT)
            add(SL, HT, BL);
        if (HOLE_OVER_RIGHT)
            add(SR, HT, BR);
    }
    if (HB > ST && HB < SB) { // shape visible below the hole
        if (HOLE_OVER_LEFT)
            add(SL, HB, TL);
        if (HOLE_OVER_RIGHT)
            add(SR, HB, TR);
    }
}

SBrassCover brassCover(const PHLWINDOW& window, const PHLMONITOR& monitor, bool withRing) {
    SBrassCover out;
    if (!window || !monitor || !g_brass->cfg.enabled->value() || !g_brass->cfg.cutout->value())
        return out;

    // fullscreen workspaces are painted in another order; a fullscreen window covers everything anyway
    if (window->m_workspace && Fullscreen::controller()->getFullscreenModes(window->m_workspace).internal != Fullscreen::FSMODE_NONE)
        return out;

    const auto& WINDOWS = Desktop::windowState()->windows();
    const auto  IT      = std::ranges::find(WINDOWS, window);
    if (IT == WINDOWS.end())
        return out;

    const auto KEY   = stackKey(window, IT - WINDOWS.begin());
    const auto SCALE = monitor->m_scale;
    const int  UNIT  = Brass::unitPx(g_brass->cfg.pixelSize->value(), SCALE);
    const int  GAP   = std::max(0, sc<int>(g_brass->cfg.cutoutGap->value()));
    const int  THICK = std::max(0, sc<int>(g_brass->cfg.borderSize->value()));

    const auto SELF       = decoFor(window);
    const bool JUNCTIONS  = SELF && SELF->frameEnabled();
    const CBox SELF_SHAPE = SELF ? paintedShape(window, SELF, monitor, UNIT) : CBox{};

    for (size_t i = 0; i < WINDOWS.size(); ++i) {
        const auto& W = WINDOWS[i];
        if (W == window || !validMapped(W) || W->isHidden() || isFullscreenAny(W))
            continue;

        const bool PINNED = W->isFloating() && sc<bool>(W->m_state & Desktop::View::WINDOW_STATE_PINNED);
        if (W->m_workspace != window->m_workspace && !PINNED && !(W->onSpecialWorkspace() && !window->onSpecialWorkspace()))
            continue;

        if (stackKey(W, i) <= KEY || !g_pHyprRenderer->shouldRenderWindow(W, monitor))
            continue;

        if (W->presentation().alphaValue(Desktop::View::WINDOW_ALPHA_FADE) <= 0.F)
            continue;

        const auto DECO = decoFor(W);
        if (!DECO)
            continue;

        const auto  SHAPE   = paintedShape(W, DECO, monitor, UNIT);
        CBox        hole    = SHAPE;
        hole.expand(sc<double>(GAP) * UNIT);

        const bool  CORNERS = DECO->frameEnabled() && Brass::fits(SHAPE, UNIT, g_brass->corner, THICK);
        const auto& PATTERN = CORNERS ? g_brass->corner : std::vector<int>{};

        CRegion     region{hole};
        region.subtract(Brass::cutRegion(hole, UNIT, PATTERN));
        out.hole.add(region);

        if (withRing)
            out.ring.add(Brass::outerRing(hole, UNIT, PATTERN, THICK));

        if (JUNCTIONS)
            addJunctions(out, SELF_SHAPE, hole, UNIT, THICK, withRing);
    }

    return out;
}

// The area of this surface's window that must not be drawn, in monitor pixels
// (before render modifications): the cut corners and what the windows above
// take away. Empty if nothing is cut.
static CRegion surfaceCut(const CSurfacePassElement::SRenderData& d, double* scaleOut = nullptr) {
    if (!g_brass->cfg.enabled->value() || !d.pWindow)
        return {};

    const auto MON = d.pMonitor.lock();
    if (!MON)
        return {};

    const auto DECO = decoFor(d.pWindow);
    if (!DECO)
        return {};

    const auto SCALE = MON->m_scale;
    const int  UNIT  = Brass::unitPx(g_brass->cfg.pixelSize->value(), SCALE);

    CRegion    cut = brassCover(d.pWindow, MON, false).hole;

    if (DECO->frameEnabled() && !d.popup && !d.dontRound) {
        CBox win = {d.pos.x - MON->m_position.x, d.pos.y - MON->m_position.y, d.w, d.h};
        win.scale(SCALE).round();

        const auto SHAPE = DECO->shapeBox(win, UNIT, d.decorate && DECO->barShown());
        if (Brass::fits(SHAPE, UNIT, g_brass->corner, std::max(0, sc<int>(g_brass->cfg.borderSize->value()))))
            cut.add(Brass::cutRegion(SHAPE, UNIT, g_brass->corner));
    }

    if (scaleOut)
        *scaleOut = SCALE;

    return cut;
}

using PreDrawSurfaceFn = void (*)(Render::IElementRenderer*, Render::CRenderContext&, WP<CSurfacePassElement>, const CRegion&);
using OpaqueRegionFn   = CRegion (*)(CSurfacePassElement*, Render::CRenderContext&);

static CFunctionHook* g_preDrawSurfaceHook = nullptr;
static CFunctionHook* g_opaqueRegionHook   = nullptr;

// Every surface of a window is drawn with the window's corners removed from
// the damage, so those pixels are simply never written: whatever is behind
// the window shows through, exactly one art pixel at a time.
static void hkPreDrawSurface(Render::IElementRenderer* self, Render::CRenderContext& ctx, WP<CSurfacePassElement> element, const CRegion& damage) {
    const auto ORIGINAL = (PreDrawSurfaceFn)g_preDrawSurfaceHook->m_original;

    CRegion    cut = element ? surfaceCut(element->m_data) : CRegion{};
    if (cut.empty()) {
        ORIGINAL(self, ctx, element, damage);
        return;
    }

    ctx.m_data.renderModif.applyToRegion(cut);

    const CRegion SAVED = ctx.m_data.damage.copy();
    ctx.m_data.damage.subtract(cut);

    ORIGINAL(self, ctx, element, damage);

    ctx.m_data.damage = SAVED;
}

// The corners aren't opaque anymore: without this Hyprland would skip drawing
// what is behind them (occlusion culling) and leave stale pixels there.
static CRegion hkOpaqueRegion(CSurfacePassElement* self, Render::CRenderContext& ctx) {
    CRegion opaque = ((OpaqueRegionFn)g_opaqueRegionHook->m_original)(self, ctx);
    if (opaque.empty())
        return opaque;

    double  scale = 1.0;
    CRegion cut   = surfaceCut(self->m_data, &scale);
    if (cut.empty())
        return opaque;

    cut.scale(1.0 / scale).expand(1);
    return opaque.subtract(cut);
}

// ---------------------------------------------------------------- windows

static void addDeco(PHLWINDOW window) {
    if (decoFor(window))
        return;

    auto deco    = makeShared<CBrassDeco>(window);
    deco->m_self = deco;
    g_brass->decos.emplace_back(deco);

    auto margin    = makeShared<CBrassMarginDeco>(window);
    deco->m_margin = margin;

    HyprlandAPI::addWindowDecoration(PHANDLE, window, std::move(deco));
    HyprlandAPI::addWindowDecoration(PHANDLE, window, std::move(margin));
}

static void forEachDeco(const std::function<void(CBrassDeco*)>& fn) {
    // copy: callbacks may close windows
    const auto DECOS = g_brass->decos;
    for (const auto& d : DECOS) {
        if (d)
            fn(d.get());
    }
}

static void onConfigReloaded() {
    g_brass->corner = Brass::parseCorner(g_brass->cfg.corner->value());
    forEachDeco([](CBrassDeco* d) { d->onConfigReloaded(); });
}

// ---------------------------------------------------------------- lua

// hl.plugin.brasswindows.add_button({
//     sprite   = "~/.config/hypr/brass/close.png",          -- required
//     hover    = "~/.config/hypr/brass/close_hover.png",    -- optional
//     pressed  = "~/.config/hypr/brass/close_pressed.png",  -- optional
//     inactive = "~/.config/hypr/brass/close_inactive.png", -- optional (window not focused)
//     action   = "close",                                   -- close | maximize | fullscreen | minimize | float | pin | <shell command>
// })
static int luaAddButton(lua_State* L) {
    using Config::Lua::Bindings::Internal::configError;

    if (!lua_istable(L, 1))
        return configError(L, "brasswindows.add_button: expected a table { sprite, hover?, pressed?, inactive?, action }");

    auto field = [L](const char* name, std::string& out) -> bool {
        lua_getfield(L, 1, name);
        const bool OK = lua_isstring(L, -1);
        if (OK)
            out = lua_tostring(L, -1);
        lua_pop(L, 1);
        return OK;
    };

    SBrassButton button;
    std::string  path, err;

    if (!field("sprite", path))
        return configError(L, "brasswindows.add_button: `sprite` (path to a png) is required");
    if (!(button.normal = loadSprite(path, err)))
        return configError(L, ("brasswindows.add_button: " + err).c_str());

    std::pair<const char*, SP<SBrassSprite>*> optional[] = {{"hover", &button.hover}, {"pressed", &button.pressed}, {"inactive", &button.inactive}};
    for (auto& [name, target] : optional) {
        if (!field(name, path))
            continue;
        if (!(*target = loadSprite(path, err)))
            return configError(L, ("brasswindows.add_button: " + err).c_str());
    }

    if (!field("action", button.action))
        return configError(L, "brasswindows.add_button: `action` is required");

    g_brass->buttons.push_back(std::move(button));
    forEachDeco([](CBrassDeco* d) { d->damageEntire(); });
    return 0;
}

// ---------------------------------------------------------------- init

static CFunctionHook* hookFunction(const std::string& name, const std::string& demangledNeedle, void* destination) {
    for (const auto& m : HyprlandAPI::findFunctionsByName(PHANDLE, name)) {
        if (!m.demangled.contains(demangledNeedle))
            continue;

        auto hook = HyprlandAPI::createFunctionHook(PHANDLE, m.address, destination);
        if (!hook || !hook->hook())
            throw std::runtime_error("[brasswindows] failed to hook " + demangledNeedle);
        return hook;
    }

    throw std::runtime_error("[brasswindows] function not found: " + demangledNeedle);
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;

    if (std::string(__hyprland_api_get_hash()) != std::string(__hyprland_api_get_client_hash())) {
        HyprlandAPI::addNotification(PHANDLE, "[brasswindows] Mismatched headers! Can't proceed.", CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        throw std::runtime_error("[brasswindows] Version mismatch");
    }

    g_brass = makeUnique<SBrassState>();
    auto& c = g_brass->cfg;

    using namespace Config::Values;
    // clang-format off
    c.enabled            = makeShared<CBoolValue>("plugin:brasswindows:enabled", "Cut the window corners and draw the focus outline", true);
    c.pixelSize          = makeShared<CIntValue>("plugin:brasswindows:pixel_size", "Size of one art pixel, in logical pixels", 4);
    c.corner             = makeShared<CStringValue>("plugin:brasswindows:corner", "Corner staircase: art pixels cut per row, from the edge (e.g. \"2,1\")", "1");
    c.borderSize         = makeShared<CIntValue>("plugin:brasswindows:border_size", "Outline thickness, in art pixels (drawn on top of the window)", 1);
    c.cutout             = makeShared<CBoolValue>("plugin:brasswindows:cutout", "Windows erase what is below them, plus a gap, and the windows below outline the hole", true);
    c.cutoutGap          = makeShared<CIntValue>("plugin:brasswindows:cutout_gap", "Gap around a window cut into the windows below it, in art pixels", 1);
    c.activeColor        = makeShared<CColorValue>("plugin:brasswindows:active_color", "Outline color of the focused window", 0xffffffff);
    c.inactiveColor      = makeShared<CColorValue>("plugin:brasswindows:inactive_color", "Outline color of unfocused windows (transparent = none)", 0x00000000);

    c.barEnabled         = makeShared<CBoolValue>("plugin:brasswindows:bar_enabled", "Show a title bar on floating windows", true);
    c.barOnTiled         = makeShared<CBoolValue>("plugin:brasswindows:bar_on_tiled", "Show the title bar on tiled windows too", false);
    c.barHeight          = makeShared<CIntValue>("plugin:brasswindows:bar_height", "Title bar height, in art pixels", 9);
    c.barColor           = makeShared<CColorValue>("plugin:brasswindows:bar_color", "Title bar color", 0xff000000);
    c.buttonsAlign       = makeShared<CStringValue>("plugin:brasswindows:buttons_align", "Buttons on the \"left\" (macOS) or \"right\"", "left");
    c.buttonsPadding     = makeShared<CIntValue>("plugin:brasswindows:buttons_padding", "Space between the bar edge and the buttons, in art pixels", 2);
    c.buttonsSpacing     = makeShared<CIntValue>("plugin:brasswindows:buttons_spacing", "Space between buttons, in art pixels", 1);
    c.titleEnabled       = makeShared<CBoolValue>("plugin:brasswindows:title_enabled", "Show the window title in the bar", true);
    c.titleFont          = makeShared<CStringValue>("plugin:brasswindows:title_font", "Title font", "Sans");
    c.titleSize          = makeShared<CIntValue>("plugin:brasswindows:title_size", "Title font size, in logical pixels", 16);
    c.titleAlign         = makeShared<CStringValue>("plugin:brasswindows:title_align", "Title alignment: \"center\" or \"left\"", "center");
    c.titleColor         = makeShared<CColorValue>("plugin:brasswindows:title_color", "Title color", 0xffffffff);
    c.titleInactiveColor = makeShared<CColorValue>("plugin:brasswindows:title_inactive_color", "Title color of unfocused windows", 0xff808080);
    c.onDoubleClick      = makeShared<CStringValue>("plugin:brasswindows:on_double_click", "Action on double click of the bar (same as button actions)", "maximize");
    // clang-format on

    for (const auto& v : std::initializer_list<SP<Config::Values::IValue>>{c.enabled, c.cutout, c.cutoutGap, c.pixelSize, c.corner, c.borderSize, c.activeColor, c.inactiveColor, c.barEnabled,
                                                                          c.barOnTiled, c.barHeight, c.barColor, c.buttonsAlign, c.buttonsPadding, c.buttonsSpacing,
                                                                          c.titleEnabled, c.titleFont, c.titleSize, c.titleAlign, c.titleColor, c.titleInactiveColor,
                                                                          c.onDoubleClick}) {
        if (!HyprlandAPI::addConfigValueV2(PHANDLE, v))
            throw std::runtime_error("[brasswindows] unable to register a config value");
    }

    g_brass->noBarRuleIdx   = Desktop::Rule::windowEffects()->registerEffect("brasswindows:no_bar");
    g_brass->noFrameRuleIdx = Desktop::Rule::windowEffects()->registerEffect("brasswindows:no_frame");

    HyprlandAPI::addLuaFunction(PHANDLE, "brasswindows", "add_button", ::luaAddButton);

    static auto P1 = Event::bus()->m_events.window.open.listen([](PHLWINDOW w) { addDeco(w); });
    static auto P2 = Event::bus()->m_events.window.updateRules.listen([](PHLWINDOW w) {
        if (auto d = decoFor(w))
            d->updateRules();
    });
    static auto P3 = Event::bus()->m_events.window.floating.listen([](PHLWINDOW w) {
        if (auto d = decoFor(w))
            d->refresh();
    });
    static auto P4 = Event::bus()->m_events.window.fullscreen.listen([](PHLWINDOW w) {
        if (auto d = decoFor(w))
            d->refresh();
    });
    static auto P5 = Event::bus()->m_events.window.active.listen([](PHLWINDOW, Desktop::eFocusReason) { forEachDeco([](CBrassDeco* d) { d->onFocusChanged(); }); });
    static auto P6 = Event::bus()->m_events.config.preReload.listen([] { g_brass->buttons.clear(); });
    static auto P7 = Event::bus()->m_events.config.reloaded.listen([] { onConfigReloaded(); });

    g_preDrawSurfaceHook = hookFunction("preDrawSurface", "IElementRenderer::preDrawSurface", (void*)&hkPreDrawSurface);
    g_opaqueRegionHook   = hookFunction("opaqueRegion", "CSurfacePassElement::opaqueRegion", (void*)&hkOpaqueRegion);

    for (auto& w : Desktop::windowState()->windows()) {
        if (w->isHidden() || !validMapped(w))
            continue;
        addDeco(w);
    }

    HyprlandAPI::reloadConfig();

    return {"brasswindows", "Pixel-art window corners, focus outline and floating title bars", "LiukRast", "2.0"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    for (auto& m : State::monitorState()->monitors())
        m->m_scheduledRecalc = true;

    g_pHyprRenderer->context().m_pass.removeAllOfType("CBrassPassElement");

    Desktop::Rule::windowEffects()->unregisterEffect(g_brass->noBarRuleIdx);
    Desktop::Rule::windowEffects()->unregisterEffect(g_brass->noFrameRuleIdx);
}

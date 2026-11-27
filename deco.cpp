#include "deco.hpp"
#include "shape.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/FocusState.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/desktop/view/window/WindowPresentation.hpp>
#include <hyprland/src/desktop/rule/windowRule/WindowRule.hpp>
#include <hyprland/src/helpers/MiscFunctions.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/managers/fullscreen/FullscreenController.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/decorations/DecorationPositioner.hpp>
#include <hyprland/src/config/shared/animation/AnimationTree.hpp>
#include <hyprland/src/animation/AnimationManager.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>

#include <linux/input-event-codes.h>

using namespace Render::GL;

static CHyprColor configColor(Config::INTEGER c) {
    return CHyprColor{sc<uint64_t>(c)};
}

static const std::vector<int> NO_CORNER = {};

// ---------------------------------------------------------------- lifecycle

CBrassDeco::CBrassDeco(PHLWINDOW pWindow) : IHyprWindowDecoration(pWindow), m_window(pWindow) {
    Animation::mgr()->createAnimation(configColor(g_brass->cfg.inactiveColor->value()), m_borderColor, Config::animationTree()->getAnimationPropertyConfig("border"), pWindow,
                                      AVARDAMAGE_NONE);
    m_borderColor->setUpdateCallback([this](auto) { damageEntire(); });

    m_mouseButtonListener = Event::bus()->m_events.input.mouse.button.listen([this](IPointer::SButtonEvent e, Event::SCallbackInfo& info) { onMouseButton(info, e); });
    m_mouseMoveListener   = Event::bus()->m_events.input.mouse.move.listen([this](Vector2D, Event::SCallbackInfo&) { onMouseMove(); });

    // not registered on the window yet: only read the state, don't reposition
    readRules();
    m_barShown = wantsBar();
}

CBrassDeco::~CBrassDeco() {
    if (m_dragging)
        g_layoutManager->endDragTarget();

    std::erase(g_brass->decos, m_self);
}

PHLWINDOW CBrassDeco::owner() const {
    return m_window.lock();
}

std::string CBrassDeco::getDisplayName() {
    return "BrassWindows";
}

eDecorationType CBrassDeco::getDecorationType() {
    return DECORATION_CUSTOM;
}

eDecorationLayer CBrassDeco::getDecorationLayer() {
    // above the window surface (the outline is drawn on top of the contents), below popups
    return DECORATION_LAYER_OVER;
}

uint64_t CBrassDeco::getDecorationFlags() {
    return DECORATION_ALLOWS_MOUSE_INPUT | DECORATION_PART_OF_MAIN_WINDOW;
}

// ---------------------------------------------------------------- state

bool CBrassDeco::focused() const {
    const auto W = owner();
    return W && Desktop::focusState()->window() == W;
}

bool CBrassDeco::frameEnabled() const {
    return g_brass->cfg.enabled->value() && !m_ruleNoFrame;
}

bool CBrassDeco::barShown() const {
    return m_barShown;
}

bool CBrassDeco::wantsBar() const {
    const auto W = owner();
    if (!W || !g_brass->cfg.enabled->value() || !g_brass->cfg.barEnabled->value() || m_ruleNoBar)
        return false;

    if (!W->isFloating() && !g_brass->cfg.barOnTiled->value())
        return false;

    return Fullscreen::controller()->getFullscreenModes(W).internal != Fullscreen::FSMODE_FULLSCREEN;
}

void CBrassDeco::refresh() {
    const bool WANT = wantsBar();
    if (WANT == m_barShown)
        return;

    m_barShown = WANT;
    g_pDecorationPositioner->repositionDeco(this);

    if (const auto W = owner(); W && W->layoutTarget())
        W->layoutTarget()->recalc();

    damageEntire();
}

void CBrassDeco::readRules() {
    const auto W = owner();
    if (!W)
        return;

    const auto& PROPS = W->m_ruleApplicator->m_otherProps.props;

    m_ruleNoBar   = PROPS.contains(g_brass->noBarRuleIdx) && truthy(PROPS.at(g_brass->noBarRuleIdx)->effect);
    m_ruleNoFrame = PROPS.contains(g_brass->noFrameRuleIdx) && truthy(PROPS.at(g_brass->noFrameRuleIdx)->effect);
}

void CBrassDeco::updateRules() {
    readRules();
    refresh();
    damageEntire();
}

void CBrassDeco::onConfigReloaded() {
    m_titleTex = nullptr;
    m_hovered = m_pressed = -1;

    // the bar height may have changed even if its visibility didn't
    g_pDecorationPositioner->repositionDeco(this);
    if (const auto M = m_margin.lock())
        g_pDecorationPositioner->repositionDeco(M.get());
    refresh();
    damageEntire();
}

void CBrassDeco::onFocusChanged() {
    const auto GOAL = configColor(focused() ? g_brass->cfg.activeColor->value() : g_brass->cfg.inactiveColor->value());
    if (m_borderColor->goal() != GOAL)
        *m_borderColor = GOAL;

    damageEntire();
}

void CBrassDeco::updateWindow(PHLWINDOW) {
    damageEntire();
}

void CBrassDeco::damageEntire() {
    if (const auto W = owner(); W && validMapped(W))
        g_pHyprRenderer->damageWindow(W);
}

// ---------------------------------------------------------------- positioning

SDecorationPositioningInfo CBrassDeco::getPositioningInfo() {
    const int HEIGHT = m_barShown ? g_brass->cfg.barHeight->value() * g_brass->cfg.pixelSize->value() : 0;

    SDecorationPositioningInfo info;
    info.policy         = DECORATION_POSITION_STICKY;
    info.edges          = DECORATION_EDGE_TOP;
    info.priority       = 10005; // right against the window
    info.reserved       = true;
    info.desiredExtents = {{0, sc<int>(HEIGHT)}, {0, 0}};
    return info;
}

SDecorationPositioningInfo CBrassMarginDeco::getPositioningInfo() {
    SDecorationPositioningInfo info;
    info.policy = DECORATION_POSITION_ABSOLUTE;
    info.edges  = DECORATION_EDGE_TOP | DECORATION_EDGE_BOTTOM | DECORATION_EDGE_LEFT | DECORATION_EDGE_RIGHT;

    if (g_brass->cfg.enabled->value() && g_brass->cfg.cutout->value()) {
        const auto   PX  = g_brass->cfg.pixelSize->value();
        const double M   = sc<double>((std::max<int64_t>(0, g_brass->cfg.cutoutGap->value()) + std::max<int64_t>(0, g_brass->cfg.borderSize->value())) * PX + 1);
        const double BAR = sc<double>(g_brass->cfg.barHeight->value() * PX); // the bar may be shown or not, over-damage is fine
        info.desiredExtents = {{M, M + BAR}, {M, M}};
    }
    return info;
}

void CBrassMarginDeco::damageEntire() {
    if (const auto W = m_window.lock(); W && validMapped(W))
        g_pHyprRenderer->damageWindow(W);
}

void CBrassDeco::onPositioningReply(const SDecorationPositioningReply& reply) {
    m_assignedBox = reply.assignedGeometry;
}

CBox CBrassDeco::shapeBox(const CBox& windowPx, int unit, bool withBar) const {
    if (!withBar)
        return windowPx;

    const double BAR = sc<double>(g_brass->cfg.barHeight->value()) * unit;
    return {windowPx.x, windowPx.y - BAR, windowPx.w, windowPx.h + BAR};
}

CBox CBrassDeco::barBoxGlobal() const {
    return g_pDecorationPositioner->getWindowDecorationBox(const_cast<CBrassDeco*>(this));
}

// ---------------------------------------------------------------- buttons

CBox CBrassDeco::buttonBox(size_t idx, const CBox& bar, double unit) const {
    const auto& BUTTONS = g_brass->buttons;
    const bool  RIGHT   = g_brass->cfg.buttonsAlign->value() == "right";
    const auto  PADDING = g_brass->cfg.buttonsPadding->value();
    const auto  SPACING = g_brass->cfg.buttonsSpacing->value();
    const auto  HEIGHT  = g_brass->cfg.barHeight->value();

    double      off = PADDING;
    for (size_t j = 0; j < idx; ++j)
        off += BUTTONS[j].normal->w + SPACING;

    const auto&  SPRITE = BUTTONS[idx].normal;
    const double Y      = std::floor((HEIGHT - SPRITE->h) / 2.0);
    const double X      = RIGHT ? bar.w - (off + SPRITE->w) * unit : off * unit;

    return {bar.x + X, bar.y + Y * unit, SPRITE->w * unit, SPRITE->h * unit};
}

int CBrassDeco::buttonAt(const Vector2D& barRelative) const {
    const auto BOX  = barBoxGlobal();
    const auto BAR  = CBox{0, 0, BOX.w, BOX.h};
    const auto UNIT = sc<double>(g_brass->cfg.pixelSize->value());

    for (size_t i = 0; i < g_brass->buttons.size(); ++i) {
        if (buttonBox(i, BAR, UNIT).containsPoint(barRelative))
            return sc<int>(i);
    }
    return -1;
}

// ---------------------------------------------------------------- input

bool CBrassDeco::onInputOnDeco(const eInputType type, const Vector2D& coords, std::any data) {
    if (!m_barShown || type != INPUT_TYPE_BUTTON)
        return false; // drag-start (mod + drag) etc. keep Hyprland's behavior

    const auto& E = std::any_cast<const IPointer::SButtonEvent&>(data);
    if (E.button != BTN_LEFT)
        return false;

    if (E.state != WL_POINTER_BUTTON_STATE_PRESSED) {
        finishPress();
        return true;
    }

    const auto W = owner();
    if (!W)
        return false;

    if (!focused())
        Desktop::focusState()->fullWindowFocus(W, Desktop::FOCUS_REASON_CLICK);
    if (W->isFloating())
        Desktop::windowState()->raise(W);

    const int IDX = buttonAt(coords - barBoxGlobal().pos());
    if (IDX >= 0) {
        m_pressed = m_hovered = IDX;
        damageEntire();
        return true;
    }

    const auto NOW = Time::steadyNow();
    const auto DBL = g_brass->cfg.onDoubleClick->value();
    if (!DBL.empty() && std::chrono::duration_cast<std::chrono::milliseconds>(NOW - m_lastClick).count() < 400) {
        m_lastClick = Time::steady_tp{};
        runBrassAction(DBL, W);
        return true;
    }

    m_lastClick   = NOW;
    m_dragPending = true; // the drag starts on the first motion, so plain clicks don't move the window
    return true;
}

void CBrassDeco::onMouseButton(Event::SCallbackInfo& info, const IPointer::SButtonEvent& e) {
    // presses are handled through onInputOnDeco; the release can land anywhere
    if (e.state == WL_POINTER_BUTTON_STATE_PRESSED || e.button != BTN_LEFT)
        return;

    if (m_pressed < 0 && !m_dragging && !m_dragPending)
        return;

    info.cancelled = true;
    finishPress();
}

void CBrassDeco::onMouseMove() {
    if (!m_barShown)
        return;

    if (m_dragPending) {
        m_dragPending = false;
        if (const auto W = owner(); W && validMapped(W)) {
            g_layoutManager->beginDragTarget(W->layoutTarget(), MBIND_MOVE);
            m_dragging = true;
        }
        return;
    }

    if (m_dragging)
        return;

    const auto BOX     = barBoxGlobal();
    const auto MOUSE   = g_pInputManager->getMouseCoordsInternal();
    const int  HOVERED = BOX.containsPoint(MOUSE) ? buttonAt(MOUSE - BOX.pos()) : -1;

    if (HOVERED != m_hovered) {
        m_hovered = HOVERED;
        damageEntire();
    }
}

void CBrassDeco::finishPress() {
    if (m_dragging) {
        m_dragging = false;
        g_layoutManager->endDragTarget();
    }
    m_dragPending = false;

    if (m_pressed < 0)
        return;

    const int IDX = m_pressed;
    m_pressed     = -1;
    damageEntire();

    if (IDX != m_hovered || IDX >= sc<int>(g_brass->buttons.size()))
        return;

    // copy: the action may reload the config (and clear the buttons)
    const std::string ACTION = g_brass->buttons[IDX].action;
    runBrassAction(ACTION, owner());
}

// ---------------------------------------------------------------- rendering

void CBrassDeco::draw(Render::CRenderContext& ctx, PHLMONITOR monitor, const float& a, const Render::SWindowRenderPresentation& presentation) {
    refresh();

    const auto W = owner();
    if (!W || !validMapped(W) || !W->m_ruleApplicator->decorate().valueOrDefault())
        return;

    if (!frameEnabled() && !m_barShown)
        return;

    const auto POS  = W->position(Desktop::View::IGeometric::GEOMETRIC_CURRENT) + presentation.workspaceOffset + presentation.floatingOffset - monitor->m_position;
    const auto SIZE = W->size(Desktop::View::IGeometric::GEOMETRIC_CURRENT);

    CBox       windowLogical = {POS, SIZE};
    CBox       bounds        = windowLogical;
    if (m_barShown) {
        const double BAR = sc<double>(g_brass->cfg.barHeight->value() * g_brass->cfg.pixelSize->value());
        bounds.y -= BAR;
        bounds.h += BAR;
    }

    Render::IHyprRenderer::addPassElement(ctx, makeUnique<CBrassPassElement>(CBrassPassElement::SData{this, windowLogical, bounds.expand(2), a}));
}

std::vector<UP<IPassElement>> CBrassPassElement::draw(Render::CRenderContext& ctx) {
    m_data.deco->renderPass(ctx, m_data.windowLogical, m_data.a);
    return {};
}

void CBrassDeco::renderPass(Render::CRenderContext& ctx, const CBox& windowLogical, float a) {
    const auto MON = ctx.m_data.pMonitor.lock();
    if (!MON)
        return;

    // nothing of this window is drawn where the windows above cut it
    const auto  COVER = brassCover(owner(), MON, true);
    CRegion     hole  = COVER.hole;
    ctx.m_data.renderModif.applyToRegion(hole);

    const CRegion SAVED = ctx.m_data.damage.copy();
    ctx.m_data.damage.subtract(hole);

    renderClipped(ctx, windowLogical, a, COVER.ring);

    ctx.m_data.damage = SAVED;
}

void CBrassDeco::renderClipped(Render::CRenderContext& ctx, const CBox& windowLogical, float a, const CRegion& ring) {
    const auto MON   = ctx.m_data.pMonitor.lock();
    const auto SCALE = MON->m_scale;
    const int  UNIT  = Brass::unitPx(g_brass->cfg.pixelSize->value(), SCALE);
    const int  THICK = std::max(0, sc<int>(g_brass->cfg.borderSize->value()));

    CBox       win = windowLogical;
    win.scale(SCALE).round();

    const CBox  SHAPE   = shapeBox(win, UNIT, m_barShown);
    const auto& PATTERN = Brass::fits(SHAPE, UNIT, g_brass->corner, THICK) ? g_brass->corner : NO_CORNER;
    const bool  FRAME   = frameEnabled();
    const auto  CUT     = FRAME ? Brass::cutRegion(SHAPE, UNIT, PATTERN) : CRegion{};

    if (m_barShown)
        renderBar(ctx, {SHAPE.x, SHAPE.y, SHAPE.w, win.y - SHAPE.y}, CUT, UNIT, a);

    if (!FRAME || THICK <= 0)
        return;

    const auto GOAL = configColor(focused() ? g_brass->cfg.activeColor->value() : g_brass->cfg.inactiveColor->value());
    if (m_borderColor->goal() != GOAL)
        *m_borderColor = GOAL;

    CHyprColor color = m_borderColor->value();
    color.a *= a;
    if (color.a <= 0.F)
        return;

    for (const auto& r : Brass::borderRects(SHAPE, UNIT, PATTERN, THICK))
        g_pHyprOpenGL->renderRect(ctx, r, color, {});

    // the outline also follows the holes cut by the windows above (only where this window is)
    if (ring.empty())
        return;

    CRegion inner{SHAPE};
    inner.subtract(CUT);
    CRegion edge = ring.copy().intersect(inner);
    edge.forEachRect([&](const auto& r) { g_pHyprOpenGL->renderRect(ctx, CBox{sc<double>(r.x1), sc<double>(r.y1), sc<double>(r.x2 - r.x1), sc<double>(r.y2 - r.y1)}, color, {}); });
}

void CBrassDeco::renderBar(Render::CRenderContext& ctx, const CBox& barPx, const CRegion& cut, int unit, float a) {
    const auto W = owner();
    if (barPx.w < 1 || barPx.h < 1 || !W)
        return;

    // background, minus the cut corners
    CHyprColor bg = configColor(g_brass->cfg.barColor->value());
    bg.a *= a;
    if (bg.a > 0.F) {
        CRegion region{barPx};
        region.subtract(cut);
        region.forEachRect([&](const auto& r) { g_pHyprOpenGL->renderRect(ctx, CBox{sc<double>(r.x1), sc<double>(r.y1), sc<double>(r.x2 - r.x1), sc<double>(r.y2 - r.y1)}, bg, {}); });
    }

    const bool FOCUSED = focused();
    const auto SCALE   = ctx.m_data.pMonitor->m_scale;

    // buttons (pixel art: nearest-neighbor, whole art pixels)
    const bool PREVNN             = ctx.m_data.useNearestNeighbor;
    ctx.m_data.useNearestNeighbor = true;

    double buttonsEnd = 0; // art pixels taken by the buttons, from the aligned edge
    for (size_t i = 0; i < g_brass->buttons.size(); ++i) {
        const auto& B   = g_brass->buttons[i];
        const CBox  BOX = buttonBox(i, barPx, unit);
        buttonsEnd      = std::max(buttonsEnd, (g_brass->cfg.buttonsAlign->value() == "right" ? barPx.x + barPx.w - BOX.x : BOX.x + BOX.w - barPx.x) / unit);

        if (BOX.x < barPx.x || BOX.x + BOX.w > barPx.x + barPx.w)
            continue; // window too narrow

        SP<SBrassSprite> sprite = B.normal;
        if (sc<int>(i) == m_pressed && m_pressed == m_hovered && B.pressed)
            sprite = B.pressed;
        else if (sc<int>(i) == m_hovered && m_pressed < 0 && B.hover)
            sprite = B.hover;
        else if (!FOCUSED && B.inactive)
            sprite = B.inactive;

        const auto TEX = sprite ? sprite->texture() : nullptr;
        if (!TEX)
            continue;

        g_pHyprOpenGL->renderTexture(ctx, TEX, BOX, {.a = a});
    }

    ctx.m_data.useNearestNeighbor = PREVNN;

    // title
    if (!g_brass->cfg.titleEnabled->value())
        return;

    const auto PADDING = g_brass->cfg.buttonsPadding->value();
    const bool CENTER  = g_brass->cfg.titleAlign->value() != "left";
    const bool RIGHT   = g_brass->cfg.buttonsAlign->value() == "right";
    const int  SIZEPX  = std::round(g_brass->cfg.titleSize->value() * SCALE);
    const auto RESERVE = (buttonsEnd + PADDING) * unit; // space kept free for the buttons
    const int  MAXW    = std::max(0, sc<int>(barPx.w - (CENTER ? RESERVE * 2 : RESERVE + PADDING * unit)));
    const auto TITLE   = W->metadata().title();

    if (TITLE.empty() || MAXW < 1 || SIZEPX < 1)
        return;

    if (!m_titleTex || TITLE != m_titleText || MAXW != m_titleMaxW || SIZEPX != m_titleSizePx || FOCUSED != m_titleFocused) {
        const auto COLOR = configColor(FOCUSED ? g_brass->cfg.titleColor->value() : g_brass->cfg.titleInactiveColor->value());
        m_titleTex       = g_pHyprRenderer->renderText(TITLE, COLOR, SIZEPX, false, g_brass->cfg.titleFont->value(), MAXW);
        m_titleText      = TITLE;
        m_titleMaxW      = MAXW;
        m_titleSizePx    = SIZEPX;
        m_titleFocused   = FOCUSED;
    }

    if (!m_titleTex || m_titleTex->m_size.x < 1)
        return;

    const auto TSIZE = m_titleTex->m_size;
    double     x     = 0;
    if (CENTER)
        x = barPx.x + (barPx.w - TSIZE.x) / 2.0;
    else
        x = RIGHT ? barPx.x + PADDING * unit : barPx.x + RESERVE;

    CBox box = {x, barPx.y + (barPx.h - TSIZE.y) / 2.0, TSIZE.x, TSIZE.y};
    box.round(); // whole pixels, otherwise the (pixel) font gets resampled

    g_pHyprOpenGL->renderTexture(ctx, m_titleTex, box, {.a = a});
}

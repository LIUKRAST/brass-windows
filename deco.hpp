#pragma once

#define WLR_USE_UNSTABLE

#include <hyprland/src/render/decorations/IHyprWindowDecoration.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/devices/IPointer.hpp>
#include <hyprland/src/helpers/AnimatedVariable.hpp>
#include <hyprland/src/helpers/signal/Signal.hpp>
#include <hyprland/src/helpers/time/Time.hpp>

#include "globals.hpp"

namespace Event {
    struct SCallbackInfo;
}

// One per window. Draws (above the window surface):
//  - the title bar, only for floating windows (it reserves space on top)
//  - the focus outline, on top of the window contents
// The corners of the window itself are cut in the surface render hook (main.cpp).
class CBrassMarginDeco;

class CBrassDeco : public IHyprWindowDecoration {
  public:
    CBrassDeco(PHLWINDOW);
    virtual ~CBrassDeco();

    virtual SDecorationPositioningInfo getPositioningInfo();
    virtual void                       onPositioningReply(const SDecorationPositioningReply& reply);
    virtual void                       draw(Render::CRenderContext& ctx, PHLMONITOR monitor, const float& a, const Render::SWindowRenderPresentation& presentation);
    virtual eDecorationType            getDecorationType();
    virtual void                       updateWindow(PHLWINDOW);
    virtual void                       damageEntire();
    virtual bool                       onInputOnDeco(const eInputType, const Vector2D&, std::any = {});
    virtual eDecorationLayer           getDecorationLayer();
    virtual uint64_t                   getDecorationFlags();
    virtual std::string                getDisplayName();

    PHLWINDOW                          owner() const;

    // whether the corners of this window get cut / outlined at all
    bool frameEnabled() const;
    // whether the bar is currently part of the window's shape
    bool barShown() const;

    // full shape (bar + window) in monitor pixels, from the window box in monitor pixels
    CBox shapeBox(const CBox& windowPx, int unit, bool withBar) const;

    // re-evaluates floating / fullscreen / rules; repositions if the bar appears or disappears
    void refresh();
    void updateRules();
    void onConfigReloaded();
    void onFocusChanged();

    void renderPass(Render::CRenderContext& ctx, const CBox& windowLogical, float a);

    WP<CBrassDeco>       m_self;
    WP<CBrassMarginDeco> m_margin;

  private:
    PHLWINDOWREF           m_window;
    CBox                   m_assignedBox;

    bool                   m_barShown  = false;
    bool                   m_ruleNoBar = false, m_ruleNoFrame = false;

    PHLANIMVAR<CHyprColor> m_borderColor;

    // title cache
    SP<Render::ITexture>   m_titleTex;
    std::string            m_titleText;
    int                    m_titleMaxW = -1, m_titleSizePx = -1;
    bool                   m_titleFocused = false;

    // input
    int                    m_hovered  = -1;
    int                    m_pressed  = -1;
    bool                   m_dragging = false;
    bool                   m_dragPending = false;
    Time::steady_tp        m_lastClick = Time::steadyNow();

    CHyprSignalListener    m_mouseButtonListener;
    CHyprSignalListener    m_mouseMoveListener;

    void                   readRules();
    bool                   wantsBar() const;
    bool                   focused() const;
    CBox                   barBoxGlobal() const;
    int                    buttonAt(const Vector2D& barRelative) const;
    // button idx inside `bar`, where one art pixel is `unit` (logical px for hit tests, monitor px for drawing)
    CBox                   buttonBox(size_t idx, const CBox& bar, double unit) const;
    void                   onMouseButton(Event::SCallbackInfo& info, const IPointer::SButtonEvent& e);
    void                   onMouseMove();
    void                   finishPress();

    void                   renderClipped(Render::CRenderContext& ctx, const CBox& windowLogical, float a, const CRegion& ring);
    void                   renderBar(Render::CRenderContext& ctx, const CBox& barPx, const CRegion& cut, int unit, float a);
};

// Draws nothing. Its (non reserved) extents grow the window's damage box by
// the cutout gap + ring, so moving / closing the window repaints the gap it
// cut into the windows below.
class CBrassMarginDeco : public IHyprWindowDecoration {
  public:
    CBrassMarginDeco(PHLWINDOW w) : IHyprWindowDecoration(w), m_window(w) {}
    virtual ~CBrassMarginDeco() = default;

    virtual SDecorationPositioningInfo getPositioningInfo();
    virtual void                       onPositioningReply(const SDecorationPositioningReply&) {}
    virtual void                       draw(Render::CRenderContext&, PHLMONITOR, const float&, const Render::SWindowRenderPresentation&) {}
    virtual eDecorationType            getDecorationType() {
        return DECORATION_CUSTOM;
    }
    virtual void updateWindow(PHLWINDOW) {}
    virtual void damageEntire();
    virtual bool onInputOnDeco(const eInputType, const Vector2D&, std::any = {}) {
        return false;
    }
    virtual eDecorationLayer getDecorationLayer() {
        return DECORATION_LAYER_BOTTOM;
    }
    virtual uint64_t getDecorationFlags() {
        return DECORATION_NON_SOLID;
    }
    virtual std::string getDisplayName() {
        return "BrassWindows margin";
    }

  private:
    PHLWINDOWREF m_window;
};

class CBrassPassElement : public IPassElement {
  public:
    struct SData {
        CBrassDeco* deco = nullptr;
        CBox        windowLogical; // monitor-local logical box of the window surface
        CBox        bounds;        // monitor-local logical bounding box (window + bar)
        float       a = 1.F;
    };

    CBrassPassElement(const SData& data) : m_data(data) {}
    virtual ~CBrassPassElement() = default;

    virtual std::vector<UP<IPassElement>> draw(Render::CRenderContext& ctx);
    virtual bool                          needsLiveBlur(Render::CRenderContext&) {
        return false;
    }
    virtual bool needsPrecomputeBlur(Render::CRenderContext&) {
        return false;
    }
    virtual std::optional<CBox> boundingBox(Render::CRenderContext&) {
        return m_data.bounds;
    }
    virtual const char* passName() {
        return "CBrassPassElement";
    }
    virtual ePassElementType type() {
        return EK_CUSTOM;
    }

  private:
    SData m_data;
};

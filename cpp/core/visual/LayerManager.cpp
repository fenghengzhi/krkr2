//---------------------------------------------------------------------------
/*
        TVP2 ( T Visual Presenter 2 )  A script authoring tool
        Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

        See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Layer Management
//---------------------------------------------------------------------------

#include "tjsCommHead.h"
#include <spdlog/spdlog.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include "tjsArray.h"
#include "LayerManager.h"
#include "MsgIntf.h"
#include "LayerBitmapIntf.h"
#include "StorageIntf.h"
#include "EventIntf.h"
#include "SysInitIntf.h"
#include "TickCount.h"
#include "DebugIntf.h"
#include "LayerTreeOwner.h"

static bool lowLevelLogoTraceEnabled() {
#ifdef __EMSCRIPTEN__
    return EM_ASM_INT({
               try {
                   if(typeof window !== 'undefined' &&
                      window.__KRKR_TRACE_LOGO_CHAIN__) {
                       return 1;
                   }
                   const params = new URLSearchParams(window.location.search);
                   const traceParam = params.get('trace') || "";
                   return params.has('traceLogoChain') ||
                          traceParam === 'logo' ||
                          traceParam.split(',').includes('logo')
                              ? 1
                              : 0;
               } catch(e) {
                   return 0;
               }
           }) != 0;
#else
    return false;
#endif
}

//---------------------------------------------------------------------------
// tTVPLayerManager
//---------------------------------------------------------------------------
tTVPLayerManager::tTVPLayerManager(iTVPLayerTreeOwner *owner) {
    // Four-reference note: do not value-initialize the whole object here.
    // ReleaseTouchCaptureIDMark and ReleaseCaptureCalled are deliberately
    // absent from this initialization sequence in every native target.
    RefCount = 1;
    LayerTreeOwner = owner;
    DrawDeviceData = nullptr;
    DrawBuffer = nullptr;
    DesiredLayerType = ltOpaque;

    CaptureOwner = nullptr;
    LastMouseMoveSent = nullptr;
    Primary = nullptr;
    FocusedLayer = nullptr;
    OverallOrderIndexValid = false;
    EnabledWorkRefCount = 0;
    FocusChangeLock = false;
    VisualStateChanged = true;
    LastMouseMoveX = -1;
    LastMouseMoveY = -1;
    InNotifyingHintOrCursorChange = false;
}
//---------------------------------------------------------------------------
tTVPLayerManager::~tTVPLayerManager() {
    // The native destructor owns only DrawBuffer plus the automatic member
    // storage destroyed after this body. It does not detach/release/clear the
    // raw owner, DrawDeviceData, primary/focus/capture pointers, or pointees in
    // TouchCapture, ModalLayerVector, and AllNodes. Normal BaseLayer teardown
    // is responsible for the detach/unregister path before the final Release.
    // Deleting DrawBuffer immediately destroys its wrapper, while the wrapped
    // iTVPTexture2D follows the renderer's deferred-release queue.
    if(DrawBuffer)
        delete DrawBuffer;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::AddRef() {
    // Deliberately plain and non-atomic; there is no saturation/lifetime guard.
    RefCount++;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::Release() {
    // Exact native boundary: the final path enters delete this while RefCount
    // is still 1. It neither stores zero nor performs a BeforeDestruction-style
    // hook/recheck. The non-final path has no zero guard, so count 0 follows the
    // same decrement expression rather than being treated as already dead.
    if(RefCount == 1)
        delete this;
    else
        RefCount--;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::RegisterSelfToWindow() {
    LayerTreeOwner->RegisterLayerManager(this);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::UnregisterSelfFromWindow() {
    LayerTreeOwner->UnregisterLayerManager(this);
}

void tTVPLayerManager::SetHoldAlpha(bool b) {
    HoldAlpha = b;
    if(!DrawBuffer)
        return;
    static_cast<tTVPDestTexture *>(DrawBuffer)->SetHoldAlpha(b);
}

//---------------------------------------------------------------------------
tTVPBaseTexture *tTVPLayerManager::GetDrawTargetBitmap(const tTVPRect &rect,
                                                       tTVPRect &cliprect) {
    // retrieve draw target bitmap
    tjs_int w = rect.get_width();
    tjs_int h = rect.get_height();

    if(!DrawBuffer) {
        // create draw buffer
        if(Primary) {
            const tTVPRect &rc = Primary->GetRect();
            w = rc.get_width();
            h = rc.get_height();
        }
        DrawBuffer = new tTVPDestTexture(w, h);
        DrawBuffer->Fill(tTVPRect(0, 0, w, h), 0xFF000000);
        static_cast<tTVPDestTexture *>(DrawBuffer)->SetHoldAlpha(HoldAlpha);
    } else {
        tjs_int bw = DrawBuffer->GetWidth();
        tjs_int bh = DrawBuffer->GetHeight();
        if(bw < w || bh < h) {
            // insufficient size; resize the draw buffer
            tjs_uint neww = bw > w ? bw : w, newh = bh > h ? bh : h;
            neww += (neww & 1); // align to even
            DrawBuffer->SetSize(neww, newh, false);
            DrawBuffer->Fill(tTVPRect(0, 0, neww, newh), 0xFF000000);
        }
    }

    cliprect = rect;
    return DrawBuffer;
}
//---------------------------------------------------------------------------
tTVPLayerType tTVPLayerManager::GetTargetLayerType() {
    return DesiredLayerType;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::DrawCompleted(const tTVPRect &destrect,
                                     tTVPBaseTexture *bmp,
                                     const tTVPRect &cliprect,
                                     tTVPLayerType type, tjs_int opacity) {
#if 0
	if (!LayerTreeOwner) return;
	LayerTreeOwner->NotifyBitmapCompleted(this, destrect.left, destrect.top, bmp, cliprect, type, opacity);
#else
    tjs_int w, h;
    if(!/*LayerTreeOwner->*/ GetPrimaryLayerSize(w, h))
        return;
    if(lowLevelLogoTraceEnabled()) {
        spdlog::get("core")->info(
            "WCHAIN stage=manager.drawCompleted manager={} primary={} "
            "destRect=[{},{},{},{}] clipRect=[{},{},{},{}] type={} opacity={} "
            "bmp={} bmpSize={}x{}",
            (void *)this, (void *)Primary, destrect.left, destrect.top,
            destrect.right, destrect.bottom, cliprect.left, cliprect.top,
            cliprect.right, cliprect.bottom, (int)type, opacity, (void *)bmp,
            bmp ? bmp->GetWidth() : 0, bmp ? bmp->GetHeight() : 0);
    }
    // Window->GetDrawDevice()->GetSrcSize(w, h);
    if(!DrawBuffer) {
        // create draw buffer
        DrawBuffer = new tTVPDestTexture(w, h);
        DrawBuffer->Fill(tTVPRect(0, 0, w, h), 0xFF000000);
        static_cast<tTVPDestTexture *>(DrawBuffer)->SetHoldAlpha(HoldAlpha);
    } else {
        tjs_int bw = DrawBuffer->GetWidth();
        tjs_int bh = DrawBuffer->GetHeight();
        if(bw < w || bh < h) {
            // insufficient size; resize the draw buffer
            tjs_uint neww = bw > w ? bw : w, newh = bh > h ? bh : h;
            neww += (neww & 1); // align to even
            DrawBuffer->SetSize(neww, newh, false);
            DrawBuffer->Fill(tTVPRect(0, 0, neww, newh), 0xFF000000);
        }
    }

    DrawBuffer->Blt(destrect.left, destrect.top, bmp, cliprect, type, opacity,
                    HoldAlpha);
#endif
}

tTVPBaseTexture *tTVPLayerManager::GetOrCreateDrawBuffer() {
    if(!DrawBuffer) {
        tjs_int w, h;
        if(!GetPrimaryLayerSize(w, h))
            return nullptr;
        DrawBuffer = new tTVPDestTexture(w, h);
        DrawBuffer->Fill(tTVPRect(0, 0, w, h), 0xFF000000);
        static_cast<tTVPDestTexture *>(DrawBuffer)->SetHoldAlpha(HoldAlpha);
    }
    return DrawBuffer;
}

//---------------------------------------------------------------------------
void tTVPLayerManager::AttachPrimary(tTJSNI_BaseLayer *pri) {
    // attach primary layer to the manager
    DetachPrimary();

    if(!Primary) {
        Primary = pri;
        EnabledWorkRefCount = 0;
        OverallOrderIndexValid = false;
        UpdateRegion.Clear();
        pri->SetVisible(true);
        pri->SetOpacity(255);
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::DetachPrimary() {
    // detach primary layer from the manager
    if(Primary) {
        // The reference implementations make this ordering observable and do
        // not provide a rollback guard around it.  In particular, Primary is
        // read for NotifyPart only after the focus/capture/touch/mouse calls,
        // so reentry from those calls can replace the layer that is parted.
        SetFocusTo(nullptr);
        ReleaseCapture();
        ReleaseTouchCaptureAll();
        ForceMouseLeave();
        // Function-argument evaluation snapshots this current Primary once;
        // NotifyPart uses the same value for both of its tree cleanups.  The
        // manager slot is cleared only after both return normally.
        NotifyPart(Primary);
        Primary = nullptr;
    }
}
//---------------------------------------------------------------------------
bool tTVPLayerManager::GetPrimaryLayerSize(tjs_int &w, tjs_int &h) const {
    if(IsPrimaryLayerAttached()) {
        w = Primary->GetWidth();
        h = Primary->GetHeight();
        return true;
    } else {
        return false;
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::NotifyPart(tTJSNI_BaseLayer *lay) {
    // notifies layer parting from its parent
    // Keep the invalidation before either potentially reentrant tree walk.
    // There is no local exception recovery: a failing BlurTree skips capture
    // cleanup, while either failure leaves the caller's Primary slot uncleared.
    InvalidateOverallIndex();
    BlurTree(lay);
    ReleaseCaptureFromTree(lay);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::InvalidateOverallIndex() {
    OverallOrderIndexValid = false;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::RecreateOverallOrderIndex() {
    // recreate overall order index
    if(!OverallOrderIndexValid) {
        tjs_uint index = 0;
        AllNodes.clear();
        if(Primary)
            Primary->RecreateOverallOrderIndex(index, AllNodes);

        OverallOrderIndexValid = true;
    }
}
//---------------------------------------------------------------------------
std::vector<tTJSNI_BaseLayer *> &tTVPLayerManager::GetAllNodes() {
    if(!OverallOrderIndexValid)
        RecreateOverallOrderIndex();
    return AllNodes;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::QueryUpdateExcludeRect() {
    if(!VisualStateChanged)
        return;
    tTVPRect r;
    r.clear();
    if(Primary)
        Primary->QueryUpdateExcludeRect(r, true);
    VisualStateChanged = false;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::NotifyMouseCursorChange(tTJSNI_BaseLayer *layer,
                                               tjs_int cursor) {
    if(InNotifyingHintOrCursorChange)
        return;

    InNotifyingHintOrCursorChange = true;
    try {

        tTJSNI_BaseLayer *l;

        if(CaptureOwner)
            l = CaptureOwner;
        else
            l = GetMostFrontChildAt(LastMouseMoveX, LastMouseMoveY);

        if(l == layer)
            SetMouseCursor(cursor);
    } catch(...) {
        InNotifyingHintOrCursorChange = false;
        throw;
    }

    InNotifyingHintOrCursorChange = false;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::SetMouseCursor(tjs_int cursor) {
    if(!LayerTreeOwner)
        return;

    LayerTreeOwner->SetMouseCursor(this, cursor);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::GetCursorPos(tjs_int &x, tjs_int &y) {
    if(!LayerTreeOwner)
        return;
    LayerTreeOwner->GetCursorPos(this, x, y);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::SetCursorPos(tjs_int x, tjs_int y) {
    if(!LayerTreeOwner)
        return;
    LayerTreeOwner->SetCursorPos(this, x, y);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::NotifyHintChange(tTJSNI_BaseLayer *layer,
                                        const ttstr &hint) {
    if(InNotifyingHintOrCursorChange)
        return;

    InNotifyingHintOrCursorChange = true;

    try {
        tTJSNI_BaseLayer *l;

        if(CaptureOwner)
            l = CaptureOwner;
        else
            l = GetMostFrontChildAt(LastMouseMoveX, LastMouseMoveY);

        if(l == layer)
            SetHint(l->GetOwnerNoAddRef(), hint);
    } catch(...) {
        InNotifyingHintOrCursorChange = false;
        throw;
    }

    InNotifyingHintOrCursorChange = false;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::SetHint(iTJSDispatch2 *sender, const ttstr &hint) {
    if(!LayerTreeOwner)
        return;
    LayerTreeOwner->SetHint(this, sender, hint);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::NotifyLayerResize() {
    // notifies layer resizing to the LayerTreeOwner
    if(!LayerTreeOwner)
        return;

    LayerTreeOwner->NotifyLayerResize(this);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::NotifyWindowInvalidation() {
    // notifies layer surface is invalidated and should be transfered
    // to LayerTreeOwner.
    if(!LayerTreeOwner)
        return;
    if(lowLevelLogoTraceEnabled()) {
        const tjs_int count = UpdateRegion.GetCount();
        if(count) {
            const auto &bound = UpdateRegion.GetBound();
            spdlog::get("core")->info(
                "WCHAIN stage=manager.notifyWindowInvalidation manager={} "
                "primary={} updateCount={} updateBound=[{},{},{},{}]",
                (void *)this, (void *)Primary, count, bound.left, bound.top,
                bound.right, bound.bottom);
        } else {
            spdlog::get("core")->info(
                "WCHAIN stage=manager.notifyWindowInvalidation manager={} "
                "primary={} updateCount=0",
                (void *)this, (void *)Primary);
        }
    }

    LayerTreeOwner->NotifyLayerImageChange(this);
    // TODO atlernative of LayerTreeOwner->RequestUpdate();
}
//---------------------------------------------------------------------------
void tTVPLayerManager::SetLayerTreeOwner(class iTVPLayerTreeOwner *owner) {
    // sets LayerTreeOwner
    LayerTreeOwner = owner;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::NotifyResizeFromWindow(tjs_uint w, tjs_uint h) {
    // is called by the owner window, notifies windows's client area
    // size has been changed. does not be called if owner window's
    // "autoResize" property is false.

    // currently this function is not used.

    if(Primary)
        Primary->InternalSetSize(w, h);
}
//---------------------------------------------------------------------------
tTJSNI_BaseLayer *tTVPLayerManager::GetMostFrontChildAt(
    tjs_int x, tjs_int y, tTJSNI_BaseLayer *except, bool get_disabled) {
    // return most front layer at given point.
    // this does checking of layer's visibility.
    // x and y are given in primary layer's coordinates.
    if(!Primary)
        return nullptr;

    tTJSNI_BaseLayer *lay = nullptr;
    Primary->GetMostFrontChildAt(x, y, &lay, except, get_disabled);
    return lay;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryClick(tjs_int x, tjs_int y) {
    tTJSNI_BaseLayer *l = GetMostFrontChildAt(x, y);
    if(l && CaptureOwner == l) {
        l->FromPrimaryCoordinates(x, y);
        l->FireClick(x, y);
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryDoubleClick(tjs_int x, tjs_int y) {
    tTJSNI_BaseLayer *l = GetMostFrontChildAt(x, y);
    if(l /*&& CaptureOwner == l*/) {
        l->FromPrimaryCoordinates(x, y);
        l->FireDoubleClick(x, y);
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryMouseDown(tjs_int x, tjs_int y,
                                        tTVPMouseButton mb, tjs_uint32 flags) {
    PrimaryMouseMove(x, y, flags);
    tTJSNI_BaseLayer *l =
        CaptureOwner ? CaptureOwner : GetMostFrontChildAt(x, y);
    if(l) {
        l->FromPrimaryCoordinates(x, y);
        ReleaseCaptureCalled = false;
        l->FireMouseDown(x, y, mb, flags);
        bool no_capture = ReleaseCaptureCalled;

        if(CaptureOwner != l) {
            ReleaseCapture();

            if(!no_capture) {
                CaptureOwner = l;
                if(CaptureOwner->Owner)
                    CaptureOwner->Owner->AddRef(); // addref TJS object
            }
        }

        SetHint(nullptr, ttstr());
    } else {
        ReleaseCapture();
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryMouseUp(tjs_int x, tjs_int y, tTVPMouseButton mb,
                                      tjs_uint32 flags) {
    tTJSNI_BaseLayer *l;

    if(CaptureOwner)
        l = CaptureOwner;
    else
        l = GetMostFrontChildAt(x, y);

    if(l) {
        int orig_x = x, orig_y = y;

        l->FromPrimaryCoordinates(x, y);
        l->FireMouseUp(x, y, mb, flags);

        if(!TVPIsAnyMouseButtonPressedInShiftStateFlags(flags)) {
            ReleaseCapture();
            PrimaryMouseMove(orig_x, orig_y,
                             flags); // force recheck current under-cursor layer
        }
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryMouseMove(tjs_int x, tjs_int y,
                                        tjs_uint32 flags) {
    bool poschanged = (LastMouseMoveX != x || LastMouseMoveY != y);
    LastMouseMoveX = x;
    LastMouseMoveY = y;

    tTJSNI_BaseLayer *l;

    if(CaptureOwner)
        l = CaptureOwner;
    else
        l = GetMostFrontChildAt(x, y);

    // enter/leave event
    if(LastMouseMoveSent != l) {
        if(LastMouseMoveSent)
            LastMouseMoveSent->FireMouseLeave();

        // recheck l because the layer may become invalid during
        // FireMouseLeave call.
        if(CaptureOwner)
            l = CaptureOwner;
        else
            l = GetMostFrontChildAt(x, y);

        if(l) {
            InNotifyingHintOrCursorChange = true;
            try {
                tTJSNI_BaseLayer *ll;

                l->FireMouseEnter();

                // recheck l because the layer may become invalid
                // during FireMouseEnter call.
                if(CaptureOwner)
                    ll = CaptureOwner;
                else
                    ll = GetMostFrontChildAt(x, y);

                if(l != ll) {
                    l->FireMouseLeave();
                    l = ll;
                    if(l)
                        l->FireMouseEnter();
                }

                // note: rechecking is done only once to avoid
                // infinite loop

                if(l)
                    l->SetCurrentCursorToWindow();
                if(l)
                    l->SetCurrentHintToWindow();
            } catch(...) {
                InNotifyingHintOrCursorChange = false;
                throw;
            }
            InNotifyingHintOrCursorChange = false;
        }

        if(!l) {
            SetMouseCursor(0);
            SetHint(nullptr, ttstr());
        }
    }

    if(LastMouseMoveSent != l) {
        if(LastMouseMoveSent) {
            tTJSNI_BaseLayer *lay = LastMouseMoveSent;
            LastMouseMoveSent = nullptr;
            if(lay->Owner)
                lay->Owner->Release();
        }

        LastMouseMoveSent = l;

        if(LastMouseMoveSent) {
            if(LastMouseMoveSent->Owner)
                LastMouseMoveSent->Owner->AddRef();
        }
    }

    if(l) {
        if(poschanged) {
            l->FromPrimaryCoordinates(x, y);
            l->FireMouseMove(x, y, flags);
        }
    } else {
        // no layer to send the event
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryTouchDown(tjs_real x, tjs_real y, tjs_real cx,
                                        tjs_real cy, tjs_uint32 id) {
    tjs_int ix = (tjs_int)x, iy = (tjs_int)y;
    ReleaseTouchCapture(id);
    tTJSNI_BaseLayer *l = GetMostFrontChildAt(ix, iy);
    if(l) {
        l->FromPrimaryCoordinates(x, y);
        ReleaseTouchCaptureIDMark = (tjs_int64)id;
        l->FireTouchDown(x, y, cx, cy, id);
        if(ReleaseTouchCaptureIDMark == (tjs_int64)id) {
            SetTouchCapture(id, l);
        }
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryTouchUp(tjs_real x, tjs_real y, tjs_real cx,
                                      tjs_real cy, tjs_uint32 id) {
    tjs_int ix = (tjs_int)x, iy = (tjs_int)y;
    tTJSNI_BaseLayer *l =
        GetTouchCapture(id) ? GetTouchCapture(id) : GetMostFrontChildAt(ix, iy);
    if(l) {
        l->FromPrimaryCoordinates(x, y);
        l->FireTouchUp(x, y, cx, cy, id);
        ReleaseTouchCapture(id);
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryTouchMove(tjs_real x, tjs_real y, tjs_real cx,
                                        tjs_real cy, tjs_uint32 id) {
    tjs_int ix = (tjs_int)x, iy = (tjs_int)y;
    tTJSNI_BaseLayer *l =
        GetTouchCapture(id) ? GetTouchCapture(id) : GetMostFrontChildAt(ix, iy);
    if(l) {
        l->FromPrimaryCoordinates(x, y);
        l->FireTouchMove(x, y, cx, cy, id);
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryTouchScaling(tjs_real startdist, tjs_real curdist,
                                           tjs_real cx, tjs_real cy,
                                           tjs_int flag) {
    if(FocusedLayer)
        FocusedLayer->FireTouchScaling(startdist, curdist, cx, cy, flag);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryTouchRotate(tjs_real startangle,
                                          tjs_real curangle, tjs_real dist,
                                          tjs_real cx, tjs_real cy,
                                          tjs_int flag) {
    if(FocusedLayer)
        FocusedLayer->FireTouchRotate(startangle, curangle, dist, cx, cy, flag);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryMultiTouch() {
    if(FocusedLayer)
        FocusedLayer->FireMultiTouch();
}
//---------------------------------------------------------------------------
void tTVPLayerManager::ForceMouseLeave() {
    if(LastMouseMoveSent) {
        // Clear the manager slot before firing the callback, but deliberately
        // reload the saved layer's Owner afterward.  If FireMouseLeave throws,
        // that Owner reference is not released here.
        tTJSNI_BaseLayer *lay = LastMouseMoveSent;
        LastMouseMoveSent = nullptr;
        lay->FireMouseLeave();
        if(lay->Owner)
            lay->Owner->Release();
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::ForceMouseRecheck() {
    PrimaryMouseMove(LastMouseMoveX, LastMouseMoveY, 0);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::MouseOutOfWindow() {
    // notifys that the mouse cursor goes outside of the window.
    if(!CaptureOwner)
        PrimaryMouseMove(-1, -1,
                         0); // force mouse cursor out of the all
}
//---------------------------------------------------------------------------
void tTVPLayerManager::LeaveMouseFromTree(tTJSNI_BaseLayer *root) {
    // force to leave mouse
    if(LastMouseMoveSent) {
        if(LastMouseMoveSent->IsAncestorOrSelf(root)) {
            tTJSNI_BaseLayer *lay = LastMouseMoveSent;
            LastMouseMoveSent = nullptr;
            lay->FireMouseLeave();
            if(lay->Owner)
                lay->Owner->Release();
        }
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::ReleaseCapture() {
    // release capture state
    ReleaseCaptureCalled = true;
    if(CaptureOwner) {
        // CaptureOwner is a raw layer pointer whose dispatch Owner supplies the
        // held reference.  Publish the null capture before dropping that
        // reference, then notify the current LayerTreeOwner; there is no local
        // rollback if either operation escapes.
        tTJSNI_BaseLayer *lay = CaptureOwner;
        CaptureOwner = nullptr;
        if(lay->Owner)
            lay->Owner->Release();
        // release TJS object

        LayerTreeOwner->ReleaseMouseCapture(this);
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::ReleaseCaptureFromTree(tTJSNI_BaseLayer *layer) {
    // Release capture state, if the capture object is descendant of
    // 'layer' or 'layer' itself.
    if(CaptureOwner) {
        if(CaptureOwner->IsAncestorOrSelf(layer)) {
            ReleaseCapture();
        }
    }
    std::vector<tTVPTouchCaptureLayer>::iterator itr = TouchCapture.begin();
    while(itr != TouchCapture.end()) {
        tTJSNI_BaseLayer *l = itr->Owner;
        if(l && l->IsAncestorOrSelf(layer)) {
            // This intentionally walks the live vector.  Owner::Release comes
            // before both the marker comparison and erase; reentry that mutates
            // TouchCapture can therefore invalidate itr, and an exception
            // leaves the still-present entry without rollback.
            if(l->Owner)
                l->Owner->Release();
            if(ReleaseTouchCaptureIDMark == (tjs_int64)(itr->TouchID))
                ReleaseTouchCaptureIDMark = -1;
            itr = TouchCapture.erase(itr);
        } else {
            itr++;
        }
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::ReleaseTouchCapture(tjs_uint32 id) {
    FindTouchID pred(id);
    std::vector<tTVPTouchCaptureLayer>::iterator itr =
        std::find_if(TouchCapture.begin(), TouchCapture.end(), pred);
    if(itr != TouchCapture.end()) {
        tTJSNI_BaseLayer *old = itr->Owner;
        if(old && old->Owner)
            old->Owner->Release();
        TouchCapture.erase(itr);
    }
    if(ReleaseTouchCaptureIDMark == (tjs_int64)id)
        ReleaseTouchCaptureIDMark = -1;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::ReleaseTouchCaptureAll() {
    // Do not snapshot or erase per item.  The native implementations release
    // through the live vector (re-reading its end after a Release), then clear
    // the vector and marker only on the normal tail.  A partial failure thus
    // leaves every entry present, including those whose Owner was released.
    for(std::vector<tTVPTouchCaptureLayer>::iterator itr = TouchCapture.begin();
        itr != TouchCapture.end(); itr++) {
        tTJSNI_BaseLayer *l = itr->Owner;
        if(l->Owner)
            l->Owner->Release();
    }
    TouchCapture.clear();
    ReleaseTouchCaptureIDMark = -1;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::SetTouchCapture(tjs_uint32 id, tTJSNI_BaseLayer *layer) {
    FindTouchID pred(id);
    std::vector<tTVPTouchCaptureLayer>::iterator itr =
        std::find_if(TouchCapture.begin(), TouchCapture.end(), pred);
    if(itr != TouchCapture.end()) {
        // 既に同一IDのものがある場合は、同じ場所で置き換える
        tTJSNI_BaseLayer *old = itr->Owner;
        if(old && old->Owner)
            old->Owner->Release();
        itr->Owner = layer;
        if(layer->Owner)
            layer->Owner->AddRef();
    } else {
        // ない場合は、末尾に追加。
        TouchCapture.emplace_back(id, layer);
        if(layer->Owner)
            layer->Owner->AddRef();
    }
}
//---------------------------------------------------------------------------
bool tTVPLayerManager::BlurTree(tTJSNI_BaseLayer *root) {
    // (primary only) remove focus from "root"
    RemoveTreeModalState(root);
    LeaveMouseFromTree(root);

    if(!FocusedLayer)
        return false;

    if(!FocusedLayer->IsAncestorOrSelf(root))
        return false;
    // root is not ancestor of current focused layer

    tTJSNI_BaseLayer *next = root->GetNextFocusable();

    if(next != FocusedLayer)
        SetFocusTo(next,
                   true); // focus to root's next focusable layer
    else
        SetFocusTo(nullptr, true);

    return true;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::CheckTreeFocusableState(tTJSNI_BaseLayer *root) {
    // (primary only) check newly added tree's focusable state
    /*	// uncomment here to auto-focus
            if(FocusedLayer) return;

            tTJSNI_BaseLayer *lay = root->SearchFirstFocusable(true);
            if(lay) SetFocusTo(lay, true);
    */
}
//---------------------------------------------------------------------------
tTJSNI_BaseLayer *tTVPLayerManager::FocusPrev() {
    // focus to previous layer
    tTJSNI_BaseLayer *l;
    if(!FocusedLayer)
        l = SearchFirstFocusable(false); // search first focusable layer
    else
        l = FocusedLayer->GetPrevFocusable();

    if(l)
        SetFocusTo(l, false);
    return l;
}
//---------------------------------------------------------------------------
tTJSNI_BaseLayer *tTVPLayerManager::FocusNext() {
    // focus to next layer
    tTJSNI_BaseLayer *l;
    if(!FocusedLayer)
        l = SearchFirstFocusable(false); // search first focusable layer
    else
        l = FocusedLayer->GetNextFocusable();

    if(l)
        SetFocusTo(l, true);
    return l;
}
//---------------------------------------------------------------------------
tTJSNI_BaseLayer *
tTVPLayerManager::SearchFirstFocusable(bool ignore_chain_focusable) {
    // (primary only) search first focusable layer
    if(!Primary)
        return nullptr;
    tTJSNI_BaseLayer *lay =
        Primary->SearchFirstFocusable(ignore_chain_focusable);

    return lay;
}
//---------------------------------------------------------------------------
bool tTVPLayerManager::SetFocusTo(tTJSNI_BaseLayer *layer, bool direction) {
    // set focus to layer

    // direction = true : forward focus
    // direction = false: backward focus

    if(layer && !layer->GetNodeFocusable())
        return false;

    if(layer && !layer->Shutdown)
        layer = layer->FireBeforeFocus(FocusedLayer, direction);

    if(layer && !layer->GetNodeFocusable())
        return false;

    if(FocusedLayer == layer)
        return false;

    if(FocusChangeLock)
        TVPThrowExceptionMessage(TVPCannotChangeFocusInProcessingFocus);
    FocusChangeLock = true;

    tTJSNI_BaseLayer *org = FocusedLayer;
    FocusedLayer = layer;

    try {
        // The new focus is only a raw published pointer during these callbacks;
        // its dispatch Owner is AddRef'd afterward.  Both the normal and catch
        // paths use the callback-reloaded FocusedLayer, then Release org.
        if(org && !org->Shutdown)
            org->FireBlur(layer);

        if(FocusedLayer && !FocusedLayer->Shutdown)
            FocusedLayer->FireFocus(org, direction);
    } catch(...) {
        if(FocusedLayer)
            if(FocusedLayer->Owner)
                FocusedLayer->Owner->AddRef();
        if(org)
            if(org->Owner)
                org->Owner->Release();
        FocusChangeLock = false;
        throw;
    }

    if(FocusedLayer)
        if(FocusedLayer->Owner)
            FocusedLayer->Owner->AddRef();
    if(org)
        if(org->Owner)
            org->Owner->Release();

    // These owner notifications are deliberately outside the catch above.
    // If one throws, FocusChangeLock remains set, matching the references.
    if(FocusedLayer)
        SetImeModeOf(FocusedLayer);
    else
        ResetImeMode();
    if(FocusedLayer)
        SetAttentionPointOf(FocusedLayer);
    else
        DisableAttentionPoint();

    FocusChangeLock = false;
    return true;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::ReleaseAllModalLayer() {
    // (primary only) release all modal layer on invalidation
    std::vector<tTJSNI_BaseLayer *> copy(ModalLayerVector);
    ModalLayerVector.clear();

    std::vector<tTJSNI_BaseLayer *>::iterator i;
    for(i = copy.begin(); i < copy.end(); i++) {
        if((*i)->Owner)
            (*i)->Owner->Release();
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::SetModeTo(tTJSNI_BaseLayer *layer) {
    // (primary only) set mode to layer
    if(!layer)
        return;

    SaveEnabledWork();

    try {
        tTJSNI_BaseLayer *current = GetCurrentModalLayer();
        if(current && layer->IsAncestorOrSelf(current))
            TVPThrowExceptionMessage(TVPCannotSetModeToDisabledOrModal);
        // cannot set mode to parent layer
        if(!layer->Visible)
            layer->Visible = true;
        if(!layer->GetParentVisible() || !layer->Enabled)
            TVPThrowExceptionMessage(TVPCannotSetModeToDisabledOrModal);
        // cannot set mode to parent layer
        if(layer == current)
            TVPThrowExceptionMessage(TVPCannotSetModeToDisabledOrModal);
        // cannot set mode to already modal layer

        SetFocusTo(layer->SearchFirstFocusable(), true);

        if(layer->Owner)
            layer->Owner->AddRef();
        ModalLayerVector.push_back(layer);

    } catch(...) {
        NotifyNodeEnabledState();
        throw;
    }

    NotifyNodeEnabledState();
}
//---------------------------------------------------------------------------
void tTVPLayerManager::RemoveModeFrom(tTJSNI_BaseLayer *layer) {
    // remove modal state from given layer
    bool do_notify = false;

    try {
        std::vector<tTJSNI_BaseLayer *>::iterator i;
        for(i = ModalLayerVector.begin(); i < ModalLayerVector.end();) {
            if(layer == *i) {
                if(!do_notify) {
                    do_notify = true;
                    SaveEnabledWork();
                }
                if(layer->Owner)
                    layer->Owner->Release();
                SetFocusTo(layer->GetNextFocusable(), true);
                i = ModalLayerVector.erase(i);
            } else {
                i++;
            }
        }
    } catch(...) {
        if(do_notify)
            NotifyNodeEnabledState();
        throw;
    }

    if(do_notify)
        NotifyNodeEnabledState();
}
//---------------------------------------------------------------------------
void tTVPLayerManager::RemoveTreeModalState(tTJSNI_BaseLayer *root) {
    // remove modal state from given tree
    bool do_notify = false;

    try {
        std::vector<tTJSNI_BaseLayer *>::iterator i;
        for(i = ModalLayerVector.begin(); i < ModalLayerVector.end();) {
            if((*i)->IsAncestorOrSelf(root)) {
                if(!do_notify) {
                    do_notify = true;
                    SaveEnabledWork();
                }
                // Preserve the reference ordering: release the live entry,
                // perform the focus callback, and only then erase at i.  Both
                // operations can invalidate the iterator through reentry; the
                // catch below balances enabled-state notification, not vector
                // mutation or owner references.
                if((*i)->Owner)
                    (*i)->Owner->Release();
                SetFocusTo(root->GetNextFocusable(), true);
                i = ModalLayerVector.erase(i);
            } else {
                i++;
            }
        }
    } catch(...) {
        if(do_notify)
            NotifyNodeEnabledState();
        throw;
    }

    if(do_notify)
        NotifyNodeEnabledState();
}
//---------------------------------------------------------------------------
tTJSNI_BaseLayer *tTVPLayerManager::GetCurrentModalLayer() const {
    // (primary only) get current modal layer
    tjs_uint size = (tjs_uint)ModalLayerVector.size();
    if(size == 0)
        return nullptr;
    return *(ModalLayerVector.begin() + size - 1);
}
//---------------------------------------------------------------------------
bool tTVPLayerManager::SearchAttentionPoint(tTJSNI_BaseLayer *target,
                                            tjs_int &x, tjs_int &y) {
    // search specified layer 's attention point
    while(target) {
        if(target->UseAttention) {
            x = target->AttentionLeft, y = target->AttentionTop;
            target->ToPrimaryCoordinates(x, y);
            return true;
        }
        target = target->Parent;
    }
    return false;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::SetAttentionPointOf(tTJSNI_BaseLayer *layer) {
    if(!LayerTreeOwner)
        return;
    tjs_int x, y;
    if(SearchAttentionPoint(layer, x, y))
        LayerTreeOwner->SetAttentionPoint(this, layer, x, y);
    else
        LayerTreeOwner->DisableAttentionPoint(this);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::DisableAttentionPoint() {
    if(LayerTreeOwner)
        LayerTreeOwner->DisableAttentionPoint(this);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::NotifyAttentionStateChanged(tTJSNI_BaseLayer *from) {
    if(FocusedLayer == from) {
        SetAttentionPointOf(from);
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::SetImeModeOf(tTJSNI_BaseLayer *layer) {
    if(!LayerTreeOwner)
        return;
    LayerTreeOwner->SetImeMode(this, layer->ImeMode);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::ResetImeMode() {
    if(!LayerTreeOwner)
        return;
    LayerTreeOwner->ResetImeMode(this);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::NotifyImeModeChanged(tTJSNI_BaseLayer *from) {
    if(FocusedLayer == from) {
        SetImeModeOf(from);
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::SaveEnabledWork() {
    // save current node enabled state to EnabledWork
    // this does recursive call
    if(EnabledWorkRefCount == 0)
        if(Primary)
            Primary->SaveEnabledWork();

    EnabledWorkRefCount++;
}
//---------------------------------------------------------------------------
void tTVPLayerManager::NotifyNodeEnabledState() {
    // notify node enabled state change to self and its children
    // this refers EnabledWork which is created by SaveEnabledWork
    EnabledWorkRefCount--;

    if(EnabledWorkRefCount == 0)
        if(Primary)
            Primary->NotifyNodeEnabledState();
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryKeyDown(tjs_uint key, tjs_uint32 shift) {
    if(FocusedLayer)
        FocusedLayer->FireKeyDown(key, shift);
    else if(Primary)
        Primary->DefaultKeyDown(key, shift);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryKeyUp(tjs_uint key, tjs_uint32 shift) {
    if(FocusedLayer)
        FocusedLayer->FireKeyUp(key, shift);
    else if(Primary)
        Primary->DefaultKeyUp(key, shift);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryKeyPress(tjs_char key) {
    if(FocusedLayer)
        FocusedLayer->FireKeyPress(key);
    else if(Primary)
        Primary->DefaultKeyPress(key);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::PrimaryMouseWheel(tjs_uint32 shift, tjs_int delta,
                                         tjs_int x, tjs_int y) {
    if(FocusedLayer)
        FocusedLayer->FireMouseWheel(shift, delta, x, y);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::AddUpdateRegion(const tTVPComplexRect &rects) {
    UpdateRegion.Or(rects);
    if(UpdateRegion.GetCount() > TVP_UPDATE_UNITE_LIMIT)
        UpdateRegion.Unite();
    if(lowLevelLogoTraceEnabled()) {
        const tjs_int addedCount = rects.GetCount();
        const tjs_int mergedCount = UpdateRegion.GetCount();
        if(mergedCount) {
            const auto &merged = UpdateRegion.GetBound();
            if(addedCount) {
                const auto &added = rects.GetBound();
                spdlog::get("core")->info(
                    "WCHAIN stage=manager.addUpdateRegion.rects manager={} "
                    "addedCount={} addedBound=[{},{},{},{}] mergedCount={} "
                    "mergedBound=[{},{},{},{}]",
                    (void *)this, addedCount, added.left, added.top,
                    added.right, added.bottom, mergedCount, merged.left,
                    merged.top, merged.right, merged.bottom);
            } else {
                spdlog::get("core")->info(
                    "WCHAIN stage=manager.addUpdateRegion.rects manager={} "
                    "addedCount=0 mergedCount={} mergedBound=[{},{},{},{}]",
                    (void *)this, mergedCount, merged.left, merged.top,
                    merged.right, merged.bottom);
            }
        }
    }
    NotifyWindowInvalidation();
}
//---------------------------------------------------------------------------
void tTVPLayerManager::AddUpdateRegion(const tTVPRect &rect) {
    // the window is invalidated;
    UpdateRegion.Or(rect);
    if(lowLevelLogoTraceEnabled()) {
        const tjs_int mergedCount = UpdateRegion.GetCount();
        if(mergedCount) {
            const auto &merged = UpdateRegion.GetBound();
            spdlog::get("core")->info(
                "WCHAIN stage=manager.addUpdateRegion.rect manager={} "
                "added=[{},{},{},{}] mergedCount={} mergedBound=[{},{},{},{}]",
                (void *)this, rect.left, rect.top, rect.right, rect.bottom,
                mergedCount, merged.left, merged.top, merged.right,
                merged.bottom);
        }
    }
    NotifyWindowInvalidation();
}
//---------------------------------------------------------------------------
void tTVPLayerManager::UpdateToDrawDevice() {
    if(!Primary) return;
    if(lowLevelLogoTraceEnabled()) {
        const tjs_int count = UpdateRegion.GetCount();
        if(count) {
            const auto &bound = UpdateRegion.GetBound();
            spdlog::get("core")->info(
                "WCHAIN stage=manager.updateToDrawDevice manager={} primary={} "
                "updateCount={} updateBound=[{},{},{},{}]",
                (void *)this, (void *)Primary, count, bound.left, bound.top,
                bound.right, bound.bottom);
        } else {
            spdlog::get("core")->info(
                "WCHAIN stage=manager.updateToDrawDevice manager={} primary={} "
                "updateCount=0",
                (void *)this, (void *)Primary);
        }
    }
    Primary->CompleteForWindow(this);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::NotifyUpdateRegionFixed() {
    // called by primary layer, notifying final update region is fixed
    //	Window->NotifyUpdateRegionFixed(UpdateRegion);
}
//---------------------------------------------------------------------------
void tTVPLayerManager::RequestInvalidation(const tTVPRect &r) {
    // called by the owner window to notify window surface is
    // invalidated by the system or user.
    if(!Primary)
        return;

    tTVPRect ur;
    tTVPRect cr(0, 0, Primary->Rect.get_width(), Primary->Rect.get_height());

    if(TVPIntersectRect(&ur, r, cr)) {
        AddUpdateRegion(ur);
    }
}
//---------------------------------------------------------------------------
void tTVPLayerManager::RecheckInputState() {
    // To re-check current layer under current mouse position
    // and update hint, cursor type and process layer enter/leave.
    // This can be reasonably slow, about 1 sec interval.
    ForceMouseRecheck();
}
//---------------------------------------------------------------------------
void tTVPLayerManager::DumpLayerStructure() {
    if(Primary)
        Primary->DumpStructure();
}
//---------------------------------------------------------------------------

bool tTVPDestTexture::CopyRect(
    tjs_int x, tjs_int y, const iTVPBaseBitmap *ref, tTVPRect refrect,
    tjs_int plane /*= (TVP_BB_COPY_MAIN | TVP_BB_COPY_MASK)*/) {
    if(HoldAlpha) {
        return tTVPBaseTexture::CopyRect(x, y, ref, refrect, TVP_BB_COPY_MAIN);
    } else {
        return tTVPBaseTexture::CopyRect(x, y, ref, refrect, plane);
    }
}

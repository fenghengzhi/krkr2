#include "tjsCommHead.h"
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
#include "SystemControl.h"
#include "EventIntf.h"
#include "MsgIntf.h"
// #include "WindowFormUnit.h"
#include "SysInitIntf.h"
#include "SysInitImpl.h"
#include "ScriptMgnIntf.h"
#include "WindowIntf.h"
#include "WindowImpl.h"
#include "StorageIntf.h"
#include "EmergencyExit.h" // for TVPCPUClock
#include "DebugIntf.h"
// #include "VersionFormUnit.h"
#include "WaveImpl.h"
#include "SystemImpl.h"
#include "UserEvent.h"
#include "Application.h"
#include "TickCount.h"
#include "Random.h"
#if defined(__EMSCRIPTEN__) &&                                             \
    defined(TVP_ENABLE_WCHAIN_CONTINUOUS_EVENT_TRACE) &&              \
    TVP_ENABLE_WCHAIN_CONTINUOUS_EVENT_TRACE
#include <spdlog/spdlog.h>
#include "LogoTrace.h"
#define TVP_HAS_WCHAIN_CONTINUOUS_EVENT_TRACE 1
#else
#define TVP_HAS_WCHAIN_CONTINUOUS_EVENT_TRACE 0
#endif

tTVPSystemControl *TVPSystemControl;
bool TVPSystemControlAlive = false;

#if TVP_HAS_WCHAIN_CONTINUOUS_EVENT_TRACE
// Keep the Web-only WCHAIN probe out of the reconstructed default path.  The
// reference Begin/End/pump state transitions contain no JS query, logger lookup
// or diagnostic sequence counter.
static bool TVPSystemControlLogoTraceEnabled() {
    return TVPLogoTraceEnabled();
}

static bool TVPSystemControlTraceSeqAllowed(uint32_t seq) {
    return seq <= 180 || (seq % 60) == 0;
}

static void TVPTraceSystemControlContinuous(const char *stage,
                                            bool continuousEventCalling,
                                            bool eventEnable) {
    if(!TVPSystemControlLogoTraceEnabled())
        return;

    static uint32_t seq = 0;
    ++seq;
    if(!TVPSystemControlTraceSeqAllowed(seq))
        return;

    if(auto logger = spdlog::get("core")) {
        logger->warn(
            "WCHAIN stage={} seq={} continuousEventCalling={} eventEnable={} flag={}",
            stage ? stage : "", seq, continuousEventCalling ? 1 : 0,
            eventEnable ? 1 : 0, TVPProcessContinuousHandlerEventFlag ? 1 : 0);
    }
}
#endif

//---------------------------------------------------------------------------
// Get whether to control main thread priority or to insert wait
//---------------------------------------------------------------------------
static bool TVPMainThreadPriorityControlInit = false;
static bool TVPMainThreadPriorityControl = false;
static bool TVPGetMainThreadPriorityControl() {
    // One-shot cache: unlike -contfreq, -lowpri is not refreshed when the
    // command-line generation changes.  Only the exact string "yes" enables it.
    if(TVPMainThreadPriorityControlInit)
        return TVPMainThreadPriorityControl;
    tTJSVariant val;
    if(TVPGetCommandLine(TJS_W("-lowpri"), &val)) {
        ttstr str(val);
        if(str == TJS_W("yes"))
            TVPMainThreadPriorityControl = true;
    }

    TVPMainThreadPriorityControlInit = true;

    return TVPMainThreadPriorityControl;
}

tTVPSystemControl::tTVPSystemControl() : EventEnable(true) {
    ContinuousEventCalling = false;
    AutoShowConsoleOnError = false;

    LastCompactedTick = 0;
    LastCloseClickedTick = 0;
    LastShowModalWindowSentTick = 0;
    LastRehashedTick = 0;

    TVPSystemControlAlive = true;
#if 0
	SystemWatchTimer.SetInterval(50);
	SystemWatchTimer.SetOnTimerHandler( this, &tTVPSystemControl::SystemWatchTimerTimer );
	SystemWatchTimer.SetEnabled( true );
#endif
}
void tTVPSystemControl::InvokeEvents() { CallDeliverAllEventsOnIdle(); }
void tTVPSystemControl::CallDeliverAllEventsOnIdle() {
    //	Application->PostMessageToMainWindow(
    // TVP_EV_DELIVER_EVENTS_DUMMY, 0, 0
    //);
}

void tTVPSystemControl::BeginContinuousEvent() {
    // Boolean/idempotent gate.  CallDeliverAllEventsOnIdle is empty in all four
    // targets, but the one-shot -lowpri getter is still evaluated on first entry.
    if(!ContinuousEventCalling) {
        ContinuousEventCalling = true;
#if TVP_HAS_WCHAIN_CONTINUOUS_EVENT_TRACE
        TVPTraceSystemControlContinuous("system.beginContinuousEvent",
                                        ContinuousEventCalling, EventEnable);
#endif
        InvokeEvents();
        if(TVPGetMainThreadPriorityControl()) {
            // make main thread priority lower
            //			SetThreadPriority(GetCurrentThread(),
            // THREAD_PRIORITY_LOWEST);
        }
    }
}
void tTVPSystemControl::EndContinuousEvent() {
    if(ContinuousEventCalling) {
        ContinuousEventCalling = false;
#if TVP_HAS_WCHAIN_CONTINUOUS_EVENT_TRACE
        TVPTraceSystemControlContinuous("system.endContinuousEvent",
                                        ContinuousEventCalling, EventEnable);
#endif
        if(TVPGetMainThreadPriorityControl()) {
            // make main thread priority normal
            //			SetThreadPriority(GetCurrentThread(),
            // THREAD_PRIORITY_NORMAL);
        }
    }
}
//---------------------------------------------------------------------------
void tTVPSystemControl::NotifyCloseClicked() {
    // close Button is clicked
    LastCloseClickedTick = TVPGetRoughTickCount32();
}

void tTVPSystemControl::NotifyEventDelivered() {
    // called from event system, notifying the event is delivered.
    LastCloseClickedTick = 0;
    // if(TVPHaltWarnForm) delete TVPHaltWarnForm, TVPHaltWarnForm =
    // nullptr;
}

bool tTVPSystemControl::ApplicationIdle() {
    DeliverEvents();
    bool cont = !ContinuousEventCalling;
    MixedIdleTick += TVPGetRoughTickCount32();
    return cont;
}

void tTVPSystemControl::DeliverEvents() {
    if(ContinuousEventCalling) {
        // Publish pending even when EventEnable suppresses actual delivery.
        TVPProcessContinuousHandlerEventFlag = true; // set flag
    }
#if TVP_HAS_WCHAIN_CONTINUOUS_EVENT_TRACE
    TVPTraceSystemControlContinuous("system.deliverEvents",
                                    ContinuousEventCalling, EventEnable);
#endif

    if(EventEnable) {
        TVPDeliverAllEvents();
    }
}

void tTVPSystemControl::SystemWatchTimerTimer() {
    if(TVPTerminated) {
        // this will ensure terminating the application.
        // the WM_QUIT message disappears in some unknown
        // situations...
        //		Application->PostMessageToMainWindow(
        // TVP_EV_DELIVER_EVENTS_DUMMY, 0, 0 );
        Application->Terminate();
        //		Application->PostMessageToMainWindow(
        // TVP_EV_DELIVER_EVENTS_DUMMY, 0, 0 );
    }

    // call events
    uint32_t tick = TVPGetRoughTickCount32();
    // push environ noise
    TVPPushEnvironNoise(&tick, sizeof(tick));
    TVPPushEnvironNoise(&LastCompactedTick, sizeof(LastCompactedTick));
    TVPPushEnvironNoise(&LastShowModalWindowSentTick,
                        sizeof(LastShowModalWindowSentTick));
    TVPPushEnvironNoise(&MixedIdleTick, sizeof(MixedIdleTick));
#if 0
	POINT pt;
	::GetCursorPos(&pt);
	TVPPushEnvironNoise(&pt, sizeof(pt));

	// CPU clock monitoring
	{
		static bool clock_rough_printed = false;
		if( !clock_rough_printed && TVPCPUClockAccuracy == ccaRough ) {
			tjs_char msg[80];
			TJS_snprintf(msg, 80, TVPInfoCpuClockRoughly, (int)TVPCPUClock);
			TVPAddImportantLog(msg);
			clock_rough_printed = true;
		}
		static bool clock_printed = false;
		if( !clock_printed && TVPCPUClockAccuracy == ccaAccurate ) {
			tjs_char msg[80];
			TJS_snprintf(msg, 80, TVPInfoCpuClock, (float)TVPCPUClock);
			TVPAddImportantLog(msg);
			clock_printed = true;
		}
	}
#endif
    // check status and deliver events
    DeliverEvents();

    // call TickBeat
    tjs_int count = TVPGetWindowCount();
    for(tjs_int i = 0; i < count; i++) {
        tTJSNI_Window *win = TVPGetWindowListAt(i);
        win->TickBeat();
    }

    // Continuous mode suppresses maintenance, but never the window TickBeat
    // loop above.  Keep the native strict comparisons and wraparound arithmetic.
    if(!ContinuousEventCalling && tick - LastCompactedTick > 4000) {
        // idle state over 4 sec.
        LastCompactedTick = tick;

        // fire compact event
        TVPDeliverCompactEvent(TVP_COMPACT_LEVEL_IDLE);
    }
    if(!ContinuousEventCalling && tick > LastRehashedTick + 1500) {
        // TJS2 object rehash
        LastRehashedTick = tick;
        TJSDoRehash();
    }
    // ensure modal window visible
    if(tick > LastShowModalWindowSentTick + 4100) {
        //	::PostMessage(Handle, WM_USER+0x32, 0, 0);
        // This is currently disabled because IME composition window
        // hides behind the window which is bringed top by the
        // window-rearrangement.
        LastShowModalWindowSentTick = tick;
    }
}

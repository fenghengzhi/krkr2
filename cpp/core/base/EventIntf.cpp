//---------------------------------------------------------------------------
/*
        TVP2 ( T Visual Presenter 2 )  A script authoring tool
        Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

        See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Script/Window Event Handling and Dispatching / System Idle Event
// Delivering
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include <algorithm>
#if defined(__EMSCRIPTEN__) &&                                             \
    defined(TVP_ENABLE_WCHAIN_CONTINUOUS_EVENT_TRACE) &&              \
    TVP_ENABLE_WCHAIN_CONTINUOUS_EVENT_TRACE
#include <string>
#include <spdlog/spdlog.h>
#include "LogoTrace.h"
#define TVP_HAS_WCHAIN_CONTINUOUS_EVENT_TRACE 1
#else
#define TVP_HAS_WCHAIN_CONTINUOUS_EVENT_TRACE 0
#endif
#include "SysInitIntf.h"
#include "EventIntf.h"
#include "WindowIntf.h"
#include "tjsDictionary.h"
#include "MsgIntf.h"
#include "ScriptMgnIntf.h"
#include "TickCount.h"
#include "SystemImpl.h"
#include "tjsDebug.h"

//---------------------------------------------------------------------------
// tTVPEvent  : script event class
//---------------------------------------------------------------------------
extern tjs_uint64 TVPEventSequenceNumber;

class tTVPEvent {
    iTJSDispatch2 *Target;
    iTJSDispatch2 *Source;
    ttstr EventName;
    tjs_uint32 Tag;
    tjs_uint NumArgs;
    tTJSVariant *Args;
    tjs_uint32 Flags;
    tjs_uint64 Sequence;

public:
    tTVPEvent(iTJSDispatch2 *target, iTJSDispatch2 *source, ttstr &eventname,
              tjs_uint32 tag, tjs_uint numargs, tTJSVariant *args,
              tjs_uint32 flags) {
        // constructor

        // eventname is not a const object but this object only touch
        // to eventname.GetHint()

        Args = nullptr;
        Target = nullptr;
        Source = nullptr;

        Sequence = TVPEventSequenceNumber;
        EventName = eventname;
        NumArgs = numargs;
        Args = new tTJSVariant[NumArgs];
        for(tjs_uint i = 0; i < NumArgs; i++)
            Args[i] = args[i];
        Target = target;
        Source = source;
        Tag = tag;
        Flags = flags;
        if(Target)
            Target->AddRef();
        if(Source)
            Source->AddRef();
    }

    tTVPEvent(const tTVPEvent &ref) {
        // copy constructor
        Args = nullptr;
        Target = nullptr;
        Source = nullptr;

        EventName = ref.EventName;
        NumArgs = ref.NumArgs;
        Args = new tTJSVariant[NumArgs];
        for(tjs_uint i = 0; i < NumArgs; i++)
            Args[i] = ref.Args[i];
        Target = ref.Target;
        Source = ref.Source;
        Tag = ref.Tag;
        if(Target)
            Target->AddRef();
        if(Source)
            Source->AddRef();
    }

    ~tTVPEvent() {
        if(Args)
            delete[] Args;
        if(Target)
            Target->Release();
        if(Source)
            Source->Release();
    }

    void Deliver() {
        if(!TJSIsObjectValid(Target->IsValid(0, nullptr, nullptr, Target)))
            return; // The target had been invalidated
        tTJSVariant **ArgsPtr = new tTJSVariant *[NumArgs];
        for(tjs_uint i = 0; i < NumArgs; i++)
            ArgsPtr[i] = Args + i;
        try {
            Target->FuncCall(0, EventName.c_str(), EventName.GetHint(), nullptr,
                             NumArgs, ArgsPtr, Target);
        } catch(...) {
            delete[] ArgsPtr;
            throw;
        }
        delete[] ArgsPtr;
    }

    iTJSDispatch2 *GetTargetNoAddRef() const { return Target; }

    iTJSDispatch2 *GetSourceNoAddRef() const { return Source; }

    ttstr &GetEventName() { return EventName; }

    tjs_uint32 GetTag() const { return Tag; }

    tjs_uint32 GetFlags() const { return Flags; }

    tjs_uint64 GetSequence() const;
};

//---------------------------------------------------------------------------
tjs_uint64 tTVPEvent::GetSequence() const { return Sequence; }
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// tTVPWinUpdateEvent : window update event class
//---------------------------------------------------------------------------
class tTVPWinUpdateEvent {
    tTJSNI_BaseWindow *Window;

public:
    tTVPWinUpdateEvent(tTJSNI_BaseWindow *window) { Window = window; }

    tTVPWinUpdateEvent(const tTVPWinUpdateEvent &ref) { Window = ref.Window; }

    ~tTVPWinUpdateEvent() = default;

    void Deliver() const {
        if(static_cast<tTJSNI_Window *>(Window)->GetVisible())
            Window->UpdateContent();
    }

    tTJSNI_BaseWindow *GetWindow() const { return Window; }

    void MarkEmpty() { Window = nullptr; }

    bool IsEmpty() const { return Window == nullptr; }
};
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// global/static definitions
//---------------------------------------------------------------------------
// event queue must be a globally sequential queue
std::vector<tTVPBaseInputEvent *> TVPInputEventQueue;
std::vector<tTVPEvent *> TVPEventQueue;
std::vector<tTVPWinUpdateEvent> TVPWinUpdateEventQueue;
bool TVPExclusiveEventPosted = false; // true if exclusive event is posted
tjs_uint64 TVPEventSequenceNumber = 0; // event sequence number
tjs_uint64 TVPEventSequenceNumberToProcess = 0;
// current event sequence which must be processed

static void TVPDestroyEventQueue() {
    // delete all event objects
    // deletion of event object may cause other deletion of event
    // objects.
    {
        std::vector<tTVPEvent *>::iterator i;
        while(TVPEventQueue.size()) {
            i = TVPEventQueue.end() - 1;
            tTVPEvent *ev = *i;
            TVPEventQueue.erase(i);
            delete ev;
        }
    }
    //--
    {
        std::vector<tTVPBaseInputEvent *>::iterator i;
        while(TVPInputEventQueue.size()) {
            i = TVPInputEventQueue.end() - 1;
            tTVPBaseInputEvent *ev = *i;
            TVPInputEventQueue.erase(i);
            delete ev;
        }
    }
}

static tTVPAtExit TVPDestroyEventQueueAtExit(TVP_ATEXIT_PRI_PREPARE,
                                             TVPDestroyEventQueue);

bool TVPEventDisabled = false;
bool TVPEventInterrupting = false;

#if TVP_HAS_WCHAIN_CONTINUOUS_EVENT_TRACE
static void TVPTraceContinuousPumpPoint(const char *stage);
#endif

// #define TVP_EVENT_TASK_RETURN_TICK 100000
/* TVP event system once returns to Operation system when
    TVP_EVENT_TASK_RETURN_TICK is elapsed during event delivering. */
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPPostEvent
//---------------------------------------------------------------------------
void TVPPostEvent(iTJSDispatch2 *source, iTJSDispatch2 *target,
                  ttstr &eventname, tjs_uint32 tag, tjs_uint32 flag,
                  tjs_uint numargs, tTJSVariant *args) {
    bool evdisabled = TVPEventDisabled || TVPGetSystemEventDisabledState();

    if((flag & TVP_EPT_DISCARDABLE) && (TVPEventInterrupting || evdisabled))
        return;

    tjs_int method = flag & TVP_EPT_METHOD_MASK;

    if(method == TVP_EPT_IMMEDIATE) {
        // the event is delivered immediately

        if(evdisabled)
            return;

        try {
            try {
                tTVPEvent(target, source, eventname, tag, numargs, args, flag)
                    .Deliver();
            }
            TJS_CONVERT_TO_TJS_EXCEPTION
        }
        TVP_CATCH_AND_SHOW_SCRIPT_EXCEPTION(TJS_W("immediate event"));

        return;
    }

    if(method == TVP_EPT_REMOVE_POST) {
        // events in queue that have same target/source/name/tag are
        // to be removed
        std::vector<tTVPEvent *>::iterator i;
        i = TVPEventQueue.begin();
        while(/*TVPEventQueue.size() &&*/ i != TVPEventQueue.end()) {
            if(source == (*i)->GetSourceNoAddRef() &&
               target == (*i)->GetTargetNoAddRef() &&
               eventname == (*i)->GetEventName() &&
               ((tag == 0) ? true : (tag == (*i)->GetTag()))) {
                tTVPEvent *ev = *i;
                TVPEventQueue.erase(i);
                i = TVPEventQueue.begin();
                delete ev;
            } else {
                i++;
            }
        }
    }

    // put into queue
    TVPEventQueue.push_back(
        new tTVPEvent(target, source, eventname, tag, numargs, args, flag));

    // is exclusive?
    if((flag & TVP_EPT_PRIO_MASK) == TVP_EPT_EXCLUSIVE)
        TVPExclusiveEventPosted = true;

    // make sure that the event is to be delivered.
    TVPInvokeEvents();
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPCancelEvents
//---------------------------------------------------------------------------
tjs_int TVPCancelEvents(iTJSDispatch2 *source, iTJSDispatch2 *target,
                        const ttstr &eventname, tjs_uint32 tag) {
    tjs_int count = 0;
    std::vector<tTVPEvent *>::iterator i;
    i = TVPEventQueue.begin();
    while(/*TVPEventQueue.size() &&*/ i != TVPEventQueue.end()) {
        if(source == (*i)->GetSourceNoAddRef() &&
           target == (*i)->GetTargetNoAddRef() &&
           eventname == (*i)->GetEventName() &&
           ((tag == 0) ? true : (tag == (*i)->GetTag()))) {
            tTVPEvent *ev = *i;
            TVPEventQueue.erase(i);
            i = TVPEventQueue.begin();
            delete ev;
            count++;
        } else {
            i++;
        }
    }
    return count;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPAreEventsInQueue
//---------------------------------------------------------------------------
bool TVPAreEventsInQueue(iTJSDispatch2 *source, iTJSDispatch2 *target,
                         const ttstr &eventname, tjs_uint32 tag) {
    std::vector<tTVPEvent *>::iterator i;
    i = TVPEventQueue.begin();
    while(/*TVPEventQueue.size() &&*/ i != TVPEventQueue.end()) {
        if(source == (*i)->GetSourceNoAddRef() &&
           target == (*i)->GetTargetNoAddRef() &&
           eventname == (*i)->GetEventName() &&
           ((tag == 0) ? true : (tag == (*i)->GetTag())))
            return true;
        i++;
    }
    return false;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPCountEventsInQueue
//---------------------------------------------------------------------------
tjs_int TVPCountEventsInQueue(iTJSDispatch2 *source, iTJSDispatch2 *target,
                              const ttstr &eventname, tjs_uint32 tag) {
    tjs_int count = 0;
    std::vector<tTVPEvent *>::iterator i;
    i = TVPEventQueue.begin();
    while(/*TVPEventQueue.size() &&*/ i != TVPEventQueue.end()) {
        if(source == (*i)->GetSourceNoAddRef() &&
           target == (*i)->GetTargetNoAddRef() &&
           eventname == (*i)->GetEventName() &&
           ((tag == 0) ? true : (tag == (*i)->GetTag())))
            count++;
        i++;
    }
    return count;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPCancelEventByTag
//---------------------------------------------------------------------------
void TVPCancelEventsByTag(iTJSDispatch2 *source, iTJSDispatch2 *target,
                          tjs_uint32 tag) {
    std::vector<tTVPEvent *>::iterator i;
    i = TVPEventQueue.begin();
    while(/*TVPEventQueue.size() &&*/ i != TVPEventQueue.end()) {
        if(source == (*i)->GetSourceNoAddRef() &&
           target == (*i)->GetTargetNoAddRef() &&
           ((tag == 0) ? true : (tag == (*i)->GetTag()))) {
            tTVPEvent *ev = *i;
            TVPEventQueue.erase(i);
            i = TVPEventQueue.begin();
            delete ev;
        } else {
            i++;
        }
    }
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPCancelSourceEvent
//---------------------------------------------------------------------------
void TVPCancelSourceEvents(iTJSDispatch2 *source) {
    std::vector<tTVPEvent *>::iterator i;
    i = TVPEventQueue.begin();
    while(/*TVPEventQueue.size() &&*/ i != TVPEventQueue.end()) {
        if(source == (*i)->GetSourceNoAddRef()) {
            tTVPEvent *ev = *i;
            TVPEventQueue.erase(i);
            i = TVPEventQueue.begin();
            delete ev;
        } else {
            i++;
        }
    }
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPDiscardAllDiscardableEvents
//---------------------------------------------------------------------------
void TVPDiscardAllDiscardableEvents() {
    std::vector<tTVPEvent *>::iterator i;
    i = TVPEventQueue.begin();
    while(/*TVPEventQueue.size() &&*/ i != TVPEventQueue.end()) {
        if((*i)->GetFlags() & TVP_EPT_DISCARDABLE) {
            tTVPEvent *ev = *i;
            TVPEventQueue.erase(i);
            i = TVPEventQueue.begin();
            delete ev;
        } else {
            i++;
        }
    }
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPDeliverAllEvents
//---------------------------------------------------------------------------
static void _TVPDeliverEventByPrio(tjs_uint prio) {
    while(true) {
        tTVPEvent *e;

        // retrieve item to deliver
        if(TVPEventQueue.size() == 0)
            break;
        std::vector<tTVPEvent *>::iterator i = TVPEventQueue.begin();
        while(i != TVPEventQueue.end()) {
            if((*i)->GetSequence() <= TVPEventSequenceNumberToProcess &&
               (((*i)->GetFlags() & TVP_EPT_PRIO_MASK) == prio))
                break;
            i++;
        }
        if(i == TVPEventQueue.end())
            break;
        e = *i;
        TVPEventQueue.erase(i);

        // event delivering
        try {
            e->Deliver();
        } catch(...) {
            delete e;
            throw;
        }
        delete e;
    }
}

static bool _TVPDeliverAllEvents2() {
    TVPExclusiveEventPosted = false;

    // process exclusive events
    _TVPDeliverEventByPrio(TVP_EPT_EXCLUSIVE);

    // check exclusive events
    if(TVPExclusiveEventPosted)
        return true;

    // process input event queue
    while(true) {
        tTVPBaseInputEvent *e;

        // retrieve item to deliver
        if(TVPInputEventQueue.size() == 0)
            break;
        std::vector<tTVPBaseInputEvent *>::iterator i =
            TVPInputEventQueue.begin();
        e = *i;
        TVPInputEventQueue.erase(i);

        // event delivering
        try {
            e->Deliver();
        } catch(...) {
            delete e;
            throw;
        }
        delete e;

        // check exclusive events
        if(TVPExclusiveEventPosted)
            return true;
    }

    // process normal event queue
    _TVPDeliverEventByPrio(TVP_EPT_NORMAL);

    // check exclusive events
    if(TVPExclusiveEventPosted)
        return true;

    return true;
}

//---------------------------------------------------------------------------
static bool _TVPDeliverAllEvents() {
    // deliver all pending events to targets.
    if(TVPEventDisabled)
        return true;

    // event invokation was received...
    TVPEventReceived();

    // for script event objects

    bool ret_value;

    ret_value = _TVPDeliverAllEvents2();

    return ret_value;
}

//---------------------------------------------------------------------------
void TVPDeliverAllEvents() {
    bool r;

    if(!TVPEventInterrupting) {
        TVPEventSequenceNumberToProcess = TVPEventSequenceNumber;
        TVPEventSequenceNumber++; // increment sequence number
    }

    TVPEventInterrupting = false;
    try {
        try {
            r = _TVPDeliverAllEvents();
        }
        TJS_CONVERT_TO_TJS_EXCEPTION
    }
    TVP_CATCH_AND_SHOW_SCRIPT_EXCEPTION(TJS_W("event"));

    if(!r) {
        // event processing is to be interrupted
        // XXX: currently this is not functional
        TVPEventInterrupting = true;
        TVPCallDeliverAllEventsOnIdle();
    }

    if(!TVPExclusiveEventPosted && !TVPEventInterrupting) {
        try {
            try {
                // process idle event queue
                _TVPDeliverEventByPrio(TVP_EPT_IDLE);
            }
            TJS_CONVERT_TO_TJS_EXCEPTION
        }
        TVP_CATCH_AND_SHOW_SCRIPT_EXCEPTION(TJS_W("idle event"));

        // process continuous events
        if(TVPProcessContinuousHandlerEventFlag) {
#if TVP_HAS_WCHAIN_CONTINUOUS_EVENT_TRACE
            TVPTraceContinuousPumpPoint("event.deliverAll.continuous-flag");
#endif
            TVPProcessContinuousHandlerEventFlag = false; // processed
            // XXX: strictly saying, we need something like
            // InterlockedExchange to look/set this flag, because
            // TVPProcessContinuousHandlerEventFlag may be accessed by
            // another thread. But I have no dought about that no one
            // does care of missing one event in rare race condition.

            TVPDeliverContinuousEvent();
        }
#if TVP_HAS_WCHAIN_CONTINUOUS_EVENT_TRACE
        else {
            TVPTraceContinuousPumpPoint("event.deliverAll.no-continuous-flag");
        }
#endif
        try {
            try {
                // for window content updating
                TVPDeliverWindowUpdateEvents();
            }
            TJS_CONVERT_TO_TJS_EXCEPTION
        }
        TVP_CATCH_AND_SHOW_SCRIPT_EXCEPTION(TJS_W("window update"));
    } else {
    }

    if(TVPEventQueue.size() == 0) {
        TVPEventSequenceNumber = 0; // reset the number
    }
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPPostWindowUpdate
//---------------------------------------------------------------------------
bool TVPWindowUpdateEventsDelivering = false;

void TVPPostWindowUpdate(tTJSNI_BaseWindow *window) {

    if(!TVPWindowUpdateEventsDelivering) {
        if(TVPWinUpdateEventQueue.size()) {
            // since duplication is not allowed ...
            std::vector<tTVPWinUpdateEvent>::const_iterator i;
            for(i = TVPWinUpdateEventQueue.begin();
                i != TVPWinUpdateEventQueue.end(); i++) {
                if(!i->IsEmpty() && window == i->GetWindow())
                    return;
            }
        }
    } else {
        if(TVPWinUpdateEventQueue.size()) {
            // duplication is allowed up to two
            tjs_int count = 0;
            std::vector<tTVPWinUpdateEvent>::const_iterator i;
            for(i = TVPWinUpdateEventQueue.begin();
                i != TVPWinUpdateEventQueue.end(); i++) {
                if(!i->IsEmpty() && window == i->GetWindow()) {
                    count++;
                    if(count == 2)
                        return;
                }
            }
        }
    }

    // put into queue.
    TVPWinUpdateEventQueue.emplace_back(window);

    // make sure that the event is to be delivered.
    TVPInvokeEvents();
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPRemoveWindowUpdate
//---------------------------------------------------------------------------
void TVPRemoveWindowUpdate(tTJSNI_BaseWindow *window) {
    // removes all window update events from queue.
    if(TVPWinUpdateEventQueue.size()) {
        std::vector<tTVPWinUpdateEvent>::iterator i;
        for(i = TVPWinUpdateEventQueue.begin();
            i != TVPWinUpdateEventQueue.end(); i++) {
            if(!i->IsEmpty() && window == i->GetWindow())
                i->MarkEmpty();
        }
    }
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPDeliverWindowUpdateEvents
//---------------------------------------------------------------------------
void TVPDeliverWindowUpdateEvents() {
    if(TVPWindowUpdateEventsDelivering)
        return; // does not allow re-entering
    TVPWindowUpdateEventsDelivering = true;

    try {
        for(tjs_uint i = 0; i < TVPWinUpdateEventQueue.size(); i++) {
            if(!TVPWinUpdateEventQueue[i].IsEmpty())
                TVPWinUpdateEventQueue[i].Deliver();
        }
    } catch(...) {
        TVPWinUpdateEventQueue.clear();
        TVPWindowUpdateEventsDelivering = false;
        throw;
    }
    TVPWinUpdateEventQueue.clear();
    TVPWindowUpdateEventsDelivering = false;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// Input Event related
//---------------------------------------------------------------------------
tjs_int TVPInputEventTagMax = 0;
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPPostInputEvent
//---------------------------------------------------------------------------
void TVPPostInputEvent(tTVPBaseInputEvent *ev, tjs_uint32 flags) {
    // flag check
    if((flags & TVP_EPT_DISCARDABLE) &&
       (TVPEventDisabled || TVPGetSystemEventDisabledState())) {
        delete ev;
        return;
    }

    if(flags & TVP_EPT_REMOVE_POST) {
        // cancel previously posted events
        TVPCancelInputEvents(ev->GetSource(), ev->GetTag());
    }

    // push into the event queue
    TVPInputEventQueue.push_back(ev);

    // make sure that the event is to be delivered.
    TVPInvokeEvents();
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPCancelInputEvents
//---------------------------------------------------------------------------
void TVPCancelInputEvents(void *source) {
    // removes all evens which have the same source
    if(TVPInputEventQueue.size()) {
        std::vector<tTVPBaseInputEvent *>::iterator i;
        for(i = TVPInputEventQueue.begin(); i != TVPInputEventQueue.end();) {
            if(source == (*i)->GetSource()) {
                tTVPBaseInputEvent *ev = *i;
                i = TVPInputEventQueue.erase(i);
                delete ev;
            } else {
                i++;
            }
        }
    }
}

//---------------------------------------------------------------------------
void TVPCancelInputEvents(void *source, tjs_int tag) {
    // removes all evens which have the same source and the same tag
    if(TVPInputEventQueue.size()) {
        std::vector<tTVPBaseInputEvent *>::iterator i;
        for(i = TVPInputEventQueue.begin(); i != TVPInputEventQueue.end();) {
            if(source == (*i)->GetSource() && tag == (*i)->GetTag()) {
                tTVPBaseInputEvent *ev = *i;
                i = TVPInputEventQueue.erase(i);
                delete ev;
            } else {
                i++;
            }
        }
    }
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPGetInputEventCount
//---------------------------------------------------------------------------
tjs_int TVPGetInputEventCount() { return (tjs_int)TVPInputEventQueue.size(); }
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// TVPCreateEventObject
//---------------------------------------------------------------------------
iTJSDispatch2 *TVPCreateEventObject(const tjs_char *type,
                                    iTJSDispatch2 *targthis,
                                    iTJSDispatch2 *targ) {
    // create a dictionary object for event dispatching ( to "action"
    // method )
    iTJSDispatch2 *object = TJSCreateDictionaryObject();

    static ttstr type_name(TJS_W("type"));
    static ttstr target_name(TJS_W("target"));

    {
        tTJSVariant val(type);
        if(TJS_FAILED(object->PropSet(TJS_MEMBERENSURE | TJS_IGNOREPROP,
                                      type_name.c_str(), type_name.GetHint(),
                                      &val, object)))
            TVPThrowInternalError;
    }

    {
        tTJSVariant val(targthis, targ);
        if(TJS_FAILED(object->PropSet(TJS_MEMBERENSURE | TJS_IGNOREPROP,
                                      target_name.c_str(),
                                      target_name.GetHint(), &val, object)))
            TVPThrowInternalError;
    }

    return object;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
ttstr TVPActionName(TJS_W("action"));
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// Continuous Event Delivering related
//---------------------------------------------------------------------------
bool TVPProcessContinuousHandlerEventFlag = false;
// The first vector is a non-owning raw-pointer registry.  Removal writes null
// tombstones; delivery performs the eventual erase.  The closure vector owns
// exactly the references acquired by TVPAddContinuousHandler and releases them
// manually because tTJSVariantClosure itself is a raw two-pointer value.
static std::vector<tTVPContinuousEventCallbackIntf *> TVPContinuousEventVector;
static std::vector<tTJSVariantClosure> TVPContinuousHandlerVector;
// This is a same-thread reentry guard, not a lock or an atomic synchronization
// primitive.  A nested TVPDeliverContinuousEvent call is deliberately a no-op.
static bool TVPContinuousEventProcessing = false;

#if defined(KRKR2_WASMTIME_DIAGNOSTICS)
// Read-only counters for the dedicated Wasmtime differential build.  They are
// deliberately excluded from every production/native target and never feed
// back into event control flow.
static int TVPWasmtimeContinuousHandlerAddCount = 0;
static int TVPWasmtimeContinuousHandlerRemoveCount = 0;
static int TVPWasmtimeContinuousDeliverCount = 0;
static int TVPWasmtimeContinuousHandlerInvokeCount = 0;
static int TVPWasmtimeContinuousHandlerFailureCount = 0;
static tjs_uint32 TVPWasmtimeContinuousTicks[512] = {};
static int TVPWasmtimeContinuousTickCount = 0;

extern "C" int TVPWasmtimeGetContinuousEventHookSlotCount() {
    return static_cast<int>(TVPContinuousEventVector.size());
}

extern "C" int TVPWasmtimeGetContinuousEventHookLiveCount() {
    return static_cast<int>(std::count_if(
        TVPContinuousEventVector.begin(), TVPContinuousEventVector.end(),
        [](const auto *hook) { return hook != nullptr; }));
}

extern "C" int TVPWasmtimeGetContinuousHandlerSlotCount() {
    return static_cast<int>(TVPContinuousHandlerVector.size());
}

extern "C" int TVPWasmtimeGetContinuousHandlerLiveCount() {
    return static_cast<int>(std::count_if(
        TVPContinuousHandlerVector.begin(), TVPContinuousHandlerVector.end(),
        [](const auto &closure) { return closure.Object != nullptr; }));
}

extern "C" int TVPWasmtimeGetContinuousHandlerAddCount() {
    return TVPWasmtimeContinuousHandlerAddCount;
}

extern "C" int TVPWasmtimeGetContinuousHandlerRemoveCount() {
    return TVPWasmtimeContinuousHandlerRemoveCount;
}

extern "C" int TVPWasmtimeGetContinuousDeliverCount() {
    return TVPWasmtimeContinuousDeliverCount;
}

extern "C" int TVPWasmtimeGetContinuousHandlerInvokeCount() {
    return TVPWasmtimeContinuousHandlerInvokeCount;
}

extern "C" int TVPWasmtimeGetContinuousHandlerFailureCount() {
    return TVPWasmtimeContinuousHandlerFailureCount;
}

extern "C" int TVPWasmtimeGetContinuousTickCount() {
    return TVPWasmtimeContinuousTickCount;
}

extern "C" int TVPWasmtimeGetContinuousTickAt(int index) {
    if(index < 0 || index >= TVPWasmtimeContinuousTickCount ||
       index >= static_cast<int>(std::size(TVPWasmtimeContinuousTicks))) {
        return 0;
    }
    return static_cast<int>(TVPWasmtimeContinuousTicks[index]);
}
#endif

#if TVP_HAS_WCHAIN_CONTINUOUS_EVENT_TRACE
// This diagnostic block is intentionally absent from normal builds.  The four
// reference binaries have no URL/JS query, stack capture, logger lookup or
// diagnostic counter on continuous-event paths.  Enabling it is therefore an
// explicit Wasmtime diagnostic mode, not part of the reconstructed behavior.
static bool TVPLogoChainTraceEnabledForEvents() {
    return TVPLogoTraceEnabled();
}

static bool TVPTraceContinuousSeqAllowed(tjs_uint64 seq) {
    return seq <= 180 || (seq % 60) == 0;
}

static std::string TVPShortTJSStackTrace(tjs_int limit = 6) {
    ttstr stack = TJSGetStackTraceString(limit, TJS_W(" <- "));
    return stack.AsStdString();
}

static void TVPTraceContinuousPumpPoint(const char *stage) {
    if(!TVPLogoChainTraceEnabledForEvents())
        return;

    static tjs_uint64 seq = 0;
    ++seq;

    const bool hasContinuousWork =
        !TVPContinuousEventVector.empty() || !TVPContinuousHandlerVector.empty();
    if(!hasContinuousWork && stage &&
       std::string(stage).find("no-continuous-flag") != std::string::npos) {
        return;
    }
    if(!TVPTraceContinuousSeqAllowed(seq))
        return;

    if(auto logger = spdlog::get("core")) {
        logger->warn(
            "WCHAIN stage={} seq={} flag={} eventHooks={} handlers={} eventDisabled={} eventQueue={} inputQueue={} winUpdateQueue={}",
            stage ? stage : "", seq,
            TVPProcessContinuousHandlerEventFlag ? 1 : 0,
            TVPContinuousEventVector.size(),
            TVPContinuousHandlerVector.size(), TVPEventDisabled ? 1 : 0,
            TVPEventQueue.size(), TVPInputEventQueue.size(),
            TVPWinUpdateEventQueue.size());
    }
}

static void TVPTraceContinuousRegistration(const char *stage,
                                           const tTJSVariantClosure *clo,
                                           const void *hook) {
    if(!TVPLogoChainTraceEnabledForEvents())
        return;

    if(auto logger = spdlog::get("core")) {
        logger->warn(
            "WCHAIN stage={} eventHooks={} handlers={} closureObject={} closureThis={} hook={} stack={}",
            stage ? stage : "", TVPContinuousEventVector.size(),
            TVPContinuousHandlerVector.size(),
            clo ? static_cast<void *>(clo->Object) : nullptr,
            clo ? static_cast<void *>(clo->ObjThis) : nullptr, hook,
            TVPShortTJSStackTrace());
    }
}
#endif

static void TVPDestroyContinuousHandlerVector() {
    // Registered at TVP_ATEXIT_PRI_PREPARE.  This logical destruction pass
    // releases the closure pairs before the vector's static storage destructor
    // later frees only the backing allocation.
    std::vector<tTJSVariantClosure>::iterator i;
    for(i = TVPContinuousHandlerVector.begin();
        i != TVPContinuousHandlerVector.end(); i++) {
        i->Release();
    }
    TVPContinuousHandlerVector.clear();
}

static tTVPAtExit
    TVPDestroyContinuousHandlerVectorAtExit(TVP_ATEXIT_PRI_PREPARE,
                                            TVPDestroyContinuousHandlerVector);

//---------------------------------------------------------------------------
void TVPAddContinuousEventHook(tTVPContinuousEventCallbackIntf *cb) {
    // Reference order is significant: scheduling begins before vector growth.
    // Null and duplicate pointers are accepted, and a growth exception does not
    // roll back TVPBeginContinuousEvent.
    TVPBeginContinuousEvent();
    TVPContinuousEventVector.push_back(cb);
#if TVP_HAS_WCHAIN_CONTINUOUS_EVENT_TRACE
    TVPTraceContinuousRegistration("event.addContinuousEventHook", nullptr, cb);
#endif
}

//---------------------------------------------------------------------------
void TVPRemoveContinuousEventHook(tTVPContinuousEventCallbackIntf *cb) {
    // Null every match.  Do not erase, shrink, release the callback, or call
    // TVPEndContinuousEvent here; a later successful delivery owns compaction
    // and the empty-registry stop transition.
    std::vector<tTVPContinuousEventCallbackIntf *>::iterator i;
    for(i = TVPContinuousEventVector.begin();
        i != TVPContinuousEventVector.end();) {
        if(cb == *i)
            *i = nullptr; // simply assign a nullptr
        i++;
    }
#if TVP_HAS_WCHAIN_CONTINUOUS_EVENT_TRACE
    TVPTraceContinuousRegistration("event.removeContinuousEventHook", nullptr,
                                   cb);
#endif
}

//---------------------------------------------------------------------------
static void _TVPDeliverContinuousEvent() // internal
{
#if defined(KRKR2_WASMTIME_DIAGNOSTICS)
    ++TVPWasmtimeContinuousDeliverCount;
#endif
    TVPStartTickCount();
    tjs_uint64 tick = TVPGetTickCount();
#if defined(KRKR2_WASMTIME_DIAGNOSTICS)
    if(TVPWasmtimeContinuousTickCount <
       static_cast<int>(std::size(TVPWasmtimeContinuousTicks))) {
        TVPWasmtimeContinuousTicks[TVPWasmtimeContinuousTickCount] =
            static_cast<tjs_uint32>(tick);
    }
    ++TVPWasmtimeContinuousTickCount;
#endif

#if TVP_HAS_WCHAIN_CONTINUOUS_EVENT_TRACE
    static tjs_uint64 deliverSeq = 0;
    const tjs_uint64 seq = ++deliverSeq;
    const bool trace =
        TVPLogoChainTraceEnabledForEvents() &&
        TVPTraceContinuousSeqAllowed(seq);
    tjs_uint32 hookCalls = 0;
    tjs_uint32 handlerCalls = 0;
    tjs_uint32 handlerFailures = 0;
    if(trace) {
        if(auto logger = spdlog::get("core")) {
            logger->warn(
                "WCHAIN stage=event.deliverContinuous.enter seq={} tick={} eventHooks={} handlers={} eventDisabled={}",
                seq, tick, TVPContinuousEventVector.size(),
                TVPContinuousHandlerVector.size(), TVPEventDisabled ? 1 : 0);
        }
    }
#endif

    if(TVPContinuousEventVector.size()) {
        bool emptyflag = false;
        // size() and operator[] are deliberately re-evaluated after callbacks.
        // An append (including one that reallocates) is visible in this same
        // pass.  If the current callback removes itself, this iteration already
        // observed a non-null slot and therefore does not set emptyflag; that
        // tombstone can survive until a later delivery.
        for(tjs_uint32 i = 0; i < TVPContinuousEventVector.size(); i++) {
            // note that the handler can remove itself while the event
            if(TVPContinuousEventVector[i]) {
#if TVP_HAS_WCHAIN_CONTINUOUS_EVENT_TRACE
                ++hookCalls;
#endif
                TVPContinuousEventVector[i]->OnContinuousCallback(tick);
            } else {
                emptyflag = true;
            }

            if(TVPExclusiveEventPosted) {
#if TVP_HAS_WCHAIN_CONTINUOUS_EVENT_TRACE
                if(trace) {
                    if(auto logger = spdlog::get("core")) {
                        logger->warn(
                            "WCHAIN stage=event.deliverContinuous.abort seq={} reason=exclusive-after-hook hookCalls={} handlerCalls={}",
                            seq, hookCalls, handlerCalls);
                    }
                }
#endif
                // This early return intentionally skips both tombstone
                // compaction and the empty-registry TVPEndContinuousEvent call.
                return; // check exclusive events
            }
        }

        if(emptyflag) {
            // the array has empty cell

            // eliminate empty
            std::vector<tTVPContinuousEventCallbackIntf *>::iterator i;
            for(i = TVPContinuousEventVector.begin();
                i != TVPContinuousEventVector.end();) {
                if(*i == nullptr)
                    i = TVPContinuousEventVector.erase(i);
                else
                    i++;
            }
        }
    }

    if(!TVPEventDisabled && TVPContinuousHandlerVector.size()) {
        bool emptyflag = false;
        tTJSVariant vtick((tjs_int64)tick);
        tTJSVariant *pvtick = &vtick;
        // As for hooks, the loop bound and indexed closure are live vector
        // accesses.  Successful self-removal of the current handler does not
        // set emptyflag here; removal of a future handler is observed later.
        for(tjs_uint i = 0; i < TVPContinuousHandlerVector.size(); i++) {
            if(TVPContinuousHandlerVector[i].Object) {
                tjs_error er;
                try {
#if defined(KRKR2_WASMTIME_DIAGNOSTICS)
                    ++TVPWasmtimeContinuousHandlerInvokeCount;
#endif
#if TVP_HAS_WCHAIN_CONTINUOUS_EVENT_TRACE
                    ++handlerCalls;
#endif
                    er = TVPContinuousHandlerVector[i].FuncCall(
                        0, nullptr, nullptr, nullptr, 1, &pvtick, nullptr);
                } catch(...) {
                    // failed
#if defined(KRKR2_WASMTIME_DIAGNOSTICS)
                    ++TVPWasmtimeContinuousHandlerFailureCount;
#endif
                    TVPContinuousHandlerVector[i].Release();
                    TVPContinuousHandlerVector[i].Object =
                        TVPContinuousHandlerVector[i].ObjThis = nullptr;
                    throw;
                }
                if(TJS_FAILED(er)) {
                    // failed
#if defined(KRKR2_WASMTIME_DIAGNOSTICS)
                    ++TVPWasmtimeContinuousHandlerFailureCount;
#endif
#if TVP_HAS_WCHAIN_CONTINUOUS_EVENT_TRACE
                    ++handlerFailures;
#endif
                    TVPContinuousHandlerVector[i].Release();
                    TVPContinuousHandlerVector[i].Object =
                        TVPContinuousHandlerVector[i].ObjThis = nullptr;
                    emptyflag = true;
                }
                if(TVPExclusiveEventPosted) {
#if TVP_HAS_WCHAIN_CONTINUOUS_EVENT_TRACE
                    if(trace) {
                        if(auto logger = spdlog::get("core")) {
                            logger->warn(
                                "WCHAIN stage=event.deliverContinuous.abort seq={} reason=exclusive-after-handler hookCalls={} handlerCalls={} handlerFailures={}",
                                seq, hookCalls, handlerCalls,
                                handlerFailures);
                        }
                    }
#endif
                    // As on the hook side, abort precedes compaction and End.
                    return; // check exclusive events
                }
            } else {
                emptyflag = true;
            }
        }

        if(emptyflag) {
            // the array has empty cell

            // eliminate empty
            std::vector<tTJSVariantClosure>::iterator i;
            for(i = TVPContinuousHandlerVector.begin();
                i != TVPContinuousHandlerVector.end();) {
                if(!i->Object) {
                    i->Release();
                    i = TVPContinuousHandlerVector.erase(i);
                } else {
                    i++;
                }
            }
        }
    }

    if(!TVPContinuousEventVector.size() && !TVPContinuousHandlerVector.size())
        TVPEndContinuousEvent();

#if TVP_HAS_WCHAIN_CONTINUOUS_EVENT_TRACE
    if(trace) {
        if(auto logger = spdlog::get("core")) {
            logger->warn(
                "WCHAIN stage=event.deliverContinuous.exit seq={} hookCalls={} handlerCalls={} handlerFailures={} remainingEventHooks={} remainingHandlers={}",
                seq, hookCalls, handlerCalls, handlerFailures,
                TVPContinuousEventVector.size(),
                TVPContinuousHandlerVector.size());
        }
    }
#endif
}

//---------------------------------------------------------------------------
void TVPDeliverContinuousEvent() {
    if(TVPContinuousEventProcessing)
        return;
    TVPContinuousEventProcessing = true;
    try {
        try {
            try {
                _TVPDeliverContinuousEvent();
            } catch(...) {
                TVPContinuousEventProcessing = false;
                throw;
            }
        }
        TJS_CONVERT_TO_TJS_EXCEPTION
    }
    TVP_CATCH_AND_SHOW_SCRIPT_EXCEPTION(TJS_W("continuous event"));

    TVPContinuousEventProcessing = false;
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
void TVPAddContinuousHandler(tTJSVariantClosure clo) {
    std::vector<tTJSVariantClosure>::iterator i;
    i = std::find(TVPContinuousHandlerVector.begin(),
                  TVPContinuousHandlerVector.end(), clo);
    if(i == TVPContinuousHandlerVector.end()) {
        // The reference binaries perform Begin and both AddRefs before a slow
        // vector growth.  They contain no rollback catch: allocation failure
        // can leave scheduling active and the newly acquired references
        // unowned.  Preserve that boundary rather than making this transaction
        // appear exception-safe.
        TVPBeginContinuousEvent();
        clo.AddRef();
        TVPContinuousHandlerVector.emplace_back(clo);
#if defined(KRKR2_WASMTIME_DIAGNOSTICS)
        ++TVPWasmtimeContinuousHandlerAddCount;
#endif
#if TVP_HAS_WCHAIN_CONTINUOUS_EVENT_TRACE
        TVPTraceContinuousRegistration("event.addContinuousHandler", &clo,
                                       nullptr);
#endif
    }
}

//---------------------------------------------------------------------------
void TVPRemoveContinuousHandler(tTJSVariantClosure clo) {
    std::vector<tTJSVariantClosure>::iterator i;
    i = std::find(TVPContinuousHandlerVector.begin(),
                  TVPContinuousHandlerVector.end(), clo);
    if(i != TVPContinuousHandlerVector.end()) {
#if defined(KRKR2_WASMTIME_DIAGNOSTICS)
        ++TVPWasmtimeContinuousHandlerRemoveCount;
#endif
#if TVP_HAS_WCHAIN_CONTINUOUS_EVENT_TRACE
        TVPTraceContinuousRegistration("event.removeContinuousHandler",
                                       &(*i), nullptr);
#endif
        // Removal releases the first exact pair and leaves an all-null
        // tombstone.  It neither erases the slot nor stops scheduling.
        i->Release();
        i->Object = i->ObjThis = nullptr;
    }
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// "Compact" Event Delivering related
//---------------------------------------------------------------------------
// Compact events are to be delivered when:
// 1. the application is in idle state for long duration
// 2. the application had been deactivated ( application has lost the
// focus )
// 3. the application had been minimized
// these are to reduce memory usage, like garbage collection, cache
// cleaning, or etc ...
//---------------------------------------------------------------------------
static std::vector<tTVPCompactEventCallbackIntf *> TVPCompactEventVector;
bool TVPEnableGlobalHeapCompaction = false;

//---------------------------------------------------------------------------
void TVPAddCompactEventHook(tTVPCompactEventCallbackIntf *cb) {
    TVPCompactEventVector.push_back(cb);
}

//---------------------------------------------------------------------------
void TVPRemoveCompactEventHook(tTVPCompactEventCallbackIntf *cb) {
    std::vector<tTVPCompactEventCallbackIntf *>::iterator i;
    for(i = TVPCompactEventVector.begin(); i != TVPCompactEventVector.end();) {
        if(cb == *i)
            *i = nullptr; // simply assign a nullptr
        i++;
    }
}

//---------------------------------------------------------------------------
extern void TVPDoSaveSystemVariables();

void TVPDeliverCompactEvent(tjs_int level) {
    // must be called by each platforms's implementation
    // std::vector<tTVPCompactEventCallbackIntf *>::iterator i;
    if(TVPCompactEventVector.size()) {
        bool emptyflag = false;
        for(tjs_uint i = 0; i < TVPCompactEventVector.size(); i++) {
            // note that the handler can remove itself while the event
            try {
                try {
                    if(TVPCompactEventVector[i])
                        TVPCompactEventVector[i]->OnCompact(level);
                    else
                        emptyflag = true;
                }
                TJS_CONVERT_TO_TJS_EXCEPTION
            }
            TVP_CATCH_AND_SHOW_SCRIPT_EXCEPTION_FORCE_SHOW_EXCEPTION(
                TJS_W("Compact Event"));
        }

        if(emptyflag) {
            // the array has empty cell

            // eliminate empty
            std::vector<tTVPCompactEventCallbackIntf *>::iterator i;
            for(i = TVPCompactEventVector.begin();
                i != TVPCompactEventVector.end();) {
                if(*i == nullptr)
                    i = TVPCompactEventVector.erase(i);
                else
                    i++;
            }
        }
    }
    TVPDoSaveSystemVariables();
#if 0
    if( level >= TVP_COMPACT_LEVEL_MAX && TVPEnableGlobalHeapCompaction )
    {	// Do compact CRT and Global Heap
        HANDLE hHeap = ::GetProcessHeap();
        if( hHeap ) {
            ::HeapCompact( hHeap, 0 );
        }
        HANDLE hCrtHeap = (HANDLE)_get_heap_handle();
        if( hCrtHeap && hCrtHeap != hHeap ) {
            ::HeapCompact( hCrtHeap, 0 );
        }
    }
#endif
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// AsyncTrigger related
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// tTJSNI_AsyncTrigger
//---------------------------------------------------------------------------
tTJSNI_AsyncTrigger::tTJSNI_AsyncTrigger() {
    Owner = nullptr;
    Cached = true;
    IdlePendingCount = 0;
    Mode = atmNormal;
    ActionOwner.Object = ActionOwner.ObjThis = nullptr;
    ActionName = TVPActionName;
}

//---------------------------------------------------------------------------
tjs_error tTJSNI_AsyncTrigger::Construct(tjs_int numparams, tTJSVariant **param,
                                         iTJSDispatch2 *tjs_obj) {
    if(numparams < 1)
        return TJS_E_BADPARAMCOUNT;

    tjs_error hr = inherited::Construct(numparams, param, tjs_obj);
    if(TJS_FAILED(hr))
        return hr;

    if(numparams >= 2 && param[1]->Type() != tvtVoid)
        ActionName = *param[1]; // action function to be called

    ActionOwner = param[0]->AsObjectClosure();
    Owner = tjs_obj;

    return TJS_S_OK;
}

//---------------------------------------------------------------------------
void tTJSNI_AsyncTrigger::Invalidate() {
    TVPCancelSourceEvents(Owner);
    Owner = nullptr;

    ActionOwner.Release();
    ActionOwner.ObjThis = ActionOwner.Object = nullptr;

    inherited::Invalidate();
}

//---------------------------------------------------------------------------
void tTJSNI_AsyncTrigger::Trigger() {
    // trigger event
    if(Owner) {
        if(Cached) {
            // remove undelivered events from queue when "Cached" flag
            // is set
            TVPCancelSourceEvents(Owner);
        }
        static ttstr eventname(TJS_W("onFire"));

        tjs_uint32 flags = TVP_EPT_POST;
        if(Mode == atmExclusive)
            flags |= TVP_EPT_EXCLUSIVE; // fire exclusive event
        if(Mode == atmAtIdle)
            flags |= TVP_EPT_IDLE; // fire idle event

        TVPPostEvent(Owner, Owner, eventname, 0, flags, 0, nullptr);
    }
}

//---------------------------------------------------------------------------
void tTJSNI_AsyncTrigger::Cancel() {
    // cancel event
    if(Owner)
        TVPCancelSourceEvents(Owner);
    IdlePendingCount = 0;
}

//---------------------------------------------------------------------------
void tTJSNI_AsyncTrigger::SetCached(bool b) {
    // set cached operation flag.
    // when this flag is set, only one event is delivered at once.
    if(Cached != b) {
        Cached = b;
        Cancel(); // all events are canceled
    }
}

//---------------------------------------------------------------------------
void tTJSNI_AsyncTrigger::SetMode(tTVPAsyncTriggerMode m) {
    if(Mode != m) {
        Mode = m;
        Cancel();
    }
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
// tTJSNC_AsyncTrigger
//---------------------------------------------------------------------------
tjs_uint32 tTJSNC_AsyncTrigger::ClassID = -1;

tTJSNC_AsyncTrigger::tTJSNC_AsyncTrigger() :
    inherited(TJS_W("AsyncTrigger")){
        // registration of native members

        TJS_BEGIN_NATIVE_MEMBERS(AsyncTrigger) // constructor
        TJS_DECL_EMPTY_FINALIZE_METHOD
            //----------------------------------------------------------------------
            TJS_BEGIN_NATIVE_CONSTRUCTOR_DECL(
                /*var.name*/ _this, /*var.type*/ tTJSNI_AsyncTrigger,
                /*TJS class name*/ AsyncTrigger){ return TJS_S_OK;
}
TJS_END_NATIVE_CONSTRUCTOR_DECL(/*TJS class name*/ AsyncTrigger)
//----------------------------------------------------------------------

//-- methods

//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/ trigger) {
    TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                            /*var. type*/ tTJSNI_AsyncTrigger);
    _this->Trigger();
    return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/ trigger)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/ cancel) {
    TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                            /*var. type*/ tTJSNI_AsyncTrigger);
    _this->Cancel();
    return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/ cancel)
//----------------------------------------------------------------------

//-- events

//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_METHOD_DECL(/*func. name*/ onFire) {
    TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                            /*var. type*/ tTJSNI_AsyncTrigger);

    tTJSVariantClosure obj = _this->GetActionOwnerNoAddRef();
    if(obj.Object) {
        ttstr &actionname = _this->GetActionName();
        TVP_ACTION_INVOKE_BEGIN(0, "onFire", objthis);
        TVP_ACTION_INVOKE_END_NAME(
            obj, actionname.IsEmpty() ? nullptr : actionname.c_str(),
            actionname.IsEmpty() ? nullptr : actionname.GetHint());
    }

    return TJS_S_OK;
}
TJS_END_NATIVE_METHOD_DECL(/*func. name*/ onFire)
//----------------------------------------------------------------------

//--properties

//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(cached){ TJS_BEGIN_NATIVE_PROP_GETTER{
    TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                            /*var. type*/ tTJSNI_AsyncTrigger);
*result = _this->GetCached();
return TJS_S_OK;
}
TJS_END_NATIVE_PROP_GETTER

TJS_BEGIN_NATIVE_PROP_SETTER {
    TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                            /*var. type*/ tTJSNI_AsyncTrigger);
    _this->SetCached(*param);
    return TJS_S_OK;
}
TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(cached)
//----------------------------------------------------------------------
TJS_BEGIN_NATIVE_PROP_DECL(mode){ TJS_BEGIN_NATIVE_PROP_GETTER{
    TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                            /*var. type*/ tTJSNI_AsyncTrigger);
*result = (tjs_int)_this->GetMode();
return TJS_S_OK;
}
TJS_END_NATIVE_PROP_GETTER

TJS_BEGIN_NATIVE_PROP_SETTER {
    TJS_GET_NATIVE_INSTANCE(/*var. name*/ _this,
                            /*var. type*/ tTJSNI_AsyncTrigger);
    _this->SetMode((tTVPAsyncTriggerMode)(tjs_int)*param);
    return TJS_S_OK;
}
TJS_END_NATIVE_PROP_SETTER
}
TJS_END_NATIVE_PROP_DECL(mode)
//----------------------------------------------------------------------
TJS_END_NATIVE_MEMBERS
}

//---------------------------------------------------------------------------
tTJSNativeInstance *tTJSNC_AsyncTrigger::CreateNativeInstance() {
    return new tTJSNI_AsyncTrigger();
}
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
tTJSNativeClass *TVPCreateNativeClass_AsyncTrigger() {
    return new tTJSNC_AsyncTrigger();
}
//---------------------------------------------------------------------------

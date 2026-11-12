/*
 * Copyright (C) 2024 Keith Cirkel <webkit@keithcirkel.co.uk>. All rights reserved.
 * Copyright (C) 2026 Ben Lesh. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once
#include "JSValueInWrappedObject.h"
#include "Observable.h"
#include "ShareConfig.h"
#include <wtf/Deque.h>

namespace WebCore {
struct SharedObservableConfig;
class SharedObservable final : public Observable {
    WTF_MAKE_TZONE_ALLOCATED(SharedObservable);
public:
    using ProducerUnion = Variant<RefPtr<JSSubscriberCallback>, SharedObservableConfig>;
    static ExceptionOr<Ref<SharedObservable>> create(JSDOMGlobalObject&, ProducerUnion&&);
    static Ref<SharedObservable> create(ScriptExecutionContext&, Ref<SubscriberCallback>&&, ShareSettings, Vector<Ref<SharedObservable>>&&);
    ~SharedObservable();
    bool isSharedObservable() const final { return true; }
    const ShareSettings& settings() const { return m_settings; }
    uint64_t maxReplayValues() const { return m_settings.maxReplayValues; }
    uint64_t activeSubscribers() const { return m_activeSubscribers; }
    Vector<JSC::JSValue> replayValues() const;
    Subscriber* currentSubscriber() const { return m_currentSubscriber.get(); }
    void setCurrentSubscriber(Subscriber*);
    enum class Terminal : uint8_t { None, Complete, Error };
    Terminal terminal() const { return m_terminal; }
    JSC::JSValue terminalReason() const;
    void retainValue(JSC::JSValue);
    void finish(Subscriber&, Terminal, JSC::JSValue, bool);
    void lastObserverLeft(Subscriber&);
    void observationAdded() { ++m_activeSubscribers; }
    void observationRemoved() { ASSERT(m_activeSubscribers); --m_activeSubscribers; }
    template<typename Visitor> void visitSharedChildrenInGCThread(Visitor&);
private:
    SharedObservable(ScriptExecutionContext&, Ref<SubscriberCallback>&&, ShareSettings, Vector<Ref<SharedObservable>>&&);
    void stop() final;
    void retainTerminal(Terminal, JSC::JSValue);
    const ShareSettings m_settings;
    WeakPtr<Subscriber> m_currentSubscriber;
    uint64_t m_activeSubscribers { 0 };
    Terminal m_terminal { Terminal::None };
    mutable Lock m_replayLock;
    Deque<std::unique_ptr<JSValueInWrappedObject>> m_replay WTF_GUARDED_BY_LOCK(m_replayLock);
    JSValueInWrappedObject m_terminalReason;
};
}
SPECIALIZE_TYPE_TRAITS_BEGIN(WebCore::SharedObservable)
    static bool isType(const WebCore::Observable& observable) { return observable.isSharedObservable(); }
SPECIALIZE_TYPE_TRAITS_END()

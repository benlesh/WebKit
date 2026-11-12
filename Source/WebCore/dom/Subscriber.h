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
#include "AbortSignal.h"
#include "ActiveDOMObject.h"
#include "InternalObserver.h"
#include "ScriptWrappable.h"
#include "TerminationConfig.h"
#include "VoidCallback.h"
#include <wtf/RefCountedAndCanMakeWeakPtr.h>

namespace WebCore {
class Observable;
class ObservableAlgorithm;
class SharedObservable;
class Subscriber;

// Cancellation remains installed while a detached observation awaits terminal delivery.
class ObservableObservation final : public RefCountedAndCanMakeWeakPtr<ObservableObservation> {
public:
    static Ref<ObservableObservation> create(Observable&, Ref<InternalObserver>&&, RefPtr<AbortSignal>&&);
    ~ObservableObservation();
    void attach(Subscriber&);
    void cancel(JSC::JSValue);
    void detach();
    void next(JSC::JSValue);
    void terminal(bool isError, JSC::JSValue);
    bool active() const { return m_active && !m_canceled; }
    void removeAbortAlgorithm();
    void visit(JSC::AbstractSlotVisitor&) const;
private:
    ObservableObservation(Observable&, Ref<InternalObserver>&&, RefPtr<AbortSignal>&&);
    const Ref<InternalObserver> m_observer;
    const RefPtr<AbortSignal> m_consumerSignal;
    WeakPtr<SharedObservable> m_sharedOwner;
    WeakPtr<Subscriber> m_subscriber;
    std::optional<uint32_t> m_abortAlgorithm;
    bool m_active { true };
    bool m_canceled { false };
};

class Subscriber final : public ActiveDOMObject, public ScriptWrappable, public RefCounted<Subscriber> {
    WTF_MAKE_TZONE_ALLOCATED(Subscriber);
public:
    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }
    static Ref<Subscriber> create(ScriptExecutionContext&, Observable&);
    ~Subscriber();
    void next(JSC::JSValue);
    void complete(const TerminationConfig& = { });
    void error(JSC::JSValue, const TerminationConfig& = { });
    void deliverRetainedTerminal(bool, JSC::JSValue);
    void addTeardown(Ref<VoidCallback>);
    void addNativeTeardown(Function<void()>&&);
    void setAlgorithm(Ref<ObservableAlgorithm>&&);
    bool active() const { return m_active; }
    bool isActive() const;
    AbortSignal& signal() { return m_signal.get(); }
    Observable& owner() const { return m_owner.get(); }
    void addObservation(Ref<ObservableObservation>&&);
    void cancelObservation(ObservableObservation&, JSC::JSValue);
    void reportErrorObject(JSC::JSValue);
    template<typename Visitor> void visitAdditionalChildrenInGCThread(Visitor&);
private:
    Subscriber(ScriptExecutionContext&, Observable&);
    void terminate(bool isError, JSC::JSValue, bool close);
    void close(JSC::JSValue = JSC::jsUndefined());
    void didChange();
    void stop() final;
    bool virtualHasPendingActivity() const final { return m_active; }
    bool isInactiveDocument() const;
    bool m_active { true };
    mutable Lock m_algorithmLock;
    RefPtr<ObservableAlgorithm> m_algorithm WTF_GUARDED_BY_LOCK(m_algorithmLock);
    const Ref<Observable> m_owner;
    const Ref<AbortSignal> m_signal;
    mutable Lock m_observationsLock;
    Vector<Ref<ObservableObservation>> m_observations WTF_GUARDED_BY_LOCK(m_observationsLock);
    Vector<Ref<ObservableObservation>> m_terminalObservations WTF_GUARDED_BY_LOCK(m_observationsLock);
    mutable Lock m_teardownsLock;
    using Teardown = Variant<Ref<VoidCallback>, Function<void()>>;
    Vector<Teardown> m_teardowns WTF_GUARDED_BY_LOCK(m_teardownsLock);
    Vector<Teardown> m_executingTeardowns WTF_GUARDED_BY_LOCK(m_teardownsLock);
};
}

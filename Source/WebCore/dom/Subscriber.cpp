/*
 * Copyright (C) 2026 Ben Lesh. All rights reserved.
 * Copyright (C) 2024 Keith Cirkel <webkit@keithcirkel.co.uk>. All rights reserved.
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

#include "config.h"
#include "Subscriber.h"

#include "ContextDestructionObserverInlines.h"
#include "Document.h"
#include "JSDOMExceptionHandling.h"
#include "JSSubscriber.h"
#include "Observable.h"
#include "ObservableAlgorithm.h"
#include "ScriptWrappableInlines.h"
#include "SharedObservable.h"
#include <JavaScriptCore/Exception.h>
#include <JavaScriptCore/SlotVisitorInlines.h>
#include <JavaScriptCore/Strong.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(Subscriber);

Ref<ObservableObservation> ObservableObservation::create(Observable& owner, Ref<InternalObserver>&& observer, RefPtr<AbortSignal>&& signal)
{
    Ref observation = adoptRef(*new ObservableObservation(owner, WTF::move(observer), WTF::move(signal)));
    if (observation->m_consumerSignal) {
        observation->m_abortAlgorithm = observation->m_consumerSignal->addAlgorithm([weakObservation = WeakPtr { observation.get() }](JSC::JSValue reason) {
            if (RefPtr observation = weakObservation)
                observation->cancel(reason);
        });
    }
    return observation;
}

ObservableObservation::ObservableObservation(Observable& owner, Ref<InternalObserver>&& observer, RefPtr<AbortSignal>&& signal)
    : m_observer(WTF::move(observer))
    , m_consumerSignal(WTF::move(signal))
    , m_sharedOwner(dynamicDowncast<SharedObservable>(owner))
{
    if (m_sharedOwner)
        m_sharedOwner->observationAdded();
}

ObservableObservation::~ObservableObservation()
{
    removeAbortAlgorithm();
    detach();
}

void ObservableObservation::attach(Subscriber& subscriber)
{
    m_subscriber = subscriber;
    m_observer->setSubscriber(subscriber);
}

void ObservableObservation::removeAbortAlgorithm()
{
    if (auto identifier = std::exchange(m_abortAlgorithm, std::nullopt))
        m_consumerSignal->removeAlgorithm(*identifier);
}

void ObservableObservation::detach()
{
    if (!std::exchange(m_active, false))
        return;
    if (m_sharedOwner)
        m_sharedOwner->observationRemoved();
    m_observer->deactivate();
}

void ObservableObservation::cancel(JSC::JSValue reason)
{
    if (std::exchange(m_canceled, true))
        return;
    detach();
    removeAbortAlgorithm();
    if (RefPtr subscriber = m_subscriber)
        subscriber->cancelObservation(*this, reason);
}

void ObservableObservation::next(JSC::JSValue value)
{
    if (active())
        m_observer->next(value);
}

void ObservableObservation::terminal(bool isError, JSC::JSValue reason)
{
    detach();
    if (!m_canceled) {
        if (isError)
            m_observer->error(reason);
        else
            m_observer->complete();
    }
    removeAbortAlgorithm();
}

void ObservableObservation::visit(JSC::AbstractSlotVisitor& visitor) const
{
    m_observer->visitAdditionalChildrenInGCThread(visitor);
    visitor.addOpaqueRoot(m_consumerSignal.get());
}

Ref<Subscriber> Subscriber::create(ScriptExecutionContext& context, Observable& owner)
{
    Ref subscriber = adoptRef(*new Subscriber(context, owner));
    subscriber->suspendIfNeeded();
    return subscriber;
}

Subscriber::Subscriber(ScriptExecutionContext& context, Observable& owner)
    : ActiveDOMObject(&context)
    , m_owner(owner)
    , m_signal(AbortSignal::create(&context))
{
}

Subscriber::~Subscriber() = default;

bool Subscriber::isInactiveDocument() const
{
    RefPtr document = dynamicDowncast<Document>(scriptExecutionContext());
    return document && !document->isFullyActive();
}

bool Subscriber::isActive() const
{
    return m_active && !isContextStopped() && scriptExecutionContext() && !isInactiveDocument();
}

void Subscriber::addObservation(Ref<ObservableObservation>&& observation)
{
    observation->attach(*this);
    {
        Locker locker { m_observationsLock };
        m_observations.append(WTF::move(observation));
    }
    didChange();
}

void Subscriber::next(JSC::JSValue value)
{
    if (!isActive())
        return;
    Ref protectedThis { *this };
    if (RefPtr shared = dynamicDowncast<SharedObservable>(m_owner))
        shared->retainValue(value);
    Vector<Ref<ObservableObservation>> observations;
    {
        Locker locker { m_observationsLock };
        observations = m_observations;
    }
    for (auto& observation : observations) {
        if (!isActive())
            break;
        observation->next(value);
    }
}

void Subscriber::error(JSC::JSValue value, const TerminationConfig& config)
{
    if (!m_active) {
        reportErrorObject(value);
        return;
    }
    if (isActive())
        terminate(true, value, config.close);
}

void Subscriber::complete(const TerminationConfig& config)
{
    if (isActive())
        terminate(false, JSC::jsUndefined(), config.close);
}

void Subscriber::terminate(bool isError, JSC::JSValue reason, bool closePermanently)
{
    Ref protectedThis { *this };
    RefPtr context = scriptExecutionContext();
    auto* globalObject = context->globalObject();
    // Keep the wrapper (and its callbacks) alive while arbitrary script runs below.
    JSC::Strong<JSC::JSObject> wrapper(globalObject->vm(), JSC::asObject(toJS(globalObject, globalObject, *this)));
    JSC::Strong<JSC::Unknown> protectedReason(globalObject->vm(), reason);
    {
        Locker locker { m_observationsLock };
        m_terminalObservations = std::exchange(m_observations, { });
    }
    if (RefPtr shared = dynamicDowncast<SharedObservable>(m_owner))
        shared->finish(*this, isError ? SharedObservable::Terminal::Error : SharedObservable::Terminal::Complete, reason, closePermanently);
    // Commit the entire old execution before abort events or teardown can reenter.
    for (auto& observation : m_terminalObservations)
        observation->detach();
    close();
    for (auto& observation : m_terminalObservations) {
        if (!scriptExecutionContext() || isInactiveDocument())
            break;
        observation->terminal(isError, reason);
    }
    Locker locker { m_observationsLock };
    m_terminalObservations.clear();
}

void Subscriber::deliverRetainedTerminal(bool isError, JSC::JSValue reason)
{
    if (!isActive())
        return;
    {
        Locker locker { m_observationsLock };
        m_terminalObservations = std::exchange(m_observations, { });
    }
    for (auto& observation : m_terminalObservations)
        observation->detach();
    close();
    for (auto& observation : m_terminalObservations)
        observation->terminal(isError, reason);
    Locker locker { m_observationsLock };
    m_terminalObservations.clear();
}

void Subscriber::cancelObservation(ObservableObservation& observation, JSC::JSValue reason)
{
    bool empty;
    {
        Locker locker { m_observationsLock };
        m_observations.removeFirstMatching([&](auto& item) {
            return item.ptr() == &observation;
        });
        empty = m_observations.isEmpty();
    }
    if (!m_active || !empty)
        return;
    if (RefPtr shared = dynamicDowncast<SharedObservable>(m_owner)) {
        if (shared->currentSubscriber() == this) {
            if (shared->settings().keepAliveOnLastUnsubscribe)
                return;
            shared->lastObserverLeft(*this);
        }
    }
    close(reason);
}

void Subscriber::addTeardown(Ref<VoidCallback> callback)
{
    if (!scriptExecutionContext() || isInactiveDocument())
        return;
    if (!m_active) {
        callback->invoke();
        return;
    }
    {
        Locker locker { m_teardownsLock };
        m_teardowns.append(WTF::move(callback));
    }
    didChange();
}

void Subscriber::setAlgorithm(Ref<ObservableAlgorithm>&& algorithm)
{
    {
        Locker locker { m_algorithmLock };
        m_algorithm = WTF::move(algorithm);
    }
    didChange();
}

void Subscriber::didChange()
{
    auto* globalObject = scriptExecutionContext()->globalObject();
    globalObject->vm().writeBarrier(toJS(globalObject, globalObject, *this).asCell());
}

void Subscriber::addNativeTeardown(Function<void()>&& callback)
{
    if (!m_active) {
        callback();
        return;
    }
    Locker locker { m_teardownsLock };
    m_teardowns.append(WTF::move(callback));
}

void Subscriber::close(JSC::JSValue reason)
{
    if (!m_active)
        return;
    RefPtr context = scriptExecutionContext();
    if (!context)
        return;
    auto* globalObject = context->globalObject();
    JSC::Strong<JSC::JSObject> wrapper(globalObject->vm(), JSC::asObject(toJS(globalObject, globalObject, *this)));
    m_active = false;
    {
        Locker locker { m_teardownsLock };
        m_executingTeardowns = std::exchange(m_teardowns, { });
    }
    m_signal->signalAbort(reason);
    // No lock is held across user callbacks. Retain the list for concurrent GC.
    for (auto& teardown : m_executingTeardowns | std::views::reverse) {
        if (!scriptExecutionContext() || isInactiveDocument())
            break;
        WTF::switchOn(teardown,
            [](Ref<VoidCallback>& callback) { callback->invoke(); },
            [](Function<void()>& callback) {
                callback();
            });
    }
    Locker locker { m_teardownsLock };
    m_executingTeardowns.clear();
}

void Subscriber::stop()
{
    m_active = false;
    // Context destruction must not execute JavaScript.
    Vector<Ref<ObservableObservation>> observations;
    {
        Locker locker { m_observationsLock };
        observations = std::exchange(m_observations, { });
    }
    for (auto& observation : observations) {
        observation->detach();
        observation->removeAbortAlgorithm();
    }
    Locker locker { m_teardownsLock };
    m_teardowns.clear();
}

void Subscriber::reportErrorObject(JSC::JSValue value)
{
    RefPtr context = scriptExecutionContext();
    if (!context || !context->globalObject())
        return;
    auto* globalObject = context->globalObject();
    JSC::JSLockHolder lock(globalObject->vm());
    reportException(globalObject, JSC::Exception::create(globalObject->vm(), value));
}

template<typename Visitor>
void Subscriber::visitAdditionalChildrenInGCThread(Visitor& visitor)
{
    {
        Locker locker { m_algorithmLock };
        if (m_algorithm)
            m_algorithm->visit(visitor);
    }
    visitor.addOpaqueRoot(m_owner.ptr());
    visitor.addOpaqueRoot(m_signal.ptr());
    {
        Locker locker { m_observationsLock };
        for (auto& observation : m_observations)
            observation->visit(visitor);
        for (auto& observation : m_terminalObservations)
            observation->visit(visitor);
    }
    {
        Locker locker { m_teardownsLock };
        auto visit = [&](auto& teardowns) {
            for (auto& teardown : teardowns) {
                if (auto* callback = std::get_if<Ref<VoidCallback>>(&teardown))
                    (*callback)->visitJSFunctionInGCThread(visitor);
            }
        };
        visit(m_teardowns);
        visit(m_executingTeardowns);
    }
}

DEFINE_VISIT_ADDITIONAL_CHILDREN_IN_GC_THREAD(Subscriber);

} // namespace WebCore

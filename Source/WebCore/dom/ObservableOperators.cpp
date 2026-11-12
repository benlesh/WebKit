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
#include "Observable.h"

#include "ContextDestructionObserverInlines.h"
#include "JSDOMExceptionHandling.h"
#include "JSSubscriber.h"
#include "JSValueInWrappedObjectInlines.h"
#include "MapperCallback.h"
#include "ObservableAlgorithm.h"
#include "ObservableCatchCallback.h"
#include "ObservableNativeCallback.h"
#include "SharedObservable.h"
#include "SubscribeOptions.h"
#include <JavaScriptCore/Strong.h>
#include <JavaScriptCore/TopExceptionScope.h>
#include <wtf/Deque.h>

namespace WebCore {

static void follow(Observable& source, Subscriber& destination, Ref<InternalObserver>&& observer, RefPtr<AbortSignal>&& signal = nullptr)
{
    if (!destination.isActive())
        return;
    SubscribeOptions options;
    options.signal = signal ? WTF::move(signal) : RefPtr { destination.signal() };
    source.subscribeInternal(*destination.scriptExecutionContext(), WTF::move(observer), WTF::move(options));
}

Ref<ObservableNativeObserver> ObservableNativeObserver::forward(ScriptExecutionContext& context, Subscriber& subscriber)
{
    return create(context, subscriber,
        [subscriber = Ref { subscriber }](JSC::JSValue value) { subscriber->next(value); },
        [subscriber = Ref { subscriber }](JSC::JSValue value) { subscriber->error(value); },
        [subscriber = Ref { subscriber }] {
            subscriber->complete();
        });
}

Ref<SubscriberCallback> ObservableNativeCallback::forward(ScriptExecutionContext& context, Observable& source)
{
    return create(context, { Ref { source } }, { }, [source = Ref { source }](Subscriber& subscriber) {
        follow(source, subscriber, ObservableNativeObserver::forward(*subscriber.scriptExecutionContext(), subscriber));
    });
}

// The caller's top exception scope also catches conversion getters and ToNumber.
static RefPtr<Observable> convertInput(Subscriber& subscriber, JSC::JSValue value)
{
    auto& globalObject = *subscriber.scriptExecutionContext()->globalObject();
    auto scope = DECLARE_THROW_SCOPE(globalObject.vm());
    auto result = Observable::from(globalObject, value);
    if (result.hasException()) {
        propagateException(globalObject, scope, result.releaseException());
        return nullptr;
    }
    return result.releaseReturnValue();
}

static bool forwardException(Subscriber& subscriber, JSC::TopExceptionScope& scope)
{
    if (!scope.exception())
        return false;
    auto reason = scope.exception()->value();
    scope.clearException();
    subscriber.error(reason);
    return true;
}

ExceptionOr<Ref<Observable>> Observable::takeUntil(JSDOMGlobalObject& globalObject, JSC::JSValue value)
{
    auto result = from(globalObject, value);
    if (result.hasException())
        return result.releaseException();
    Ref notifier = result.releaseReturnValue();
    auto sources = collectSharedSources({ this, notifier.ptr() });
    Ref context = *globalObject.scriptExecutionContext();
    auto callback = ObservableNativeCallback::create(context, { Ref { *this }, notifier.copyRef() }, { },
        [source = Ref { *this }, notifier = WTF::move(notifier)](Subscriber& subscriber) {
            Ref context = *subscriber.scriptExecutionContext();
            // A synchronous notifier can prevent the source subscription entirely.
            follow(notifier, subscriber, ObservableNativeObserver::create(context, subscriber,
                [subscriber = Ref { subscriber }](JSC::JSValue) { subscriber->complete(); },
                [subscriber = Ref { subscriber }](JSC::JSValue) { subscriber->complete(); },
                [] { }));
            follow(source, subscriber, ObservableNativeObserver::forward(context, subscriber));
        });
    return create(context, WTF::move(callback), WTF::move(sources));
}

class ObservableFlattenAlgorithm final : public ObservableAlgorithm {
public:
    static Ref<ObservableFlattenAlgorithm> create(Subscriber& subscriber, MapperCallback& mapper, bool switching)
    {
        return adoptRef(*new ObservableFlattenAlgorithm(subscriber, mapper, switching));
    }

    void next(JSC::JSValue value)
    {
        RefPtr subscriber = m_subscriber;
        if (!subscriber || !subscriber->isActive())
            return;
        if (m_switching) {
            replace(value);
            return;
        }
        auto* globalObject = subscriber->scriptExecutionContext()->globalObject();
        auto entry = makeUnique<JSValueInWrappedObject>();
        auto* wrapper = toJS(globalObject, globalObject, *subscriber).asCell();
        {
            Locker locker { m_lock };
            entry->set(*globalObject, wrapper, value);
            m_queue.append(WTF::move(entry));
        }
        drain();
    }

    void complete()
    {
        m_outerDone = true;
        if (!m_switching)
            drain();
        else if (!m_innerActive) {
            if (RefPtr subscriber = m_subscriber)
                subscriber->complete();
        }
    }

    void clear()
    {
        Locker locker { m_lock };
        m_queue.clear();
    }

    void visit(JSC::AbstractSlotVisitor& visitor) const final
    {
        m_mapper->visitJSFunctionInGCThread(visitor);
        Locker locker { m_lock };
        for (auto& value : m_queue)
            value->visitInGCThread(visitor);
        visitor.addOpaqueRoot(m_innerSignal.get());
    }

private:
    ObservableFlattenAlgorithm(Subscriber& subscriber, MapperCallback& mapper, bool switching)
        : m_subscriber(subscriber)
        , m_mapper(mapper)
        , m_switching(switching)
    { }

    RefPtr<Observable> map(Subscriber& subscriber, JSC::JSValue value, uint64_t generation)
    {
        auto* globalObject = subscriber.scriptExecutionContext()->globalObject();
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(globalObject->vm());
        auto result = m_mapper->invokeRethrowingException(value, m_index++);
        if (forwardException(subscriber, scope) || result.type() != CallbackResultType::Success)
            return nullptr;
        if (!subscriber.isActive() || generation != m_generation)
            return nullptr;
        auto observable = convertInput(subscriber, result.releaseReturnValue());
        if (forwardException(subscriber, scope) || !subscriber.isActive() || generation != m_generation)
            return nullptr;
        return observable;
    }

    void subscribeInner(Subscriber& subscriber, Observable& inner, uint64_t generation, RefPtr<AbortSignal>&& signal)
    {
        auto observer = ObservableNativeObserver::create(*subscriber.scriptExecutionContext(), subscriber,
            [subscriber = Ref { subscriber }, state = Ref { *this }, generation](JSC::JSValue value) {
                if (generation == state->m_generation)
                    subscriber->next(value);
            },
            [subscriber = Ref { subscriber }, state = Ref { *this }, generation](JSC::JSValue reason) {
                if (generation == state->m_generation)
                    subscriber->error(reason);
            },
            [state = Ref { *this }, generation] {
                if (generation != state->m_generation)
                    return;
                state->m_innerActive = false;
                RefPtr signal = state->replaceInnerSignal(nullptr);
                if (signal)
                    signal->signalAbort(JSC::jsUndefined());
                if (!state->m_switching)
                    state->drain();
                else if (state->m_outerDone) {
                    if (RefPtr subscriber = state->m_subscriber)
                        subscriber->complete();
                }
            });
        follow(inner, subscriber, WTF::move(observer), WTF::move(signal));
    }

    RefPtr<AbortSignal> replaceInnerSignal(RefPtr<AbortSignal>&& signal)
    {
        Locker locker { m_lock };
        return std::exchange(m_innerSignal, WTF::move(signal));
    }

    void replace(JSC::JSValue value)
    {
        RefPtr subscriber = m_subscriber;
        auto generation = ++m_generation;
        auto signal = AbortSignal::create(subscriber->scriptExecutionContext());
        signal->signalFollow(subscriber->signal());
        // Publish replacement identity before old cleanup can emit another value.
        RefPtr previous = replaceInnerSignal(signal.copyRef());
        m_innerActive = true;
        if (previous)
            previous->signalAbort(JSC::jsUndefined());
        if (!subscriber->isActive() || generation != m_generation)
            return;
        RefPtr inner = map(*subscriber, value, generation);
        if (inner)
            subscribeInner(*subscriber, *inner, generation, WTF::move(signal));
    }

    void drain()
    {
        RefPtr subscriber = m_subscriber;
        if (!subscriber || !subscriber->isActive() || m_innerActive)
            return;
        std::unique_ptr<JSValueInWrappedObject> entry;
        {
            Locker locker { m_lock };
            if (!m_queue.isEmpty())
                entry = m_queue.takeFirst();
        }
        if (!entry) {
            if (m_outerDone)
                subscriber->complete();
            return;
        }
        auto* globalObject = subscriber->scriptExecutionContext()->globalObject();
        JSC::Strong<JSC::Unknown> value(globalObject->vm(), entry->getValue());
        m_innerActive = true;
        RefPtr inner = map(*subscriber, value.get(), m_generation);
        // Completion starts queued work synchronously, before the completed
        // producer resumes after its complete() call.
        if (inner)
            subscribeInner(*subscriber, *inner, m_generation, nullptr);
    }

    WeakPtr<Subscriber> m_subscriber;
    const Ref<MapperCallback> m_mapper;
    const bool m_switching;
    bool m_innerActive { false };
    bool m_outerDone { false };
    uint64_t m_index { 0 };
    uint64_t m_generation { 0 };
    RefPtr<AbortSignal> m_innerSignal WTF_GUARDED_BY_LOCK(m_lock);
    mutable Lock m_lock;
    Deque<std::unique_ptr<JSValueInWrappedObject>> m_queue WTF_GUARDED_BY_LOCK(m_lock);
};

static Ref<SubscriberCallback> flattenCallback(ScriptExecutionContext& context, Observable& source, MapperCallback& mapper, bool switching)
{
    return ObservableNativeCallback::create(context, { Ref { source } }, { Ref<ActiveDOMCallback> { mapper } },
        [source = Ref { source }, mapper = Ref { mapper }, switching](Subscriber& subscriber) {
            auto algorithm = ObservableFlattenAlgorithm::create(subscriber, mapper, switching);
            subscriber.setAlgorithm(algorithm.copyRef());
            subscriber.addNativeTeardown([weakAlgorithm = WeakPtr { algorithm }] {
                if (RefPtr algorithm = weakAlgorithm)
                    algorithm->clear();
            });
            follow(source, subscriber, ObservableNativeObserver::create(*subscriber.scriptExecutionContext(), subscriber,
                [algorithm](JSC::JSValue value) { algorithm->next(value); },
                [subscriber = Ref { subscriber }](JSC::JSValue reason) { subscriber->error(reason); },
                [algorithm] {
                    algorithm->complete();
                }));
        });
}

Ref<Observable> Observable::flatMap(ScriptExecutionContext& context, MapperCallback& mapper)
{
    return create(context, flattenCallback(context, *this, mapper, false), collectSharedSources({ this }));
}

Ref<Observable> Observable::switchMap(ScriptExecutionContext& context, MapperCallback& mapper)
{
    return create(context, flattenCallback(context, *this, mapper, true), collectSharedSources({ this }));
}

Ref<Observable> Observable::catchException(ScriptExecutionContext& context, ObservableCatchCallback& callback)
{
    auto producer = ObservableNativeCallback::create(context, { Ref { *this } }, { Ref<ActiveDOMCallback> { callback } },
        [source = Ref { *this }, callback = Ref { callback }](Subscriber& subscriber) {
            follow(source, subscriber, ObservableNativeObserver::create(*subscriber.scriptExecutionContext(), subscriber,
                [subscriber = Ref { subscriber }](JSC::JSValue value) { subscriber->next(value); },
                [subscriber = Ref { subscriber }, callback](JSC::JSValue reason) {
                    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(subscriber->scriptExecutionContext()->globalObject()->vm());
                    auto result = callback->invokeRethrowingException(reason);
                    if (forwardException(subscriber, scope) || !subscriber->isActive() || result.type() != CallbackResultType::Success)
                        return;
                    auto replacement = convertInput(subscriber, result.releaseReturnValue());
                    if (forwardException(subscriber, scope) || !subscriber->isActive())
                        return;
                    follow(*replacement, subscriber, ObservableNativeObserver::forward(*subscriber->scriptExecutionContext(), subscriber));
                },
                [subscriber = Ref { subscriber }] {
                    subscriber->complete();
                }));
        });
    return create(context, WTF::move(producer), collectSharedSources({ this }));
}

Ref<Observable> Observable::finallyCallback(ScriptExecutionContext& context, VoidCallback& callback)
{
    auto producer = ObservableNativeCallback::create(context, { Ref { *this } }, { Ref<ActiveDOMCallback> { callback } },
        [source = Ref { *this }, callback = Ref { callback }](Subscriber& subscriber) {
            subscriber.addTeardown(callback.copyRef());
            follow(source, subscriber, ObservableNativeObserver::forward(*subscriber.scriptExecutionContext(), subscriber));
        });
    return create(context, WTF::move(producer), collectSharedSources({ this }));
}

} // namespace WebCore

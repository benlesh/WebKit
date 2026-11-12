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
#include "Document.h"
#include "InternalObserverDrop.h"
#include "InternalObserverFilter.h"
#include "InternalObserverFromScript.h"
#include "InternalObserverInspect.h"
#include "InternalObserverMap.h"
#include "InternalObserverTake.h"
#include "JSObservable.h"
#include "JSSubscriber.h"
#include "JSSubscriberCallback.h"
#include "JSSubscriptionObserverCallback.h"
#include "ObservableConfig.h"
#include "ObservableInspector.h"
#include "ObservableNativeCallback.h"
#include "ScriptWrappableInlines.h"
#include "SharedObservable.h"
#include "SubscribeOptions.h"
#include "Subscriber.h"
#include "SubscriptionObserver.h"
#include <JavaScriptCore/MarkedArgumentBuffer.h>
#include <JavaScriptCore/Strong.h>
#include <JavaScriptCore/TopExceptionScope.h>
#include <wtf/HashSet.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(Observable);

static Vector<Ref<SharedObservable>> uniqueSources(Vector<Ref<SharedObservable>>&& sources)
{
    HashSet<SharedObservable*> seen;
    sources.removeAllMatching([&](auto& source) {
        return !seen.add(source.ptr()).isNewEntry;
    });
    return WTF::move(sources);
}

Ref<Observable> Observable::create(ScriptExecutionContext& context, ProducerUnion&& producer)
{
    return WTF::switchOn(WTF::move(producer),
        [&](RefPtr<JSSubscriberCallback>&& callback) {
            return create(context, callback.releaseNonNull());
        },
        [&](ObservableConfig&& config) {
            return create(context, config.producer.releaseNonNull(), config.sharedSources.value_or(Vector<Ref<SharedObservable>> { }));
        });
}

Ref<Observable> Observable::create(ScriptExecutionContext& context, Ref<SubscriberCallback>&& callback)
{
    return create(context, WTF::move(callback), { });
}

Ref<Observable> Observable::create(ScriptExecutionContext& context, Ref<SubscriberCallback>&& callback, Vector<Ref<SharedObservable>>&& sources)
{
    Ref observable = adoptRef(*new Observable(context, WTF::move(callback), WTF::move(sources)));
    observable->suspendIfNeeded();
    return observable;
}

Observable::Observable(ScriptExecutionContext& context, Ref<SubscriberCallback>&& callback, Vector<Ref<SharedObservable>>&& sources)
    : ActiveDOMObject(&context)
    , m_subscriberCallback(WTF::move(callback))
    , m_sharedSources(uniqueSources(WTF::move(sources)))
{
}

Observable::~Observable() = default;

bool Observable::canSubscribe() const
{
    RefPtr context = scriptExecutionContext();
    if (!context || isContextStopped())
        return false;
    RefPtr document = dynamicDowncast<Document>(context);
    return !document || document->isFullyActive();
}

void Observable::subscribe(ScriptExecutionContext& context, ObserverUnion&& observer, SubscribeOptions&& options)
{
    WTF::switchOn(WTF::move(observer),
        [&](RefPtr<JSSubscriptionObserverCallback>&& next) {
            subscribeInternal(context, InternalObserverFromScript::create(context, next.releaseNonNull()), WTF::move(options));
        },
        [&](SubscriptionObserver&& subscription) {
            subscribeInternal(context, InternalObserverFromScript::create(context, WTF::move(subscription)), WTF::move(options));
        });
}

void Observable::subscribeInternal(ScriptExecutionContext&, Ref<InternalObserver>&& observer, SubscribeOptions&& options)
{
    if (!canSubscribe() || (options.signal && options.signal->aborted())) {
        observer->deactivate();
        return;
    }
    Ref protectedThis { *this };
    Ref context = *scriptExecutionContext();
    auto* globalObject = context->globalObject();
    JSC::JSLockHolder lock(globalObject->vm());
    RefPtr shared = dynamicDowncast<SharedObservable>(*this);
    bool retained = shared && shared->terminal() != SharedObservable::Terminal::None;
    RefPtr subscriber = shared ? shared->currentSubscriber() : nullptr;
    bool start = !retained && !subscriber;
    if (!subscriber) {
        subscriber = Subscriber::create(context, *this);
        if (shared && start)
            shared->setCurrentSubscriber(subscriber.get());
    }
    JSC::Strong<JSC::JSObject> wrapper(globalObject->vm(), JSC::asObject(toJS(globalObject, globalObject, *subscriber)));
    auto observation = ObservableObservation::create(*this, WTF::move(observer), WTF::move(options.signal));
    subscriber->addObservation(observation.copyRef());
    if (shared) {
        // A fixed, rooted snapshot permits synchronous reentrancy without mutation or queuing.
        JSC::MarkedArgumentBuffer replay;
        for (auto value : shared->replayValues())
            replay.append(value);
        RELEASE_ASSERT(!replay.hasOverflowed());
        for (auto value : replay) {
            if (!observation->active() || !canSubscribe())
                break;
            observation->next(value);
        }
    }
    if (retained) {
        subscriber->deliverRetainedTerminal(shared->terminal() == SharedObservable::Terminal::Error, shared->terminalReason());
        return;
    }
    if (!start || !subscriber->isActive())
        return;
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(globalObject->vm());
    m_subscriberCallback->invokeRethrowingException(*subscriber);
    if (auto* exception = scope.exception()) {
        auto value = exception->value();
        scope.clearException();
        subscriber->error(value);
    }
}

std::optional<Vector<Ref<SharedObservable>>> Observable::sharedSources() const
{
    if (m_sharedSources.isEmpty())
        return std::nullopt;
    return m_sharedSources;
}

Vector<Ref<SharedObservable>> Observable::collectSharedSources(std::initializer_list<Observable*> inputs)
{
    Vector<Ref<SharedObservable>> result;
    for (auto* input : inputs) {
        if (auto* shared = dynamicDowncast<SharedObservable>(*input))
            result.append(*shared);
        else
            result.appendVector(input->m_sharedSources);
    }
    return uniqueSources(WTF::move(result));
}

ExceptionOr<Ref<SharedObservable>> Observable::share(JSDOMGlobalObject& globalObject, const ShareConfig& config)
{
    RefPtr shared = dynamicDowncast<SharedObservable>(*this);
    auto settings = config.normalize(globalObject, shared ? shared->settings() : ShareSettings { });
    if (settings.hasException())
        return settings.releaseException();
    if (shared && settings.returnValue() == shared->settings())
        return shared.releaseNonNull();
    auto callback = ObservableNativeCallback::forward(*globalObject.scriptExecutionContext(), *this);
    return SharedObservable::create(*globalObject.scriptExecutionContext(), WTF::move(callback), settings.releaseReturnValue(), collectSharedSources({ this }));
}

Ref<Observable> Observable::map(ScriptExecutionContext& context, MapperCallback& mapper)
{
    return create(context, createSubscriberCallbackMap(context, *this, mapper), collectSharedSources({ this }));
}

Ref<Observable> Observable::filter(ScriptExecutionContext& context, PredicateCallback& predicate)
{
    return create(context, createSubscriberCallbackFilter(context, *this, predicate), collectSharedSources({ this }));
}

Ref<Observable> Observable::take(ScriptExecutionContext& context, uint64_t amount)
{
    return create(context, createSubscriberCallbackTake(context, *this, amount), collectSharedSources({ this }));
}

Ref<Observable> Observable::drop(ScriptExecutionContext& context, uint64_t amount)
{
    return create(context, createSubscriberCallbackDrop(context, *this, amount), collectSharedSources({ this }));
}

Ref<Observable> Observable::inspect(ScriptExecutionContext& context, InspectorUnion&& inspectorUnion)
{
    return WTF::switchOn(WTF::move(inspectorUnion),
        [&](RefPtr<JSSubscriptionObserverCallback>&& next) {
            return create(context, createSubscriberCallbackInspect(context, *this, next.releaseNonNull()), collectSharedSources({ this }));
        },
        [&](ObservableInspector&& inspector) {
            return create(context, createSubscriberCallbackInspect(context, *this, WTF::move(inspector)), collectSharedSources({ this }));
        });
}

template<typename Visitor>
void Observable::visitAdditionalChildrenInGCThread(Visitor& visitor)
{
    m_subscriberCallback->visitJSFunctionInGCThread(visitor);
    for (auto& source : m_sharedSources)
        visitor.addOpaqueRoot(source.ptr());
    if (auto* shared = dynamicDowncast<SharedObservable>(*this))
        shared->visitSharedChildrenInGCThread(visitor);
}

DEFINE_VISIT_ADDITIONAL_CHILDREN_IN_GC_THREAD(Observable);

} // namespace WebCore

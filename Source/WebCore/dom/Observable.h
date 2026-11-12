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
#include "ActiveDOMObject.h"
#include "ExceptionOr.h"
#include "ScriptWrappable.h"
#include "SubscriberCallback.h"
#include <JavaScriptCore/JSCJSValue.h>
#include <wtf/RefCounted.h>
#include <wtf/Vector.h>

namespace WebCore {
class InternalObserver;
class JSDOMGlobalObject;
class JSSubscriberCallback;
class JSSubscriptionObserverCallback;
class MapperCallback;
class ObservableCatchCallback;
class PredicateCallback;
class SharedObservable;
class Subscriber;
class VoidCallback;
struct ObservableConfig;
struct ObservableInspector;
struct ShareConfig;
struct SubscribeOptions;
struct SubscriptionObserver;

class Observable : public ActiveDOMObject, public ScriptWrappable, public RefCounted<Observable> {
    WTF_MAKE_TZONE_ALLOCATED(Observable);
public:
    using ProducerUnion = Variant<RefPtr<JSSubscriberCallback>, ObservableConfig>;
    using ObserverUnion = Variant<RefPtr<JSSubscriptionObserverCallback>, SubscriptionObserver>;
    using InspectorUnion = Variant<RefPtr<JSSubscriptionObserverCallback>, ObservableInspector>;
    static Ref<Observable> create(ScriptExecutionContext&, ProducerUnion&&);
    static Ref<Observable> create(ScriptExecutionContext&, Ref<SubscriberCallback>&&);
    static Ref<Observable> create(ScriptExecutionContext&, Ref<SubscriberCallback>&&, Vector<Ref<SharedObservable>>&&);
    virtual ~Observable();
    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }
    virtual bool isSharedObservable() const { return false; }
    bool canSubscribe() const;
    void subscribe(ScriptExecutionContext&, ObserverUnion&&, SubscribeOptions&&);
    void subscribeInternal(ScriptExecutionContext&, Ref<InternalObserver>&&, SubscribeOptions&&);
    static ExceptionOr<Ref<Observable>> from(JSDOMGlobalObject&, JSC::JSValue);
    ExceptionOr<Ref<SharedObservable>> share(JSDOMGlobalObject&, const ShareConfig&);
    ExceptionOr<Ref<Observable>> takeUntil(JSDOMGlobalObject&, JSC::JSValue);
    Ref<Observable> map(ScriptExecutionContext&, MapperCallback&);
    Ref<Observable> filter(ScriptExecutionContext&, PredicateCallback&);
    Ref<Observable> take(ScriptExecutionContext&, uint64_t);
    Ref<Observable> drop(ScriptExecutionContext&, uint64_t);
    Ref<Observable> flatMap(ScriptExecutionContext&, MapperCallback&);
    Ref<Observable> switchMap(ScriptExecutionContext&, MapperCallback&);
    Ref<Observable> inspect(ScriptExecutionContext&, InspectorUnion&&);
    Ref<Observable> catchException(ScriptExecutionContext&, ObservableCatchCallback&);
    Ref<Observable> finallyCallback(ScriptExecutionContext&, VoidCallback&);
    std::optional<Vector<Ref<SharedObservable>>> sharedSources() const;
    static Vector<Ref<SharedObservable>> collectSharedSources(std::initializer_list<Observable*>);
    template<typename Visitor> void visitAdditionalChildrenInGCThread(Visitor&);
protected:
    Observable(ScriptExecutionContext&, Ref<SubscriberCallback>&&, Vector<Ref<SharedObservable>>&&);
    void stop() override { }
    bool virtualHasPendingActivity() const override { return false; }
    const Ref<SubscriberCallback> m_subscriberCallback;
    const Vector<Ref<SharedObservable>> m_sharedSources;
};
}

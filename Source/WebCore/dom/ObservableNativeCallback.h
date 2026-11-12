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

#pragma once

#include "InternalObserver.h"
#include "Observable.h"
#include "Subscriber.h"
#include "SubscriberCallback.h"
#include <JavaScriptCore/AbstractSlotVisitor.h>
#include <JavaScriptCore/SlotVisitorInlines.h>
#include <wtf/Function.h>

namespace WebCore {

class ObservableNativeCallback final : public SubscriberCallback {
public:
    static Ref<ObservableNativeCallback> create(ScriptExecutionContext& context, Vector<Ref<Observable>>&& sources, Vector<Ref<ActiveDOMCallback>>&& callbacks, Function<void(Subscriber&)>&& invoke)
    {
        return adoptRef(*new ObservableNativeCallback(context, WTF::move(sources), WTF::move(callbacks), WTF::move(invoke)));
    }
    static Ref<SubscriberCallback> forward(ScriptExecutionContext&, Observable&);
    CallbackResult<void> invoke(Subscriber& subscriber) final
    {
        if (subscriber.isActive())
            m_invoke(subscriber);
        return { };
    }
    CallbackResult<void> invokeRethrowingException(Subscriber& subscriber) final { return invoke(subscriber); }
    void visitJSFunctionInGCThread(JSC::AbstractSlotVisitor& visitor) final { visit(visitor); }
    void visitJSFunctionInGCThread(JSC::SlotVisitor& visitor) final { visit(visitor); }
private:
    ObservableNativeCallback(ScriptExecutionContext& context, Vector<Ref<Observable>>&& sources, Vector<Ref<ActiveDOMCallback>>&& callbacks, Function<void(Subscriber&)>&& invoke)
        : SubscriberCallback(&context)
        , m_sources(WTF::move(sources))
        , m_callbacks(WTF::move(callbacks))
        , m_invoke(WTF::move(invoke))
    { }
    bool hasCallback() const final { return true; }
    template<typename Visitor> void visit(Visitor& visitor)
    {
        for (auto& source : m_sources)
            visitor.addOpaqueRoot(source.ptr());
        for (auto& callback : m_callbacks)
            callback->visitJSFunctionInGCThread(visitor);
    }
    const Vector<Ref<Observable>> m_sources;
    const Vector<Ref<ActiveDOMCallback>> m_callbacks;
    Function<void(Subscriber&)> m_invoke;
};

class ObservableNativeObserver final : public InternalObserver {
public:
    static Ref<ObservableNativeObserver> create(ScriptExecutionContext& context, Subscriber& destination, Function<void(JSC::JSValue)>&& next, Function<void(JSC::JSValue)>&& error, Function<void()>&& complete)
    {
        Ref observer = adoptRef(*new ObservableNativeObserver(context, destination, WTF::move(next), WTF::move(error), WTF::move(complete)));
        observer->suspendIfNeeded();
        return observer;
    }
    static Ref<ObservableNativeObserver> forward(ScriptExecutionContext&, Subscriber&);
    void next(JSC::JSValue value) final
    {
        if (m_destination->isActive())
            m_next(value);
    }
    void error(JSC::JSValue value) final
    {
        if (m_destination->isActive())
            m_error(value);
    }
    void complete() final
    {
        if (m_destination->isActive())
            m_complete();
    }
    void visitAdditionalChildrenInGCThread(JSC::AbstractSlotVisitor& visitor) const final
    {
        visitor.addOpaqueRoot(m_destination.ptr());
    }
private:
    ObservableNativeObserver(ScriptExecutionContext& context, Subscriber& destination, Function<void(JSC::JSValue)>&& next, Function<void(JSC::JSValue)>&& error, Function<void()>&& complete)
        : InternalObserver(context)
        , m_destination(destination)
        , m_next(WTF::move(next))
        , m_error(WTF::move(error))
        , m_complete(WTF::move(complete))
    { }
    const Ref<Subscriber> m_destination;
    Function<void(JSC::JSValue)> m_next;
    Function<void(JSC::JSValue)> m_error;
    Function<void()> m_complete;
};

} // namespace WebCore

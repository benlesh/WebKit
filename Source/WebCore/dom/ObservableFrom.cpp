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
#include "JSDOMPromise.h"
#include "JSObservable.h"
#include "JSSubscriber.h"
#include "JSValueInWrappedObjectInlines.h"
#include "ObservableAlgorithm.h"
#include "Subscriber.h"
#include <JavaScriptCore/CallData.h>
#include <JavaScriptCore/ExceptionHelpers.h>
#include <JavaScriptCore/IteratorOperations.h>
#include <JavaScriptCore/JSPromise.h>
#include <JavaScriptCore/Strong.h>
#include <JavaScriptCore/TopExceptionScope.h>

namespace WebCore {

static JSC::JSValue getObservableMethod(JSDOMGlobalObject& globalObject, JSC::JSObject& object, JSC::PropertyName property)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject.vm());
    auto method = object.get(&globalObject, property);
    RETURN_IF_EXCEPTION(scope, { });
    if (method.isUndefinedOrNull())
        return JSC::jsUndefined();
    if (JSC::getCallData(method).type == JSC::CallData::Type::None) {
        JSC::throwTypeError(&globalObject, scope, "Iterator method is not callable"_s);
        return { };
    }
    return method;
}

class ObservableIteratorAlgorithm final : public ObservableAlgorithm {
public:
    static Ref<ObservableIteratorAlgorithm> create(Subscriber& subscriber, JSC::IterationRecord record, bool async)
    {
        return adoptRef(*new ObservableIteratorAlgorithm(subscriber, record, async));
    }

    void advance()
    {
        RefPtr subscriber = m_subscriber;
        if (!subscriber || !subscriber->isActive() || m_done)
            return;
        auto* globalObject = subscriber->scriptExecutionContext()->globalObject();
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(globalObject->vm());
        do {
            auto result = JSC::iteratorNextExported(globalObject, { m_iterator.getValue(), m_next.getValue() });
            if (m_async) {
                JSC::JSPromise* promise;
                if (auto* exception = scope.exception()) {
                    auto reason = exception->value();
                    scope.clearException();
                    m_done = true;
                    promise = JSC::JSPromise::rejectedPromise(globalObject, reason);
                } else
                    promise = JSC::JSPromise::resolvedPromise(globalObject, result);
                if (fail(*subscriber, scope))
                    return;
                auto* wrapper = JSC::asObject(toJS(globalObject, globalObject, *subscriber));
                DOMPromise::whenPromiseIsSettled(globalObject, promise,
                    [state = Ref { *this }, subscriber = Ref { *subscriber }](JSDOMGlobalObject*, bool fulfilled, JSC::JSValue value) {
                        if (!fulfilled) {
                            state->m_done = true;
                            subscriber->error(value);
                            return;
                        }
                        if (!subscriber->isActive())
                            return;
                        state->accept(*subscriber, value);
                        state->advance();
                    }, wrapper);
                return;
            }
            if (fail(*subscriber, scope))
                return;
            accept(*subscriber, result);
        } while (subscriber->isActive() && !m_done);
    }

    void close()
    {
        if (std::exchange(m_done, true))
            return;
        RefPtr subscriber = m_subscriber;
        if (!subscriber || !subscriber->scriptExecutionContext())
            return;
        auto* globalObject = subscriber->scriptExecutionContext()->globalObject();
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(globalObject->vm());
        if (!m_async)
            JSC::iteratorClose(globalObject, m_iterator.getValue());
        else {
            auto method = getObservableMethod(*globalObject, *JSC::asObject(m_iterator.getValue()), globalObject->vm().propertyNames->returnKeyword);
            if (!scope.exception() && !method.isUndefined()) {
                JSC::MarkedArgumentBuffer arguments;
                auto value = JSC::call(globalObject, method, JSC::getCallData(method), m_iterator.getValue(), arguments);
                if (!scope.exception()) {
                    auto* promise = JSC::JSPromise::resolvedPromise(globalObject, value);
                    if (!scope.exception()) {
                        DOMPromise::whenPromiseIsSettled(globalObject, promise,
                            [subscriber = Ref { *subscriber }](JSDOMGlobalObject* globalObject, bool fulfilled, JSC::JSValue value) {
                                if (!fulfilled)
                                    subscriber->reportErrorObject(value);
                                else if (!value.isObject())
                                    subscriber->reportErrorObject(JSC::createTypeError(globalObject, "Iterator return result is not an object"_s));
                            }, JSC::asObject(toJS(globalObject, globalObject, *subscriber)));
                    }
                }
            }
        }
        if (auto* exception = scope.exception()) {
            auto reason = exception->value();
            scope.clearException();
            subscriber->reportErrorObject(reason);
        }
    }

    void visit(JSC::AbstractSlotVisitor& visitor) const final
    {
        m_iterator.visitInGCThread(visitor);
        m_next.visitInGCThread(visitor);
    }

private:
    ObservableIteratorAlgorithm(Subscriber& subscriber, JSC::IterationRecord record, bool async)
        : m_subscriber(subscriber)
        , m_async(async)
    {
        auto* globalObject = subscriber.scriptExecutionContext()->globalObject();
        auto* wrapper = toJS(globalObject, globalObject, subscriber).asCell();
        m_iterator.set(*globalObject, wrapper, record.iterator);
        m_next.set(*globalObject, wrapper, record.nextMethod);
    }

    bool fail(Subscriber& subscriber, JSC::TopExceptionScope& scope)
    {
        if (!scope.exception())
            return false;
        auto reason = scope.exception()->value();
        scope.clearException();
        m_done = true;
        subscriber.error(reason);
        return true;
    }

    void accept(Subscriber& subscriber, JSC::JSValue result)
    {
        auto* globalObject = subscriber.scriptExecutionContext()->globalObject();
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(globalObject->vm());
        JSC::Strong<JSC::Unknown> protectedResult(globalObject->vm(), result);
        if (!result.isObject()) {
            m_done = true;
            subscriber.error(JSC::createTypeError(globalObject, "Iterator result is not an object"_s));
            return;
        }
        bool done = JSC::iteratorCompleteExported(globalObject, result);
        if (fail(subscriber, scope))
            return;
        if (!subscriber.isActive())
            return;
        if (done) {
            m_done = true;
            subscriber.complete();
            return;
        }
        auto value = JSC::iteratorValue(globalObject, result);
        if (!fail(subscriber, scope))
            subscriber.next(value);
    }

    WeakPtr<Subscriber> m_subscriber;
    const bool m_async;
    bool m_done { false };
    JSValueInWrappedObject m_iterator;
    JSValueInWrappedObject m_next;
};

class ObservableFromCallback final : public SubscriberCallback {
public:
    enum class Kind : uint8_t { Iterable, AsyncIterable, Promise };
    static Ref<ObservableFromCallback> create(ScriptExecutionContext& context, Kind kind)
    {
        return adoptRef(*new ObservableFromCallback(context, kind));
    }
    void setInput(JSDOMGlobalObject& globalObject, Observable& owner, JSC::JSValue value)
    {
        m_input.set(globalObject, toJS(&globalObject, &globalObject, owner).asCell(), value);
    }
    CallbackResult<void> invokeRethrowingException(Subscriber& subscriber) final { return invoke(subscriber); }
    CallbackResult<void> invoke(Subscriber& subscriber) final
    {
        auto* globalObject = subscriber.scriptExecutionContext()->globalObject();
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(globalObject->vm());
        if (m_kind == Kind::Promise) {
            DOMPromise::whenPromiseIsSettled(globalObject, JSC::jsCast<JSC::JSPromise*>(m_input.getValue()),
                [subscriber = Ref { subscriber }](JSDOMGlobalObject*, bool fulfilled, JSC::JSValue value) {
                    if (fulfilled) {
                        subscriber->next(value);
                        subscriber->complete();
                    } else
                        subscriber->error(value);
                }, JSC::asObject(toJS(globalObject, globalObject, subscriber)));
        } else {
            // Reacquire the protocol and iterator on every subscription.
            auto record = m_kind == Kind::AsyncIterable
                ? JSC::getAsyncIteratorExported(*globalObject, m_input.getValue())
                : JSC::iteratorForIterable(globalObject, m_input.getValue());
            if (!scope.exception() && subscriber.isActive()) {
                auto algorithm = ObservableIteratorAlgorithm::create(subscriber, record, m_kind == Kind::AsyncIterable);
                subscriber.setAlgorithm(algorithm.copyRef());
                subscriber.signal().addAlgorithm([weakAlgorithm = WeakPtr { algorithm }](JSC::JSValue) {
                    if (RefPtr algorithm = weakAlgorithm)
                        algorithm->close();
                });
                algorithm->advance();
            }
        }
        if (auto* exception = scope.exception()) {
            auto reason = exception->value();
            scope.clearException();
            subscriber.error(reason);
        }
        return { };
    }
    void visitJSFunctionInGCThread(JSC::AbstractSlotVisitor& visitor) final { m_input.visitInGCThread(visitor); }
    void visitJSFunctionInGCThread(JSC::SlotVisitor& visitor) final { m_input.visitInGCThread(visitor); }
private:
    ObservableFromCallback(ScriptExecutionContext& context, Kind kind)
        : SubscriberCallback(&context)
        , m_kind(kind)
    { }
    bool hasCallback() const final { return true; }
    const Kind m_kind;
    JSValueInWrappedObject m_input;
};

ExceptionOr<Ref<Observable>> Observable::from(JSDOMGlobalObject& globalObject, JSC::JSValue value)
{
    auto scope = DECLARE_THROW_SCOPE(globalObject.vm());
    if (!value.isObject())
        return Exception { ExceptionCode::TypeError, "Observable.from requires an object"_s };
    if (auto* observable = JSObservable::toWrapped(globalObject.vm(), value))
        return Ref { *observable };
    auto asyncMethod = getObservableMethod(globalObject, *JSC::asObject(value), globalObject.vm().propertyNames->asyncIteratorSymbol);
    RETURN_IF_EXCEPTION(scope, Exception { ExceptionCode::ExistingExceptionError });
    auto kind = ObservableFromCallback::Kind::AsyncIterable;
    if (asyncMethod.isUndefined()) {
        auto method = getObservableMethod(globalObject, *JSC::asObject(value), globalObject.vm().propertyNames->iteratorSymbol);
        RETURN_IF_EXCEPTION(scope, Exception { ExceptionCode::ExistingExceptionError });
        kind = ObservableFromCallback::Kind::Iterable;
        if (method.isUndefined()) {
            if (!dynamicDowncast<JSC::JSPromise>(value))
                return Exception { ExceptionCode::TypeError, "Object is not an Observable, iterable, async iterable, or Promise"_s };
            kind = ObservableFromCallback::Kind::Promise;
        }
    }
    auto callback = ObservableFromCallback::create(*globalObject.scriptExecutionContext(), kind);
    auto observable = create(*globalObject.scriptExecutionContext(), callback.copyRef());
    callback->setInput(globalObject, observable, value);
    return observable;
}

} // namespace WebCore

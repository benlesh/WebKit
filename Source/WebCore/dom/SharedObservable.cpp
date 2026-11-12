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
#include "SharedObservable.h"

#include "ContextDestructionObserverInlines.h"
#include "JSDOMGlobalObject.h"
#include "JSSharedObservable.h"
#include "JSSubscriberCallback.h"
#include "JSValueInWrappedObjectInlines.h"
#include "ScriptWrappableInlines.h"
#include "SharedObservableConfig.h"
#include "Subscriber.h"
#include <JavaScriptCore/ExceptionHelpers.h>
#include <JavaScriptCore/Strong.h>
#include <cmath>
#include <wtf/TZoneMallocInlines.h>

namespace WebCore {

WTF_MAKE_TZONE_ALLOCATED_IMPL(SharedObservable);

ExceptionOr<ShareSettings> ShareConfig::normalize(JSDOMGlobalObject& globalObject, const ShareSettings& inherited) const
{
    auto scope = DECLARE_THROW_SCOPE(globalObject.vm());
    auto settings = inherited;
    if (!maxReplayValues.isUndefined()) {
        double capacity = maxReplayValues.toNumber(&globalObject);
        RETURN_IF_EXCEPTION(scope, Exception { ExceptionCode::ExistingExceptionError });
        if (!std::isfinite(capacity) || capacity < 0 || std::floor(capacity) != capacity || capacity > 9007199254740991.0)
            return Exception { ExceptionCode::RangeError, "maxReplayValues must be a non-negative safe integer"_s };
        settings.maxReplayValues = static_cast<uint64_t>(capacity);
    }
    settings.closeOnComplete = closeOnComplete.value_or(inherited.closeOnComplete);
    settings.closeOnError = closeOnError.value_or(inherited.closeOnError);
    settings.closeOnLastUnsubscribe = closeOnLastUnsubscribe.value_or(inherited.closeOnLastUnsubscribe);
    settings.keepAliveOnLastUnsubscribe = keepAliveOnLastUnsubscribe.value_or(inherited.keepAliveOnLastUnsubscribe);
    if (settings.closeOnLastUnsubscribe && settings.keepAliveOnLastUnsubscribe)
        return Exception { ExceptionCode::TypeError, "closeOnLastUnsubscribe and keepAliveOnLastUnsubscribe are mutually exclusive"_s };
    return settings;
}

ExceptionOr<Ref<SharedObservable>> SharedObservable::create(JSDOMGlobalObject& globalObject, ProducerUnion&& producer)
{
    return WTF::switchOn(WTF::move(producer),
        [&](RefPtr<JSSubscriberCallback>&& callback) -> ExceptionOr<Ref<SharedObservable>> {
            return create(*globalObject.scriptExecutionContext(), callback.releaseNonNull(), { }, { });
        },
        [&](SharedObservableConfig&& config) -> ExceptionOr<Ref<SharedObservable>> {
            auto settings = config.normalize(globalObject);
            if (settings.hasException())
                return settings.releaseException();
            return create(*globalObject.scriptExecutionContext(), config.producer.releaseNonNull(), settings.releaseReturnValue(), config.sharedSources.value_or(Vector<Ref<SharedObservable>> { }));
        });
}

Ref<SharedObservable> SharedObservable::create(ScriptExecutionContext& context, Ref<SubscriberCallback>&& callback, ShareSettings settings, Vector<Ref<SharedObservable>>&& sources)
{
    Ref result = adoptRef(*new SharedObservable(context, WTF::move(callback), settings, WTF::move(sources)));
    result->suspendIfNeeded();
    return result;
}

SharedObservable::SharedObservable(ScriptExecutionContext& context, Ref<SubscriberCallback>&& callback, ShareSettings settings, Vector<Ref<SharedObservable>>&& sources)
    : Observable(context, WTF::move(callback), WTF::move(sources))
    , m_settings(settings)
{
}

SharedObservable::~SharedObservable() = default;

void SharedObservable::setCurrentSubscriber(Subscriber* subscriber)
{
    m_currentSubscriber = subscriber;
}

void SharedObservable::retainValue(JSC::JSValue value)
{
    if (!m_settings.maxReplayValues)
        return;
    auto* globalObject = scriptExecutionContext()->globalObject();
    auto wrapper = toJS(globalObject, globalObject, *this);
    auto entry = makeUnique<JSValueInWrappedObject>();
    Locker locker { m_replayLock };
    entry->set(*globalObject, wrapper.asCell(), value);
    m_replay.append(WTF::move(entry));
    if (m_replay.size() > m_settings.maxReplayValues)
        m_replay.removeFirst();
}

Vector<JSC::JSValue> SharedObservable::replayValues() const
{
    Locker locker { m_replayLock };
    Vector<JSC::JSValue> result;
    result.reserveInitialCapacity(m_replay.size());
    for (auto& value : m_replay)
        result.append(value->getValue());
    return result;
}

JSC::JSValue SharedObservable::terminalReason() const
{
    return m_terminalReason.getValue();
}

void SharedObservable::retainTerminal(Terminal terminal, JSC::JSValue reason)
{
    m_terminal = terminal;
    auto* globalObject = scriptExecutionContext()->globalObject();
    auto wrapper = toJS(globalObject, globalObject, *this);
    Locker locker { m_replayLock };
    m_terminalReason.set(*globalObject, wrapper.asCell(), reason);
}

void SharedObservable::finish(Subscriber& subscriber, Terminal terminal, JSC::JSValue reason, bool close)
{
    ASSERT(m_currentSubscriber.get() == &subscriber);
    m_currentSubscriber = nullptr;
    bool retain = close || (terminal == Terminal::Error ? m_settings.closeOnError : m_settings.closeOnComplete);
    if (retain)
        retainTerminal(terminal, reason);
    else {
        Locker locker { m_replayLock };
        m_replay.clear();
    }
}

void SharedObservable::lastObserverLeft(Subscriber& subscriber)
{
    ASSERT(m_currentSubscriber.get() == &subscriber);
    m_currentSubscriber = nullptr;
    if (m_settings.closeOnLastUnsubscribe)
        retainTerminal(Terminal::Complete, JSC::jsUndefined());
    else {
        Locker locker { m_replayLock };
        m_replay.clear();
    }
}

void SharedObservable::stop()
{
    m_currentSubscriber = nullptr;
    Locker locker { m_replayLock };
    m_replay.clear();
    m_terminalReason.clear();
}

template<typename Visitor>
void SharedObservable::visitSharedChildrenInGCThread(Visitor& visitor)
{
    Locker locker { m_replayLock };
    for (auto& value : m_replay)
        value->visitInGCThread(visitor);
    m_terminalReason.visitInGCThread(visitor);
}

template void SharedObservable::visitSharedChildrenInGCThread(JSC::AbstractSlotVisitor&);
template void SharedObservable::visitSharedChildrenInGCThread(JSC::SlotVisitor&);

} // namespace WebCore

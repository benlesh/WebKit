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
#include "EventTarget.h"

#include "AddEventListenerOptions.h"
#include "ContextDestructionObserverInlines.h"
#include "EventTargetInlines.h"
#include "JSEvent.h"
#include "ObservableEventListenerOptions.h"
#include "ObservableNativeCallback.h"

namespace WebCore {

class ObservableEventListener final : public EventListener {
public:
    static Ref<ObservableEventListener> create(Subscriber& subscriber)
    {
        return adoptRef(*new ObservableEventListener(subscriber));
    }
    void handleEvent(ScriptExecutionContext&, Event& event) final
    {
        if (!m_subscriber->isActive())
            return;
        auto* globalObject = m_subscriber->scriptExecutionContext()->globalObject();
        JSC::JSLockHolder lock(globalObject->vm());
        m_subscriber->next(toJS(globalObject, globalObject, event));
    }
    void visitJSFunctionInGCThread(JSC::AbstractSlotVisitor& visitor) final { visitor.addOpaqueRoot(m_subscriber.ptr()); }
    void visitJSFunctionInGCThread(JSC::SlotVisitor& visitor) final { visitor.addOpaqueRoot(m_subscriber.ptr()); }
private:
    explicit ObservableEventListener(Subscriber& subscriber)
        : EventListener(NativeEventListenerType)
        , m_subscriber(subscriber)
    { }
    const Ref<Subscriber> m_subscriber;
};

Ref<Observable> EventTarget::when(ScriptExecutionContext& context, const AtomString& type, const ObservableEventListenerOptions& options)
{
    auto callback = ObservableNativeCallback::create(context, { }, { },
        [target = WeakPtr { *this }, type, options](Subscriber& subscriber) {
            RefPtr protectedTarget = target;
            if (!protectedTarget || !subscriber.isActive())
                return;
            AddEventListenerOptions listenerOptions;
            listenerOptions.capture = options.capture;
            listenerOptions.passive = options.passive;
            listenerOptions.signal = subscriber.signal();
            // Each ordinary subscription gets its own listener. The normal DOM
            // listener path supplies default passive behavior and abort removal.
            protectedTarget->addEventListenerForBindings(type, ObservableEventListener::create(subscriber), WTF::move(listenerOptions));
        });
    return Observable::create(context, WTF::move(callback));
}

} // namespace WebCore

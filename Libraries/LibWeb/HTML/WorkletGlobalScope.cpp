/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/WorkletGlobalScope.h>

namespace Web::Bindings {

JS::Realm& main_world_realm(HTML::WorkletGlobalScope const& worklet_global_scope)
{
    auto wrapper = worklet_global_scope.cached_main_world_wrapper();
    VERIFY(wrapper);
    return wrapper->shape().realm();
}

}

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(WorkletGlobalScope);

WorkletGlobalScope::WorkletGlobalScope()
{
}

WorkletGlobalScope::~WorkletGlobalScope() = default;

JS::Realm& WorkletGlobalScope::realm() const
{
    return Bindings::main_world_realm(*this);
}

EnvironmentSettingsObject& WorkletGlobalScope::settings_object() const
{
    VERIFY(m_settings_object);
    return *m_settings_object;
}

void WorkletGlobalScope::set_settings_object(Badge<WorkletEnvironmentSettingsObject>, GC::Ref<EnvironmentSettingsObject> settings_object)
{
    m_settings_object = settings_object;
}

void WorkletGlobalScope::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    UniversalGlobalScopeMixin::visit_edges(visitor);
    visitor.visit(m_settings_object);
    for (auto& timeout : m_timers)
        visitor.visit(timeout.value.completion_step);
}

void WorkletGlobalScope::finalize()
{
    for (auto& timer : m_timers)
        timer.value.timer->stop();
    m_timers.clear();
    Base::finalize();
}

// https://html.spec.whatwg.org/multipage/timers-and-user-prompts.html#run-steps-after-a-timeout
// AD-HOC: The timeout algorithm only defines Window and WorkerGlobalScope globals. Worklet globals also need internal
//         timeouts for JavaScript engine jobs, even though they don't expose the setTimeout/setInterval API.
i32 WorkletGlobalScope::run_steps_after_a_timeout(i32 timeout, GC::Ref<GC::Function<void()>> completion_step)
{
    auto timer_key = m_timer_id_allocator.allocate();
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(timeout), [this, timer_key] {
        auto timeout = m_timers.take(timer_key);
        if (!timeout.has_value())
            return;
        m_timer_id_allocator.deallocate(timer_key);
        (*timeout->completion_step)();
    });
    m_timers.set(timer_key, Timeout { timer, completion_step });
    timer->start();
    return timer_key;
}

void WorkletGlobalScope::cancel_timeout_steps(i32 timer_key)
{
    if (auto timer = m_timers.take(timer_key); timer.has_value()) {
        timer->timer->stop();
        m_timer_id_allocator.deallocate(timer_key);
    }
}

}

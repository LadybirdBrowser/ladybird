/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2024, Tim Ledbetter <timledbetter@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/HTML/Scripting/WorkerEnvironmentSettingsObject.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <WebWorker/DedicatedWorkerHost.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(WorkerEnvironmentSettingsObject);

// https://html.spec.whatwg.org/multipage/workers.html#set-up-a-worker-environment-settings-object
GC::Ref<WorkerEnvironmentSettingsObject> WorkerEnvironmentSettingsObject::setup(GC::Ref<Page> page, NonnullOwnPtr<JS::ExecutionContext> execution_context, SerializedEnvironmentSettingsObject const& outside_settings, HighResolutionTime::DOMHighResTimeStamp unsafe_worker_creation_time)
{
    // 1. Let inherited origin be outside settings's origin.
    auto inherited_origin = outside_settings.origin;

    // 2. Let realm be the value of execution context's Realm component.
    auto realm = execution_context->realm;
    VERIFY(realm);

    // 3. Let worker global scope be realm's global object.
    auto& worker = as<HTML::WorkerGlobalScope>(realm->global_object());

    // 4. Let settings object be a new environment settings object whose algorithms are defined as follows:
    // NOTE: See the functions defined for this class.
    auto settings_object = realm->create<WorkerEnvironmentSettingsObject>(move(execution_context), worker, unsafe_worker_creation_time);
    settings_object->target_browsing_context = nullptr;
    settings_object->m_origin = move(inherited_origin);

    // FIXME: 5. Set settings object's id to a new unique opaque string, creation URL to worker global scope's url, top-level creation URL to null, target browsing context to null, and active service worker to null.
    // 6. If worker global scope is a DedicatedWorkerGlobalScope object, then set settings object's top-level origin to outside settings's top-level origin.
    if (is<WebWorker::DedicatedWorkerHost>(worker)) {
        settings_object->top_level_origin = outside_settings.top_level_origin;
    }
    // FIXME: 7. Otherwise, set settings object's top-level origin to an implementation-defined value.

    // 8. Set realm's [[HostDefined]] field to settings object.
    auto intrinsics = realm->create<Bindings::Intrinsics>(*realm);
    auto host_defined = make<Bindings::PrincipalHostDefined>(settings_object, intrinsics, page);
    realm->set_host_defined(move(host_defined));

    // Non-Standard: We cannot fully initialize worker object until *after* the we set up
    //    the realm's [[HostDefined]] internal slot as the internal slot contains the web platform intrinsics
    worker.initialize_web_interfaces({});

    // 9. Return settings object.
    return settings_object;
}

URL::URL WorkerEnvironmentSettingsObject::api_base_url() const
{
    // Return worker global scope's url.
    return m_global_scope->url();
}

URL::Origin WorkerEnvironmentSettingsObject::origin() const
{
    // FIXME: Return a unique opaque origin if worker global scope's url's scheme is "data", and inherited origin otherwise.
    return m_origin;
}

GC::Ref<PolicyContainer> WorkerEnvironmentSettingsObject::policy_container() const
{
    // Return worker global scope's policy container.
    return m_global_scope->policy_container();
}

CanUseCrossOriginIsolatedAPIs WorkerEnvironmentSettingsObject::cross_origin_isolated_capability() const
{
    // FIXME: Return worker global scope's cross-origin isolated capability.
    return CanUseCrossOriginIsolatedAPIs::No;
}

double WorkerEnvironmentSettingsObject::time_origin() const
{
    // Return the result of coarsening unsafeWorkerCreationTime with worker global scope's cross-origin isolated capability.
    return HighResolutionTime::coarsen_time(m_unsafe_worker_creation_time, cross_origin_isolated_capability() == CanUseCrossOriginIsolatedAPIs::Yes);
}

void WorkerEnvironmentSettingsObject::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_global_scope);
}

}

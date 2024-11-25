/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/RootVector.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/ContentSecurityPolicy/PolicyList.h>
#include <LibWeb/ContentSecurityPolicy/SerializedPolicy.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/ShadowRealmGlobalScope.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>

namespace Web::ContentSecurityPolicy {

GC_DEFINE_ALLOCATOR(PolicyList);

GC::Ref<PolicyList> PolicyList::create(JS::Realm& realm, GC::RootVector<GC::Ref<Policy>> const& policies)
{
    auto policy_list = realm.create<PolicyList>();
    for (auto policy : policies)
        policy_list->m_policies.append(policy);
    return policy_list;
}

GC::Ref<PolicyList> PolicyList::create(JS::Realm& realm, Vector<SerializedPolicy> const& serialized_policies)
{
    auto policy_list = realm.create<PolicyList>();
    for (auto const& serialized_policy : serialized_policies) {
        auto policy = Policy::create_from_serialized_policy(realm, serialized_policy);
        policy_list->m_policies.append(policy);
    }
    return policy_list;
}

// https://w3c.github.io/webappsec-csp/#get-csp-of-object
GC::Ptr<PolicyList> PolicyList::from_object(JS::Object& object)
{
    // 1. If object is a Document return object’s policy container's CSP list.
    if (is<DOM::Document>(object)) {
        auto& document = static_cast<DOM::Document&>(object);
        return document.policy_container()->csp_list;
    }

    // 2. If object is a Window or a WorkerGlobalScope or a WorkletGlobalScope, return environment settings object’s
    //    policy container's CSP list.
    // FIXME: File a spec issue to make this look at ShadowRealmGlobalScope to support ShadowRealm.
    if (is<HTML::Window>(object) || is<HTML::WorkerGlobalScope>(object) || is<HTML::ShadowRealmGlobalScope>(object)) {
        auto& settings = HTML::relevant_principal_settings_object(object);
        return settings.policy_container()->csp_list;
    }

    // 3. Return null.
    return nullptr;
}

void PolicyList::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_policies);
}

// https://w3c.github.io/webappsec-csp/#contains-a-header-delivered-content-security-policy
bool PolicyList::contains_header_delivered_policy() const
{
    // A CSP list contains a header-delivered Content Security Policy if it contains a policy whose source is "header".
    auto header_delivered_entry = m_policies.find_if([](auto const& policy) {
        return policy->source() == Policy::Source::Header;
    });

    return !header_delivered_entry.is_end();
}

HTML::SandboxingFlagSet PolicyList::csp_derived_sandboxing_flags() const
{
    dbgln("FIXME: Implement PolicyList::csp_derived_sandboxing_flags");
    return HTML::SandboxingFlagSet {};
}

GC::Ref<PolicyList> PolicyList::clone(JS::Realm& realm) const
{
    auto policy_list = realm.create<PolicyList>();
    for (auto policy : m_policies) {
        auto cloned_policy = policy->clone(realm);
        policy_list->m_policies.append(cloned_policy);
    }
    return policy_list;
}

Vector<SerializedPolicy> PolicyList::serialize() const
{
    Vector<SerializedPolicy> serialized_policies;

    for (auto policy : m_policies) {
        serialized_policies.append(policy->serialize());
    }

    return serialized_policies;
}

}

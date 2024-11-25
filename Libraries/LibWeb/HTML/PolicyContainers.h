/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/CellAllocator.h>
#include <LibJS/Heap/Cell.h>
#include <LibURL/Forward.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/EmbedderPolicy.h>
#include <LibWeb/ReferrerPolicy/ReferrerPolicy.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/origin.html#policy-container
// A policy container is a struct containing policies that apply to a Document, a WorkerGlobalScope, or a WorkletGlobalScope. It has the following items:
struct PolicyContainer : public JS::Cell {
    GC_CELL(PolicyContainer, JS::Cell)
    GC_DECLARE_ALLOCATOR(PolicyContainer);

public:
    virtual ~PolicyContainer() = default;

    // https://html.spec.whatwg.org/multipage/origin.html#policy-container-csp-list
    // A CSP list, which is a CSP list. It is initially empty.
    GC::Ref<ContentSecurityPolicy::PolicyList> csp_list;

    // https://html.spec.whatwg.org/multipage/origin.html#policy-container-embedder-policy
    // An embedder policy, which is an embedder policy. It is initially a new embedder policy.
    EmbedderPolicy embedder_policy {};

    // https://html.spec.whatwg.org/multipage/origin.html#policy-container-referrer-policy
    // A referrer policy, which is a referrer policy. It is initially the default referrer policy.
    ReferrerPolicy::ReferrerPolicy referrer_policy { ReferrerPolicy::DEFAULT_REFERRER_POLICY };

    [[nodiscard]] GC::Ref<PolicyContainer> clone(JS::Realm&) const;
    [[nodiscard]] SerializedPolicyContainer serialize() const;

protected:
    virtual void visit_edges(Cell::Visitor&) override;

private:
    PolicyContainer(JS::Realm&);
};

// https://html.spec.whatwg.org/multipage/browsers.html#requires-storing-the-policy-container-in-history
[[nodiscard]] bool url_requires_storing_the_policy_container_in_history(URL::URL const& url);

[[nodiscard]] GC::Ref<PolicyContainer> create_a_policy_container_from_serialized_policy_container(JS::Realm&, SerializedPolicyContainer const&);

}

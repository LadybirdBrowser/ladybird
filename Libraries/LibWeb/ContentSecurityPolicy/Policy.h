/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibURL/Origin.h>
#include <LibWeb/ContentSecurityPolicy/Directives/Directive.h>
#include <LibWeb/Forward.h>

namespace Web::ContentSecurityPolicy {

#define ENUMERATE_DISPOSITION_TYPES                  \
    __ENUMERATE_DISPOSITION_TYPE(Enforce, "enforce") \
    __ENUMERATE_DISPOSITION_TYPE(Report, "report")

// https://w3c.github.io/webappsec-csp/#content-security-policy-object
// A policy defines allowed and restricted behaviors, and may be applied to a Document, WorkerGlobalScope,
// or WorkletGlobalScope.
class Policy final : public JS::Cell {
    GC_CELL(Policy, JS::Cell);
    GC_DECLARE_ALLOCATOR(Policy);

public:
    enum class Disposition {
#define __ENUMERATE_DISPOSITION_TYPE(type, _) type,
        ENUMERATE_DISPOSITION_TYPES
#undef __ENUMERATE_DISPOSITION_TYPE
    };

    enum class Source {
        Header,
        Meta,
    };

    ~Policy() = default;

    [[nodiscard]] static GC::Ref<Policy> parse_a_serialized_csp(JS::Realm&, Variant<ByteBuffer, String> serialized, Source source, Disposition disposition);
    [[nodiscard]] static GC::Ref<PolicyList> parse_a_responses_content_security_policies(JS::Realm&, GC::Ref<Fetch::Infrastructure::Response const> response);
    [[nodiscard]] static GC::Ref<Policy> create_from_serialized_policy(JS::Realm&, SerializedPolicy const&);

    [[nodiscard]] Vector<GC::Ref<Directives::Directive>> const& directives() const { return m_directives; }
    [[nodiscard]] Disposition disposition() const { return m_disposition; }
    [[nodiscard]] Source source() const { return m_source; }
    [[nodiscard]] URL::Origin const& self_origin() const { return m_self_origin; }

    [[nodiscard]] bool contains_directive_with_name(StringView name) const;

    [[nodiscard]] GC::Ref<Policy> clone(JS::Realm&) const;
    [[nodiscard]] SerializedPolicy serialize() const;

protected:
    virtual void visit_edges(Cell::Visitor&) override;

private:
    Policy() = default;

    // https://w3c.github.io/webappsec-csp/#policy-directive-set
    // Each policy has an associated directive set, which is an ordered set of directives that define the policy’s
    // implications when applied.
    Vector<GC::Ref<Directives::Directive>> m_directives;

    // https://w3c.github.io/webappsec-csp/#policy-disposition
    // Each policy has an associated disposition, which is either "enforce" or "report".
    Disposition m_disposition { Disposition::Enforce };

    // https://w3c.github.io/webappsec-csp/#policy-source
    // Each policy has an associated source, which is either "header" or "meta".
    Source m_source { Source::Header };

    // https://w3c.github.io/webappsec-csp/#policy-self-origin
    // Each policy has an associated self-origin, which is an origin that is used when matching the 'self' keyword.
    // Spec Note: This is needed to facilitate the 'self' checks of local scheme documents/workers that have inherited
    //            their policy but have an opaque origin. Most of the time this will simply be the environment settings
    //            object’s origin.
    URL::Origin m_self_origin;
};

}

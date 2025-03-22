/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/CellAllocator.h>
#include <LibJS/Heap/Cell.h>
#include <LibURL/URL.h>
#include <LibWeb/ContentSecurityPolicy/Policy.h>
#include <LibWeb/Forward.h>

namespace Web::ContentSecurityPolicy {

#define ENUMERATE_RESOURCE_TYPES                                          \
    __ENUMERATE_RESOURCE_TYPE(Inline, "inline")                           \
    __ENUMERATE_RESOURCE_TYPE(Eval, "eval")                               \
    __ENUMERATE_RESOURCE_TYPE(WasmEval, "wasm-eval")                      \
    __ENUMERATE_RESOURCE_TYPE(TrustedTypesPolicy, "trusted-types-policy") \
    __ENUMERATE_RESOURCE_TYPE(TrustedTypesSink, "trusted-types-sink")

// https://w3c.github.io/webappsec-csp/#violation
// A violation represents an action or resource which goes against the set of policy objects associated with a global
// object.
class Violation final : public JS::Cell {
    GC_CELL(Violation, JS::Cell);
    GC_DECLARE_ALLOCATOR(Violation);

public:
    enum class Resource {
#define __ENUMERATE_RESOURCE_TYPE(type, _) type,
        ENUMERATE_RESOURCE_TYPES
#undef __ENUMERATE_RESOURCE_TYPE
    };

    using ResourceType = Variant<Empty, Resource, URL::URL>;

    virtual ~Violation() = default;

    [[nodiscard]] static GC::Ref<Violation> create_a_violation_object_for_global_policy_and_directive(JS::Realm& realm, GC::Ptr<JS::Object> global_object, GC::Ref<Policy const> policy, String directive);
    [[nodiscard]] static GC::Ref<Violation> create_a_violation_object_for_request_and_policy(JS::Realm& realm, GC::Ref<Fetch::Infrastructure::Request> request, GC::Ref<Policy const>);

    // https://w3c.github.io/webappsec-csp/#violation-url
    [[nodiscard]] URL::URL url() const;

    [[nodiscard]] u16 status() const { return m_status; }
    void set_status(u16 status) { m_status = status; }

    [[nodiscard]] ResourceType const& resource() const { return m_resource; }
    void set_resource(ResourceType resource) { m_resource = resource; }

    [[nodiscard]] Optional<URL::URL> const& referrer() const { return m_referrer; }

    [[nodiscard]] Policy const& policy() const { return m_policy; }

    // https://w3c.github.io/webappsec-csp/#violation-disposition
    [[nodiscard]] Policy::Disposition disposition() const { return m_policy->disposition(); }

    [[nodiscard]] String const& effective_directive() const { return m_effective_directive; }

    [[nodiscard]] Optional<URL::URL> source_file() const { return m_source_file; }
    void set_source_file(URL::URL source_file) { m_source_file = source_file; }

    [[nodiscard]] u32 line_number() const { return m_line_number; }
    void set_line_number(u32 line_number) { m_line_number = line_number; }

    [[nodiscard]] u32 column_number() const { return m_column_number; }
    void set_column_number(u32 column_number) { m_column_number = column_number; }

    [[nodiscard]] GC::Ptr<DOM::Element> element() const { return m_element; }
    void set_element(GC::Ref<DOM::Element> element) { m_element = element; }

    [[nodiscard]] String const& sample() const { return m_sample; }
    void set_sample(String sample) { m_sample = sample; }

    void report_a_violation(JS::Realm&);

protected:
    virtual void visit_edges(Cell::Visitor&) override;

private:
    Violation(GC::Ptr<JS::Object> global_object, GC::Ref<Policy const> policy, String directive);

    [[nodiscard]] String obtain_the_blocked_uri_of_resource() const;
    [[nodiscard]] ByteBuffer obtain_the_deprecated_serialization(JS::Realm&) const;

    // https://w3c.github.io/webappsec-csp/#violation-global-object
    // Each violation has a global object, which is the global object whose policy has been violated.
    GC::Ptr<JS::Object> m_global_object;

    // https://w3c.github.io/webappsec-csp/#violation-status
    // Each violation has a status which is a non-negative integer representing the HTTP status code of the resource
    // for which the global object was instantiated.
    u16 m_status { 0 };

    // https://w3c.github.io/webappsec-csp/#violation-resource
    // Each violation has a resource, which is either null, "inline", "eval", "wasm-eval", "trusted-types-policy"
    // "trusted-types-sink" or a URL. It represents the resource which violated the policy.
    // Spec Note:  The value null for a violation’s resource is only allowed while the violation is being populated.
    //             By the time the violation is reported and its resource is used for obtaining the blocked URI, the
    //             violation’s resource should be populated with a URL or one of the allowed strings.
    ResourceType m_resource;

    // https://w3c.github.io/webappsec-csp/#violation-referrer
    // Each violation has a referrer, which is either null, or a URL. It represents the referrer of the resource whose
    // policy was violated.
    Optional<URL::URL> m_referrer;

    // https://w3c.github.io/webappsec-csp/#violation-policy
    // Each violation has a policy, which is the policy that has been violated.
    GC::Ref<Policy const> m_policy;

    // https://w3c.github.io/webappsec-csp/#violation-effective-directive
    // Each violation has an effective directive which is a non-empty string representing the directive whose enforcement
    // caused the violation.
    String m_effective_directive;

    // https://w3c.github.io/webappsec-csp/#violation-source-file
    // Each violation has a source file, which is either null or a URL.
    Optional<URL::URL> m_source_file;

    // https://w3c.github.io/webappsec-csp/#violation-line-number
    // Each violation has a line number, which is a non-negative integer.
    u32 m_line_number { 0 };

    // https://w3c.github.io/webappsec-csp/#violation-column-number
    // Each violation has a column number, which is a non-negative integer.
    u32 m_column_number { 0 };

    // https://w3c.github.io/webappsec-csp/#violation-element
    // Each violation has a element, which is either null or an element.
    GC::Ptr<DOM::Element> m_element;

    // https://w3c.github.io/webappsec-csp/#violation-sample
    // Each violation has a sample, which is a string. It is the empty string unless otherwise specified.
    String m_sample;
};

}

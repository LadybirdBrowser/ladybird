/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericLexer.h>
#include <AK/String.h>
#include <LibWeb/ContentSecurityPolicy/Directives/DirectiveFactory.h>
#include <LibWeb/ContentSecurityPolicy/Directives/SerializedDirective.h>
#include <LibWeb/ContentSecurityPolicy/Policy.h>
#include <LibWeb/ContentSecurityPolicy/PolicyList.h>
#include <LibWeb/ContentSecurityPolicy/SerializedPolicy.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Headers.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/Infra/CharacterTypes.h>
#include <LibWeb/Infra/Strings.h>

namespace Web::ContentSecurityPolicy {

GC_DEFINE_ALLOCATOR(Policy);

// https://w3c.github.io/webappsec-csp/#abstract-opdef-parse-a-serialized-csp
GC::Ref<Policy> Policy::parse_a_serialized_csp(JS::Realm& realm, Variant<ByteBuffer, String> serialized, Source source, Disposition disposition)
{
    // To parse a serialized CSP, given a byte sequence or string serialized, a source source, and a disposition disposition,
    // execute the following steps.
    // This algorithm returns a Content Security Policy object. If serialized could not be parsed, the object’s directive
    // set will be empty.

    // 1. If serialized is a byte sequence, then set serialized to be the result of isomorphic decoding serialized.
    auto serialized_string = serialized.has<String>()
        ? serialized.get<String>()
        : Infra::isomorphic_decode(serialized.get<ByteBuffer>());

    // 2. Let policy be a new policy with an empty directive set, a source of source, and a disposition of disposition.
    auto policy = realm.create<Policy>();
    policy->m_pre_parsed_policy_string = serialized_string;
    policy->m_source = source;
    policy->m_disposition = disposition;

    // 3. For each token returned by strictly splitting serialized on the U+003B SEMICOLON character (;):
    auto tokens = MUST(serialized_string.split(';', SplitBehavior::KeepEmpty));
    for (auto token : tokens) {
        // 1. Strip leading and trailing ASCII whitespace from token.
        auto stripped_token = MUST(token.trim(Infra::ASCII_WHITESPACE));
        auto stripped_token_view = stripped_token.bytes_as_string_view();

        // 2. If token is an empty string, or if token is not an ASCII string, continue.
        if (stripped_token.is_empty() || !all_of(stripped_token_view, is_ascii))
            continue;

        // 3. Let directive name be the result of collecting a sequence of code points from token which are not
        //    ASCII whitespace.
        GenericLexer lexer(stripped_token_view);
        auto directive_name = lexer.consume_until(Infra::is_ascii_whitespace);

        // 4. Set directive name to be the result of running ASCII lowercase on directive name.
        // Spec Note: Directive names are case-insensitive, that is: script-SRC 'none' and ScRiPt-sRc 'none' are
        //            equivalent.
        auto lowercase_directive_name = MUST(Infra::to_ascii_lowercase(directive_name));

        // 5. If policy’s directive set contains a directive whose name is directive name, continue.
        if (policy->contains_directive_with_name(lowercase_directive_name)) {
            // Spec Note: In this case, the user agent SHOULD notify developers that a duplicate directive was
            //            ignored. A console warning might be appropriate, for example.
            dbgln("Ignoring duplicate Content Security Policy directive: {}", lowercase_directive_name);
            continue;
        }

        // 6. Let directive value be the result of splitting token on ASCII whitespace.
        auto rest_of_the_token = lexer.consume_all();
        auto directive_value_views = rest_of_the_token.split_view_if(Infra::is_ascii_whitespace);

        Vector<String> directive_value;
        for (auto directive_value_view : directive_value_views) {
            String directive_value_entry = MUST(String::from_utf8(directive_value_view));
            directive_value.append(move(directive_value_entry));
        }

        // 7. Let directive be a new directive whose name is directive name, and value is directive value.
        auto directive = Directives::create_directive(realm, move(lowercase_directive_name), move(directive_value));

        // 8. Append directive to policy’s directive set.
        policy->m_directives.append(directive);
    }

    // 4. Return policy.
    return policy;
}

// https://w3c.github.io/webappsec-csp/#abstract-opdef-parse-a-responses-content-security-policies
GC::Ref<PolicyList> Policy::parse_a_responses_content_security_policies(JS::Realm& realm, GC::Ref<Fetch::Infrastructure::Response const> response)
{
    // To parse a response’s Content Security Policies given a response response, execute the following steps.
    // This algorithm returns a list of Content Security Policy objects. If the policies cannot be parsed,
    // the returned list will be empty.

    // 1. Let policies be an empty list.
    GC::RootVector<GC::Ref<Policy>> policies(realm.heap());

    // 2. For each token returned by extracting header list values given Content-Security-Policy and response’s header
    //    list:
    auto enforce_policy_tokens_or_failure = Fetch::Infrastructure::extract_header_list_values("Content-Security-Policy"sv.bytes(), response->header_list());
    auto enforce_policy_tokens = enforce_policy_tokens_or_failure.has<Vector<ByteBuffer>>() ? enforce_policy_tokens_or_failure.get<Vector<ByteBuffer>>() : Vector<ByteBuffer> {};
    for (auto enforce_policy_token : enforce_policy_tokens) {
        // 1. Let policy be the result of parsing token, with a source of "header", and a disposition of "enforce".
        auto policy = parse_a_serialized_csp(realm, enforce_policy_token, Policy::Source::Header, Policy::Disposition::Enforce);

        // 2. If policy’s directive set is not empty, append policy to policies.
        if (!policy->m_directives.is_empty()) {
            policies.append(policy);
        }
    }

    // 3. For each token returned by extracting header list values given Content-Security-Policy-Report-Only and
    //    response’s header list:
    auto report_policy_tokens_or_failure = Fetch::Infrastructure::extract_header_list_values("Content-Security-Policy-Report-Only"sv.bytes(), response->header_list());
    auto report_policy_tokens = report_policy_tokens_or_failure.has<Vector<ByteBuffer>>() ? report_policy_tokens_or_failure.get<Vector<ByteBuffer>>() : Vector<ByteBuffer> {};
    for (auto report_policy_token : report_policy_tokens) {
        // 1. Let policy be the result of parsing token, with a source of "header", and a disposition of "report".
        auto policy = parse_a_serialized_csp(realm, report_policy_token, Policy::Source::Header, Policy::Disposition::Report);

        // 2. If policy’s directive set is not empty, append policy to policies.
        if (!policy->m_directives.is_empty()) {
            policies.append(policy);
        }
    }

    // 4. For each policy of policies:
    for (auto& policy : policies) {
        // 1. Set policy’s self-origin to response’s url's origin.
        policy->m_self_origin = response->url()->origin();
    }

    // 5. Return policies.
    return PolicyList::create(realm, policies);
}

GC::Ref<Policy> Policy::create_from_serialized_policy(JS::Realm& realm, SerializedPolicy const& serialized_policy)
{
    auto policy = realm.create<Policy>();

    for (auto const& serialized_directive : serialized_policy.directives) {
        auto directive = Directives::create_directive(realm, serialized_directive.name, serialized_directive.value);
        policy->m_directives.append(directive);
    }

    policy->m_disposition = serialized_policy.disposition;
    policy->m_source = serialized_policy.source;
    policy->m_self_origin = serialized_policy.self_origin;
    policy->m_pre_parsed_policy_string = serialized_policy.pre_parsed_policy_string;
    return policy;
}

bool Policy::contains_directive_with_name(StringView name) const
{
    auto maybe_directive = m_directives.find_if([name](auto const& directive) {
        return directive->name() == name;
    });
    return !maybe_directive.is_end();
}

GC::Ptr<Directives::Directive> Policy::get_directive_by_name(StringView name) const
{
    auto maybe_directive = m_directives.find_if([name](auto const& directive) {
        return directive->name() == name;
    });

    if (!maybe_directive.is_end())
        return *maybe_directive;

    return nullptr;
}

GC::Ref<Policy> Policy::clone(JS::Realm& realm) const
{
    auto policy = realm.create<Policy>();

    for (auto directive : m_directives) {
        auto cloned_directive = directive->clone(realm);
        policy->m_directives.append(cloned_directive);
    }

    policy->m_disposition = m_disposition;
    policy->m_source = m_source;
    policy->m_self_origin = m_self_origin;
    policy->m_pre_parsed_policy_string = m_pre_parsed_policy_string;
    return policy;
}

SerializedPolicy Policy::serialize() const
{
    Vector<Directives::SerializedDirective> serialized_directives;

    for (auto directive : m_directives) {
        serialized_directives.append(directive->serialize());
    }

    return SerializedPolicy {
        .directives = move(serialized_directives),
        .disposition = m_disposition,
        .source = m_source,
        .self_origin = m_self_origin,
        .pre_parsed_policy_string = m_pre_parsed_policy_string,
    };
}

void Policy::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_directives);
}

}

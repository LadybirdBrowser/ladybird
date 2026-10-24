/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <LibHTTP/HeaderList.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/PermissionsPolicy/PermissionsPolicy.h>

namespace Web::PermissionsPolicy {

StringView policy_controlled_feature_token(PolicyControlledFeature feature)
{
    switch (feature) {
    case PolicyControlledFeature::Autoplay:
        return "autoplay"sv;
    case PolicyControlledFeature::Camera:
        return "camera"sv;
    case PolicyControlledFeature::CrossOriginIsolated:
        return "cross-origin-isolated"sv;
    case PolicyControlledFeature::EncryptedMedia:
        return "encrypted-media"sv;
    case PolicyControlledFeature::FocusWithoutUserActivation:
        return "focus-without-user-activation"sv;
    case PolicyControlledFeature::Fullscreen:
        return "fullscreen"sv;
    case PolicyControlledFeature::Gamepad:
        return "gamepad"sv;
    case PolicyControlledFeature::Geolocation:
        return "geolocation"sv;
    case PolicyControlledFeature::Microphone:
        return "microphone"sv;
    case PolicyControlledFeature::PictureInPicture:
        return "picture-in-picture"sv;
    case PolicyControlledFeature::WindowManagement:
        return "window-management"sv;
    }
    VERIFY_NOT_REACHED();
}

Optional<PolicyControlledFeature> policy_controlled_feature_from_token(StringView token)
{
    for (size_t i = 0; i < supported_feature_count; i++) {
        auto feature = static_cast<PolicyControlledFeature>(i);
        if (policy_controlled_feature_token(feature) == token)
            return feature;
    }
    return {};
}

DefaultAllowlist default_allowlist(PolicyControlledFeature feature)
{
    switch (feature) {
    // https://html.spec.whatwg.org/multipage/infrastructure.html#autoplay-feature
    case PolicyControlledFeature::Autoplay:
    // https://w3c.github.io/mediacapture-main/#permissions-policy-integration
    case PolicyControlledFeature::Camera:
    // https://html.spec.whatwg.org/multipage/infrastructure.html#cross-origin-isolated-feature
    case PolicyControlledFeature::CrossOriginIsolated:
    // https://w3c.github.io/encrypted-media/#permissions-policy-integration
    case PolicyControlledFeature::EncryptedMedia:
    // https://html.spec.whatwg.org/multipage/infrastructure.html#focus-without-user-activation-feature
    case PolicyControlledFeature::FocusWithoutUserActivation:
    // https://fullscreen.spec.whatwg.org/#permissions-policy-integration
    case PolicyControlledFeature::Fullscreen:
    // https://w3c.github.io/geolocation/#permissions-policy
    case PolicyControlledFeature::Geolocation:
    // https://w3c.github.io/mediacapture-main/#permissions-policy-integration
    case PolicyControlledFeature::Microphone:
    // https://w3c.github.io/window-management/#api-permission-policy-integration
    case PolicyControlledFeature::WindowManagement:
        return DefaultAllowlist::Self;
    // https://w3c.github.io/gamepad/#permission-policy
    case PolicyControlledFeature::Gamepad:
    // https://w3c.github.io/picture-in-picture/#permissions-policy
    case PolicyControlledFeature::PictureInPicture:
        return DefaultAllowlist::Star;
    }
    VERIFY_NOT_REACHED();
}

// https://w3c.github.io/webappsec-permissions-policy/#matches
bool Allowlist::matches(URL::Origin const& origin) const
{
    // 1. If the allowlist is the special value *, then return true.
    if (is_the_special_value_star)
        return true;

    // 2. If the allowlist's self-origin is not null and it is same origin-domain with origin, then return true.
    if (self_origin.has_value() && self_origin->is_same_origin_domain(origin))
        return true;

    // 3. If the allowlist's src-origin is not null and it is same origin-domain with origin, then return true.
    if (src_origin.has_value() && src_origin->is_same_origin_domain(origin))
        return true;

    // 4. If origin is an opaque origin, return false.
    if (origin.is_opaque())
        return false;

    // 5. Let url be the result of calling the url parser on the serialization of origin.
    // 6. For each permissions-source-expression item in the allowlist's expressions:
    //    1. If the result of running Does url match expression in origin with redirect count? on url, item, origin,
    //       and 0 is true then return true.
    // FIXME: Match expressions as Content Security Policy source expressions, including wildcard hosts. Only an
    //        expression naming exactly this origin matches for now.
    for (auto const& expression : expressions) {
        auto expression_url = URL::Parser::basic_parse(expression);
        if (expression_url.has_value() && expression_url->origin().is_same_origin(origin))
            return true;
    }

    // 7. Return false.
    return false;
}

// https://w3c.github.io/webappsec-permissions-policy/#empty-permissions-policy
PermissionsPolicy PermissionsPolicy::empty_permissions_policy()
{
    // An empty permissions policy is a permissions policy that has an inherited policy which contains "Enabled" for
    // every supported feature, a declared policy whose declarations and reporting configuration are both empty ordered
    // maps.
    PermissionsPolicy policy;
    policy.inherited_policy.fill(FeatureValue::Enabled);
    return policy;
}

// https://w3c.github.io/webappsec-permissions-policy/#algo-parse-policy-directive
PolicyDirective parse_policy_directive(StringView value, URL::Origin const& container_origin, Optional<URL::Origin> const& target_origin)
{
    // 1. Let directive be an empty ordered map.
    PolicyDirective directive;

    // 2. For each serialized-declaration returned by strictly splitting value on the delimiter U+003B (;):
    for (auto serialized_declaration : value.split_view(';', SplitBehavior::KeepEmpty)) {
        // 1. Let tokens be the result of splitting serialized-declaration on ASCII whitespace.
        auto tokens = serialized_declaration.split_view_if(is_ascii_space);

        // 2. If tokens is an empty list, then continue.
        if (tokens.is_empty())
            continue;

        // 3. Let feature-name be the first element of tokens.
        auto feature_name = tokens.first();

        // 4. If feature-name does not identify any recognized policy-controlled feature, then continue.
        // 5. Let feature be the policy-controlled feature identified by feature-name.
        auto feature = policy_controlled_feature_from_token(feature_name);
        if (!feature.has_value())
            continue;

        // 6. Let targetlist be the remaining elements, if any, of tokens.
        auto targetlist = tokens.span().slice(1);

        // 7. Let allowlist be a new allowlist.
        Allowlist allowlist;

        // 8. If any element of targetlist is the string "*", set allowlist to the special value *.
        if (any_of(targetlist, [](auto element) { return element == "*"sv; })) {
            allowlist.is_the_special_value_star = true;
        }
        // 9. Otherwise:
        else {
            // 1. If targetlist is empty and target origin is given, let allowlist's src-origin be target origin.
            if (targetlist.is_empty() && target_origin.has_value())
                allowlist.src_origin = target_origin;

            // 2. For each element in targetlist:
            for (auto element : targetlist) {
                // 1. If element is an ASCII case-insensitive match for "'self'":
                if (element.equals_ignoring_ascii_case("'self'"sv)) {
                    // 1. Let allowlist's self-origin be container origin.
                    allowlist.self_origin = container_origin;

                    // 2. Continue to the next element.
                    continue;
                }

                // 2. If target origin is given, and element is an ASCII case-insensitive match for "'src'":
                if (target_origin.has_value() && element.equals_ignoring_ascii_case("'src'"sv)) {
                    // 1. Let allowlist's src-origin be target origin.
                    allowlist.src_origin = target_origin;

                    // 2. Continue to the next element.
                    continue;
                }

                // 3. Let result be the result of executing the URL parser on element.
                auto result = URL::Parser::basic_parse(element);

                // 4. If result is not failure:
                if (result.has_value()) {
                    // 1. Let target be the origin of result.
                    auto target = result->origin();

                    // 2. If target is not an opaque origin, append the serialization of target to allowlist's
                    //    expressions.
                    if (!target.is_opaque())
                        allowlist.expressions.append(target.serialize());
                }
            }
        }

        // 10. Set directive[feature] to allowlist.
        directive.set(*feature, move(allowlist));
    }

    // 3. Return directive
    return directive;
}

static bool is_the_token(HTTP::StructuredFields::BareItem const& item, StringView token)
{
    auto const* item_token = item.get_pointer<HTTP::StructuredFields::Token>();
    return item_token && item_token->value == token;
}

// https://w3c.github.io/webappsec-permissions-policy/#algo-construct-policy
static DeclaredPolicy construct_policy_from_dictionary_and_origin(HTTP::StructuredFields::Dictionary const& dictionary, URL::Origin const& origin)
{
    // 1. Let declarations be an empty ordered map.
    DeclaredPolicy policy;

    // FIXME: 2. Let reporting-config be an empty ordered map.

    // 3. For each feature-name → (value, params) of dictionary:
    for (auto const& [feature_name, value] : dictionary) {
        // 1. If feature-name does not identify any recognized policy-controlled feature, then continue.
        // 2. Let feature be the policy-controlled feature identified by feature-name.
        auto feature = policy_controlled_feature_from_token(feature_name);
        if (!feature.has_value())
            continue;

        // FIXME: 3. If params["report-to"] exists, and is a token, then set reporting-config[feature] to
        //           params["report-to"].

        // 4. Let allowlist be a new allowlist.
        Allowlist allowlist;

        // 5. If value is the token *, or if value is a list which contains the token *, set allowlist to the special
        //    value *.
        auto is_the_token_star = value.visit(
            [](HTTP::StructuredFields::Item const& item) { return is_the_token(item.bare_item, "*"sv); },
            [](HTTP::StructuredFields::InnerList const& list) {
                return any_of(list.items, [](auto const& element) { return is_the_token(element.bare_item, "*"sv); });
            });
        if (is_the_token_star) {
            allowlist.is_the_special_value_star = true;
        }
        // 6. Otherwise:
        else {
            value.visit(
                // 1. If value is the token self, let allowlist's self-origin be origin.
                [&](HTTP::StructuredFields::Item const& item) {
                    if (is_the_token(item.bare_item, "self"sv))
                        allowlist.self_origin = origin;
                },
                // 2. Otherwise if value is a list, then for each element in value:
                [&](HTTP::StructuredFields::InnerList const& list) {
                    for (auto const& element : list.items) {
                        // 1. If element is the token self, let allowlist's self-origin be origin.
                        if (is_the_token(element.bare_item, "self"sv))
                            allowlist.self_origin = origin;

                        // 2. If element is a valid permissions-source-expression, append element to allowlist's
                        //    expressions.
                        // FIXME: Check that the string is a valid permissions-source-expression. Allowlist::matches()
                        //        only matches an expression that parses as a URL.
                        if (auto const* string = element.bare_item.get_pointer<ByteString>()) {
                            if (auto expression = String::from_byte_string(*string); !expression.is_error())
                                allowlist.expressions.append(expression.release_value());
                        }
                    }
                });
        }

        // 7. Set declarations[feature] to allowlist.
        policy.declarations.set(*feature, move(allowlist));
    }

    // 4. Return «declarations, reporting-config».
    return policy;
}

// https://w3c.github.io/webappsec-permissions-policy/#algo-process-response-policy
// NB: response is given as its header list, which is all these steps read of it. Report-only policies are not
//     supported.
static DeclaredPolicy process_response_policy(HTTP::HeaderList const& response_header_list, URL::Origin const& origin)
{
    // 1. Let header name be "Permissions-Policy-Report-Only" if report-only is True, or "Permissions-Policy" otherwise.
    // 2. Let parsed header be the result of executing get a structured field value given header name and "dictionary"
    //    from response's header list.
    auto parsed_header = response_header_list.get_structured_field_value<HTTP::StructuredFields::Dictionary>("Permissions-Policy"sv);

    // 3. If parsed header is null, return an empty ordered map.
    if (!parsed_header.has_value())
        return {};

    // 4. Let policy be the result of executing Construct policy from dictionary and origin on parsed header and origin.
    // 5. Return policy.
    return construct_policy_from_dictionary_and_origin(*parsed_header, origin);
}

// https://w3c.github.io/webappsec-permissions-policy/#algo-get-feature-value-for-origin
// NB: document is given as its permissions policy, which is all these steps read of it. Report-only policies are not
//     supported.
static FeatureValue get_feature_value_for_origin(PolicyControlledFeature feature, PermissionsPolicy const& document_permissions_policy, URL::Origin const& origin)
{
    // 1. Let policy be document's report-only permissions policy if report-only is True, or document's permissions
    //    policy otherwise.
    auto const& policy = document_permissions_policy;

    // 2. If policy's inherited policy for feature is "Disabled", return "Disabled".
    if (policy.inherited_policy_for_a_feature(feature) == FeatureValue::Disabled)
        return FeatureValue::Disabled;

    // 3. If feature is present in policy's declared policy:
    if (auto allowlist = policy.declared_policy.declarations.get(feature); allowlist.has_value()) {
        // 1. If policy's declared policy's declarations[feature] matches origin, then return "Enabled".
        if (allowlist->matches(origin))
            return FeatureValue::Enabled;

        // 2. Otherwise return "Disabled".
        return FeatureValue::Disabled;
    }

    // 4. Return "Enabled".
    return FeatureValue::Enabled;
}

// https://w3c.github.io/webappsec-permissions-policy/#algo-define-inherited-policy-in-container
static FeatureValue define_an_inherited_policy_for_feature_in_container_at_origin(PolicyControlledFeature feature, Optional<NavigableContainerPolicyInputs> const& container, URL::Origin const& origin)
{
    // 1. If container is null, return "Enabled".
    if (!container.has_value())
        return FeatureValue::Enabled;

    // 2. If the result of executing Get feature value for origin on feature, container's node document, container's
    //    node document's origin, and report-only is "Disabled", return "Disabled".
    if (get_feature_value_for_origin(feature, container->node_document_permissions_policy, container->node_document_origin) == FeatureValue::Disabled)
        return FeatureValue::Disabled;

    // 3. If the result of executing Get feature value for origin on feature, container's node document, origin, and
    //    report-only is "Disabled", return "Disabled".
    if (get_feature_value_for_origin(feature, container->node_document_permissions_policy, origin) == FeatureValue::Disabled)
        return FeatureValue::Disabled;

    // 4. Let container policy be the result of running Process permissions policy attributes on container.
    auto const& container_policy = container->container_policy;

    // 5. If feature exists in container policy:
    if (auto allowlist = container_policy.get(feature); allowlist.has_value()) {
        // 1. If the allowlist for feature in container policy matches origin, return "Enabled".
        if (allowlist->matches(origin))
            return FeatureValue::Enabled;

        // 2. Otherwise return "Disabled".
        return FeatureValue::Disabled;
    }

    // 6. If feature's default allowlist is *, return "Enabled".
    if (default_allowlist(feature) == DefaultAllowlist::Star)
        return FeatureValue::Enabled;

    // 7. If feature's default allowlist is 'self', and origin is same origin with container's node document's origin,
    //    return "Enabled".
    if (default_allowlist(feature) == DefaultAllowlist::Self && origin.is_same_origin(container->node_document_origin))
        return FeatureValue::Enabled;

    // 8. Otherwise return "Disabled".
    return FeatureValue::Disabled;
}

// https://w3c.github.io/webappsec-permissions-policy/#algo-create-for-navigable
PermissionsPolicy create_a_permissions_policy_for_a_navigable(Optional<NavigableContainerPolicyInputs> const& container, URL::Origin const& origin)
{
    // 1. Assert: If not null, container is a navigable container.

    // 2. Let inherited policy be a new ordered map.
    // 4. Let policy be a new permissions policy, with inherited policy inherited policy and declared policy «[], []».
    PermissionsPolicy policy;

    // 3. For each feature supported,
    for (size_t i = 0; i < supported_feature_count; i++) {
        auto feature = static_cast<PolicyControlledFeature>(i);

        // 1. Let isInherited be the result of running Define an inherited policy for feature in container at origin on
        //    feature, container and origin.
        // 2. Set inherited policy[feature] to isInherited.
        policy.inherited_policy[i] = define_an_inherited_policy_for_feature_in_container_at_origin(feature, container, origin);
    }

    // 5. Return policy.
    return policy;
}

// https://w3c.github.io/webappsec-permissions-policy/#algo-create-from-response
// NB: response is given as its header list, which is all these steps read of it.
PermissionsPolicy create_a_permissions_policy_for_a_navigable_from_response(Optional<NavigableContainerPolicyInputs> const& container, URL::Origin const& origin, HTTP::HeaderList const& response_header_list)
{
    // 1. Let policy be the result of running Create a Permissions Policy for a navigable given container and origin.
    auto policy = create_a_permissions_policy_for_a_navigable(container, origin);

    // 2. Let d be the result of running Process response policy given response, origin and report-only.
    auto d = process_response_policy(response_header_list, origin);

    // 3. For each feature → allowlist of d's declarations:
    for (auto& [feature, allowlist] : d.declarations) {
        // 1. If policy's inherited policy[feature] is true, then set policy's declared policy's declarations[feature]
        //    to allowlist.
        if (policy.inherited_policy_for_a_feature(feature) == FeatureValue::Enabled)
            policy.declared_policy.declarations.set(feature, move(allowlist));
    }

    // FIXME: 4. Set policy's declared policy[feature]'s reporting configuration to d's reporting configuration.

    // 5. Return policy.
    return policy;
}

// https://w3c.github.io/webappsec-permissions-policy/#algo-check-permissions-policy
FeatureValue check_permissions_policy(PermissionsPolicy const& policy, PolicyControlledFeature feature, URL::Origin const& origin, URL::Origin const& document_origin)
{
    // 1. If policy's inherited policy for feature is "Disabled", return "Disabled".
    if (policy.inherited_policy_for_a_feature(feature) == FeatureValue::Disabled)
        return FeatureValue::Disabled;

    // 2. If feature is present in policy's declared policy:
    if (auto allowlist = policy.declared_policy.declarations.get(feature); allowlist.has_value()) {
        // 1. If policy's declared policy's declarations[feature] matches origin, then return "Enabled".
        if (allowlist->matches(origin))
            return FeatureValue::Enabled;

        // 2. Otherwise return "Disabled".
        return FeatureValue::Disabled;
    }

    // 3. If feature's default allowlist is *, return "Enabled".
    if (default_allowlist(feature) == DefaultAllowlist::Star)
        return FeatureValue::Enabled;

    // 4. If feature's default allowlist is 'self', and origin is same origin with document origin, return "Enabled".
    if (default_allowlist(feature) == DefaultAllowlist::Self && origin.is_same_origin(document_origin))
        return FeatureValue::Enabled;

    // 5. Return "Disabled".
    return FeatureValue::Disabled;
}

}

namespace Web::PermissionsPolicy {

ErrorOr<void> encode_policy_directive(IPC::Encoder& encoder, PolicyDirective const& directive)
{
    TRY(encoder.encode_size(directive.size()));
    for (auto const& [feature, allowlist] : directive) {
        TRY(encoder.encode(to_underlying(feature)));
        TRY(encoder.encode(allowlist));
    }
    return {};
}

ErrorOr<PolicyDirective> decode_policy_directive(IPC::Decoder& decoder)
{
    auto size = TRY(decoder.decode_size());
    if (size > supported_feature_count)
        return Error::from_string_literal("IPC decode: Too many policy directive entries");
    PolicyDirective directive;
    for (size_t i = 0; i < size; i++) {
        auto feature = TRY(decoder.decode<u8>());
        if (feature >= supported_feature_count)
            return Error::from_string_literal("IPC decode: Invalid policy-controlled feature");
        directive.set(static_cast<PolicyControlledFeature>(feature), TRY(decoder.decode<Allowlist>()));
    }
    return directive;
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::PermissionsPolicy::Allowlist const& allowlist)
{
    TRY(encoder.encode(allowlist.is_the_special_value_star));
    TRY(encoder.encode(allowlist.expressions));
    TRY(encoder.encode(allowlist.self_origin));
    TRY(encoder.encode(allowlist.src_origin));
    return {};
}

template<>
ErrorOr<Web::PermissionsPolicy::Allowlist> decode(Decoder& decoder)
{
    return Web::PermissionsPolicy::Allowlist {
        .is_the_special_value_star = TRY(decoder.decode<bool>()),
        .expressions = TRY(decoder.decode<Vector<String>>()),
        .self_origin = TRY(decoder.decode<Optional<URL::Origin>>()),
        .src_origin = TRY(decoder.decode<Optional<URL::Origin>>()),
    };
}

template<>
ErrorOr<void> encode(Encoder& encoder, Web::PermissionsPolicy::PermissionsPolicy const& policy)
{
    for (auto value : policy.inherited_policy)
        TRY(encoder.encode(value == Web::PermissionsPolicy::FeatureValue::Enabled));
    TRY(Web::PermissionsPolicy::encode_policy_directive(encoder, policy.declared_policy.declarations));
    return {};
}

template<>
ErrorOr<Web::PermissionsPolicy::PermissionsPolicy> decode(Decoder& decoder)
{
    Web::PermissionsPolicy::PermissionsPolicy policy;
    for (auto& value : policy.inherited_policy)
        value = TRY(decoder.decode<bool>()) ? Web::PermissionsPolicy::FeatureValue::Enabled : Web::PermissionsPolicy::FeatureValue::Disabled;
    policy.declared_policy.declarations = TRY(Web::PermissionsPolicy::decode_policy_directive(decoder));
    return policy;
}

}

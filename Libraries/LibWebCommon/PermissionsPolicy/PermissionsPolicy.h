/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibHTTP/Forward.h>
#include <LibIPC/Forward.h>
#include <LibURL/Origin.h>
#include <LibWebCommon/Export.h>

namespace Web::PermissionsPolicy {

// https://w3c.github.io/webappsec-permissions-policy/#policy-controlled-feature
// NB: These are the supported features.
enum class PolicyControlledFeature : u8 {
    Autoplay,
    Camera,
    CrossOriginIsolated,
    EncryptedMedia,
    FocusWithoutUserActivation,
    Fullscreen,
    Gamepad,
    Microphone,
    PictureInPicture,
    WindowManagement,
};

static constexpr size_t supported_feature_count = to_underlying(PolicyControlledFeature::WindowManagement) + 1;

WEBCOMMON_API StringView policy_controlled_feature_token(PolicyControlledFeature);
WEBCOMMON_API Optional<PolicyControlledFeature> policy_controlled_feature_from_token(StringView);

// https://w3c.github.io/webappsec-permissions-policy/#default-allowlist
enum class DefaultAllowlist : u8 {
    Star,
    Self,
};

WEBCOMMON_API DefaultAllowlist default_allowlist(PolicyControlledFeature);

// https://w3c.github.io/webappsec-permissions-policy/#allowlist
struct WEBCOMMON_API Allowlist {
    // https://w3c.github.io/webappsec-permissions-policy/#the-special-value
    bool is_the_special_value_star { false };

    // https://w3c.github.io/webappsec-permissions-policy/#expressions
    Vector<String> expressions;

    // https://w3c.github.io/webappsec-permissions-policy/#self-origin
    Optional<URL::Origin> self_origin;

    // https://w3c.github.io/webappsec-permissions-policy/#src-origin
    Optional<URL::Origin> src_origin;

    bool matches(URL::Origin const&) const;
};

// https://w3c.github.io/webappsec-permissions-policy/#policy-directive
using PolicyDirective = OrderedHashMap<PolicyControlledFeature, Allowlist>;

// https://w3c.github.io/webappsec-permissions-policy/#declared-policy
struct DeclaredPolicy {
    // https://w3c.github.io/webappsec-permissions-policy/#declared-policy-declarations
    OrderedHashMap<PolicyControlledFeature, Allowlist> declarations;

    // FIXME: reporting configuration, an ordered map from features to strings.
};

enum class FeatureValue : u8 {
    Disabled,
    Enabled,
};

// https://w3c.github.io/webappsec-permissions-policy/#permissions-policy
struct WEBCOMMON_API PermissionsPolicy {
    // https://w3c.github.io/webappsec-permissions-policy/#permissions-policy-inherited-policy
    Array<FeatureValue, supported_feature_count> inherited_policy;

    // https://w3c.github.io/webappsec-permissions-policy/#permissions-policy-declared-policy
    DeclaredPolicy declared_policy;

    FeatureValue inherited_policy_for_a_feature(PolicyControlledFeature feature) const { return inherited_policy[to_underlying(feature)]; }

    static PermissionsPolicy empty_permissions_policy();
};

// What the algorithms below read of a navigable container: its node document's permissions policy and origin, and the
// container policy that processing its permissions policy attributes gives.
struct NavigableContainerPolicyInputs {
    PermissionsPolicy const& node_document_permissions_policy;
    URL::Origin const& node_document_origin;
    PolicyDirective const& container_policy;
};

WEBCOMMON_API PolicyDirective parse_policy_directive(StringView value, URL::Origin const& container_origin, Optional<URL::Origin> const& target_origin);
WEBCOMMON_API PermissionsPolicy create_a_permissions_policy_for_a_navigable(Optional<NavigableContainerPolicyInputs> const& container, URL::Origin const& origin);
WEBCOMMON_API PermissionsPolicy create_a_permissions_policy_for_a_navigable_from_response(Optional<NavigableContainerPolicyInputs> const& container, URL::Origin const& origin, HTTP::HeaderList const& response_header_list);
WEBCOMMON_API FeatureValue check_permissions_policy(PermissionsPolicy const&, PolicyControlledFeature, URL::Origin const& origin, URL::Origin const& document_origin);

WEBCOMMON_API ErrorOr<void> encode_policy_directive(IPC::Encoder&, PolicyDirective const&);
WEBCOMMON_API ErrorOr<PolicyDirective> decode_policy_directive(IPC::Decoder&);

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::PermissionsPolicy::Allowlist const&);

template<>
WEBCOMMON_API ErrorOr<Web::PermissionsPolicy::Allowlist> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::PermissionsPolicy::PermissionsPolicy const&);

template<>
WEBCOMMON_API ErrorOr<Web::PermissionsPolicy::PermissionsPolicy> decode(Decoder&);

}

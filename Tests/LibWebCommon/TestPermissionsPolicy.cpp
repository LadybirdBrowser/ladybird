/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <LibHTTP/HeaderList.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/PermissionsPolicy/PermissionsPolicy.h>

using namespace Web::PermissionsPolicy;

static URL::Origin origin_of(StringView url)
{
    return URL::Parser::basic_parse(url)->origin();
}

static bool is_enabled(PermissionsPolicy const& policy, PolicyControlledFeature feature, URL::Origin const& document_origin)
{
    return check_permissions_policy(policy, feature, document_origin, document_origin) == FeatureValue::Enabled;
}

static PermissionsPolicy policy_for_child(PermissionsPolicy const& parent_policy, URL::Origin const& parent_origin, StringView allow, URL::Origin const& child_origin)
{
    auto container_policy = parse_policy_directive(allow, parent_origin, child_origin);
    NavigableContainerPolicyInputs container { parent_policy, parent_origin, container_policy };
    return create_a_permissions_policy_for_a_navigable(container, child_origin);
}

TEST_CASE(top_level_documents_inherit_every_feature)
{
    auto origin = origin_of("https://top.example/"sv);
    auto policy = create_a_permissions_policy_for_a_navigable({}, origin);
    EXPECT(is_enabled(policy, PolicyControlledFeature::Microphone, origin));
    EXPECT(is_enabled(policy, PolicyControlledFeature::Fullscreen, origin));
}

TEST_CASE(self_default_features_are_withheld_from_cross_origin_frames)
{
    auto parent_origin = origin_of("https://top.example/"sv);
    auto parent_policy = create_a_permissions_policy_for_a_navigable({}, parent_origin);

    auto same_origin = policy_for_child(parent_policy, parent_origin, ""sv, parent_origin);
    EXPECT(is_enabled(same_origin, PolicyControlledFeature::Microphone, parent_origin));

    auto child_origin = origin_of("https://child.example/"sv);
    auto cross_origin = policy_for_child(parent_policy, parent_origin, ""sv, child_origin);
    EXPECT(!is_enabled(cross_origin, PolicyControlledFeature::Microphone, child_origin));
    EXPECT(is_enabled(cross_origin, PolicyControlledFeature::Gamepad, child_origin));
}

TEST_CASE(allow_attribute_delegates_and_withholds_features)
{
    auto parent_origin = origin_of("https://top.example/"sv);
    auto parent_policy = create_a_permissions_policy_for_a_navigable({}, parent_origin);
    auto child_origin = origin_of("https://child.example/"sv);

    auto delegated = policy_for_child(parent_policy, parent_origin, "microphone"sv, child_origin);
    EXPECT(is_enabled(delegated, PolicyControlledFeature::Microphone, child_origin));
    EXPECT(!is_enabled(delegated, PolicyControlledFeature::Camera, child_origin));

    auto named = policy_for_child(parent_policy, parent_origin, "microphone https://child.example; camera 'self'"sv, child_origin);
    EXPECT(is_enabled(named, PolicyControlledFeature::Microphone, child_origin));
    EXPECT(!is_enabled(named, PolicyControlledFeature::Camera, child_origin));

    auto withheld = policy_for_child(parent_policy, parent_origin, "microphone 'none'"sv, parent_origin);
    EXPECT(!is_enabled(withheld, PolicyControlledFeature::Microphone, parent_origin));
}

TEST_CASE(a_frame_cannot_delegate_a_feature_it_lacks)
{
    auto top_origin = origin_of("https://top.example/"sv);
    auto top_policy = create_a_permissions_policy_for_a_navigable({}, top_origin);
    auto middle_origin = origin_of("https://middle.example/"sv);
    auto middle_policy = policy_for_child(top_policy, top_origin, ""sv, middle_origin);

    auto grandchild = policy_for_child(middle_policy, middle_origin, "microphone *"sv, middle_origin);
    EXPECT(!is_enabled(grandchild, PolicyControlledFeature::Microphone, middle_origin));
}

TEST_CASE(response_header_restricts_the_document)
{
    auto origin = origin_of("https://top.example/"sv);
    auto headers = HTTP::HeaderList::create();
    headers->append(HTTP::Header::isomorphic_encode("Permissions-Policy"sv, "microphone=(), camera=self, fullscreen=(\"https://other.example\")"sv));

    auto policy = create_a_permissions_policy_for_a_navigable_from_response({}, origin, headers);
    EXPECT(!is_enabled(policy, PolicyControlledFeature::Microphone, origin));
    EXPECT(is_enabled(policy, PolicyControlledFeature::Camera, origin));
    EXPECT(!is_enabled(policy, PolicyControlledFeature::Fullscreen, origin));
    EXPECT(is_enabled(policy, PolicyControlledFeature::Gamepad, origin));
}

TEST_CASE(policies_survive_ipc)
{
    auto parent_origin = origin_of("https://top.example/"sv);
    auto parent_policy = create_a_permissions_policy_for_a_navigable({}, parent_origin);
    auto child_origin = origin_of("https://child.example/"sv);
    auto policy = policy_for_child(parent_policy, parent_origin, "camera"sv, child_origin);
    policy.declared_policy.declarations.set(PolicyControlledFeature::Camera, parse_policy_directive("camera 'self'"sv, child_origin, {}).get(PolicyControlledFeature::Camera).value());

    IPC::MessageBuffer message;
    IPC::Encoder encoder { message };
    MUST(encoder.encode(policy));
    Queue<IPC::Attachment> attachments;
    FixedMemoryStream stream { message.data().span() };
    IPC::Decoder decoder { stream, attachments };
    auto decoded = MUST(decoder.decode<PermissionsPolicy>());

    EXPECT(decoded.inherited_policy == policy.inherited_policy);
    EXPECT(is_enabled(decoded, PolicyControlledFeature::Camera, child_origin));
    EXPECT(!is_enabled(decoded, PolicyControlledFeature::Microphone, child_origin));
}

TEST_CASE(unknown_features_are_rejected_over_ipc)
{
    IPC::MessageBuffer message;
    IPC::Encoder encoder { message };
    MUST(encoder.encode_size(1));
    MUST(encoder.encode(static_cast<u8>(supported_feature_count)));
    Allowlist allowlist;
    allowlist.is_the_special_value_star = true;
    MUST(encoder.encode(allowlist));
    Queue<IPC::Attachment> attachments;
    FixedMemoryStream stream { message.data().span() };
    IPC::Decoder decoder { stream, attachments };
    EXPECT(decode_policy_directive(decoder).is_error());
}

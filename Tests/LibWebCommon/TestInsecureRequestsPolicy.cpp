/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibTest/TestCase.h>
#include <LibWebCommon/UpgradeInsecureRequests/InsecureRequestsPolicy.h>

using Web::UpgradeInsecureRequests::InsecureRequestsPolicy;

template<typename T>
static ErrorOr<InsecureRequestsPolicy> decode_policy_from(T const& value)
{
    IPC::MessageBuffer message;
    IPC::Encoder encoder { message };
    MUST(encoder.encode(value));
    Queue<IPC::Attachment> attachments;
    FixedMemoryStream stream { message.data().span() };
    IPC::Decoder decoder { stream, attachments };
    return decoder.decode<InsecureRequestsPolicy>();
}

TEST_CASE(policies_survive_ipc)
{
    for (auto policy : { InsecureRequestsPolicy::DoNotUpgrade, InsecureRequestsPolicy::Upgrade })
        EXPECT_EQ(MUST(decode_policy_from(policy)), policy);
}

TEST_CASE(a_value_the_declaration_does_not_list_is_refused)
{
    EXPECT(decode_policy_from<u8>(2).is_error());
    EXPECT(decode_policy_from<u8>(255).is_error());
}

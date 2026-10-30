/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibTest/TestCase.h>
#include <LibWebCommon/MixedContent/ProhibitsMixedSecurityContexts.h>

using Web::MixedContent::ProhibitsMixedSecurityContexts;

template<typename T>
static ErrorOr<ProhibitsMixedSecurityContexts> decode_value_from(T const& value)
{
    IPC::MessageBuffer message;
    IPC::Encoder encoder { message };
    MUST(encoder.encode(value));
    Queue<IPC::Attachment> attachments;
    FixedMemoryStream stream { message.data().span() };
    IPC::Decoder decoder { stream, attachments };
    return decoder.decode<ProhibitsMixedSecurityContexts>();
}

TEST_CASE(each_value_survives_ipc)
{
    for (auto value : { ProhibitsMixedSecurityContexts::ProhibitsMixedSecurityContexts, ProhibitsMixedSecurityContexts::DoesNotRestrictMixedSecurityContexts })
        EXPECT_EQ(MUST(decode_value_from(value)), value);
}

TEST_CASE(a_value_the_declaration_does_not_list_is_refused)
{
    EXPECT(decode_value_from<u8>(2).is_error());
    EXPECT(decode_value_from<u8>(255).is_error());
}

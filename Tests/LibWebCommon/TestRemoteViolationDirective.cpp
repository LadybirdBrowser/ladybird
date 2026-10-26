/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibTest/TestCase.h>
#include <LibWebCommon/ContentSecurityPolicy/RemoteViolationDirective.h>

using Web::ContentSecurityPolicy::RemoteViolationDirective;

template<typename T>
static ErrorOr<RemoteViolationDirective> decode_directive_from(T const& value)
{
    IPC::MessageBuffer message;
    IPC::Encoder encoder { message };
    MUST(encoder.encode(value));
    Queue<IPC::Attachment> attachments;
    FixedMemoryStream stream { message.data().span() };
    IPC::Decoder decoder { stream, attachments };
    return decoder.decode<RemoteViolationDirective>();
}

TEST_CASE(directives_survive_ipc)
{
    for (auto directive : { RemoteViolationDirective::FrameSrc, RemoteViolationDirective::ObjectSrc, RemoteViolationDirective::FormAction })
        EXPECT_EQ(MUST(decode_directive_from(directive)), directive);
}

TEST_CASE(a_value_the_declaration_does_not_list_is_refused)
{
    EXPECT(decode_directive_from<u8>(3).is_error());
    EXPECT(decode_directive_from<u8>(255).is_error());
}

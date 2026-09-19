/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <AK/Queue.h>
#include <AK/Vector.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Decoder.h>
#include <LibTest/TestCase.h>

template<typename T>
static ErrorOr<T> decode_bytes(ReadonlyBytes bytes)
{
    FixedMemoryStream stream { bytes };
    Queue<IPC::Attachment> attachments;
    IPC::Decoder decoder { stream, attachments };
    return decoder.decode<T>();
}

TEST_CASE(decode_bool)
{
    EXPECT_EQ(MUST(decode_bytes<bool>(Array<u8, 1> { 0 })), false);
    EXPECT_EQ(MUST(decode_bytes<bool>(Array<u8, 1> { 1 })), true);
    EXPECT(decode_bytes<bool>(Array<u8, 1> { 2 }).is_error());
    EXPECT(decode_bytes<bool>(Array<u8, 1> { 0xff }).is_error());
}

TEST_CASE(decode_vector_of_bool)
{
    auto vector_bytes = [](Array<u8, 3> values) {
        Vector<u8> bytes;
        u32 size = values.size();
        bytes.append(reinterpret_cast<u8 const*>(&size), sizeof(size));
        bytes.append(values.data(), values.size());
        return bytes;
    };

    EXPECT_EQ(MUST(decode_bytes<Vector<bool>>(vector_bytes({ 1, 0, 1 }))), (Vector<bool> { true, false, true }));
    EXPECT(decode_bytes<Vector<bool>>(vector_bytes({ 1, 2, 1 })).is_error());
}

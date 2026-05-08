/*
 * Copyright (c) 2026, Kevin Bortis
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/SecureByteBuffer.h>
#include <LibTest/TestCase.h>

TEST_CASE(test_secure_byte_buffer_copy_and_verify)
{
    u8 const secret[] = { 0xde, 0xad, 0xbe, 0xef, 0xca, 0xfe };
    auto buf = TRY_OR_FAIL(Crypto::SecureByteBuffer::copy(ReadonlyBytes { secret, sizeof(secret) }));

    EXPECT_EQ(buf.size(), sizeof(secret));
    EXPECT_EQ(buf.bytes(), ReadonlyBytes(secret));
}

TEST_CASE(test_secure_byte_buffer_move_constructor)
{
    u8 const secret[] = { 0x01, 0x02, 0x03, 0x04 };
    auto original = TRY_OR_FAIL(Crypto::SecureByteBuffer::copy(ReadonlyBytes { secret, sizeof(secret) }));

    auto moved = move(original);

    EXPECT_EQ(moved.size(), sizeof(secret));
    EXPECT_EQ(moved.bytes(), ReadonlyBytes(secret));
    EXPECT(original.is_empty());
}

TEST_CASE(test_secure_byte_buffer_move_assignment)
{
    u8 const first[] = { 0xaa, 0xbb };
    u8 const second[] = { 0xcc, 0xdd, 0xee };

    auto buf_a = TRY_OR_FAIL(Crypto::SecureByteBuffer::copy(ReadonlyBytes { first, sizeof(first) }));
    auto buf_b = TRY_OR_FAIL(Crypto::SecureByteBuffer::copy(ReadonlyBytes { second, sizeof(second) }));

    buf_a = move(buf_b);

    EXPECT_EQ(buf_a.size(), sizeof(second));
    EXPECT_EQ(buf_a.bytes(), ReadonlyBytes(second));
    EXPECT(buf_b.is_empty());
}

TEST_CASE(test_secure_byte_buffer_empty_default)
{
    Crypto::SecureByteBuffer buf;
    EXPECT(buf.is_empty());
    EXPECT_EQ(buf.size(), 0u);
}

TEST_CASE(test_secure_byte_buffer_create_uninitialized)
{
    auto buf = TRY_OR_FAIL(Crypto::SecureByteBuffer::create_uninitialized(64));
    EXPECT_EQ(buf.size(), 64u);
    EXPECT(!buf.is_empty());
}

TEST_CASE(test_secure_byte_buffer_copy_empty_span)
{
    auto buf = TRY_OR_FAIL(Crypto::SecureByteBuffer::copy(ReadonlyBytes { }));
    EXPECT(buf.is_empty());
    EXPECT_EQ(buf.size(), 0u);
}

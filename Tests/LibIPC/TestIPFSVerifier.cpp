/*
 * Copyright (c) 2025, the Ladybird Browser Contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibIPC/IPFSVerifier.h>
#include <AK/ByteBuffer.h>

TEST_CASE(detect_cid_version_v0)
{
    // Valid CIDv0: Starts with "Qm" and is 46 characters
    auto result = IPC::IPFSVerifier::detect_version("QmYwAPJzv5CZsnA625s3Xf2nemtYgPpHdWEz79ojWnPbdG"_string);
    EXPECT(!result.is_error());
    EXPECT_EQ(result.value(), IPC::CIDVersion::V0);
}

TEST_CASE(detect_cid_version_v1)
{
    // Valid CIDv1: Starts with "baf"
    auto result = IPC::IPFSVerifier::detect_version("bafybeigdyrzt5sfp7udm7hu76uh7y26nf3efuylqabf3oclgtqy55fbzdi"_string);
    EXPECT(!result.is_error());
    EXPECT_EQ(result.value(), IPC::CIDVersion::V1);

    auto result2 = IPC::IPFSVerifier::detect_version("bafkreigh2akiscaildcqabsyg3dfr6chu3fgpregiymsck7e7aqa4s52zy"_string);
    EXPECT(!result2.is_error());
    EXPECT_EQ(result2.value(), IPC::CIDVersion::V1);
}

TEST_CASE(detect_cid_version_invalid)
{
    // Invalid: doesn't start with Qm or baf
    auto result = IPC::IPFSVerifier::detect_version("invalid-cid-format"_string);
    EXPECT(result.is_error());

    // Invalid: starts with Qm but wrong length
    auto result2 = IPC::IPFSVerifier::detect_version("QmTooShort"_string);
    EXPECT(result2.is_error());
}

TEST_CASE(parse_cid_v0_valid)
{
    // Known CIDv0 for "hello world" text
    auto result = IPC::IPFSVerifier::parse_cid("QmWATWQ7fVPP2EFGu71UkfnqhYXDYH566qy47CnJDgvs8u"_string);
    EXPECT(!result.is_error());

    auto parsed = result.release_value();
    EXPECT_EQ(parsed.version, IPC::CIDVersion::V0);
    EXPECT_EQ(parsed.hash_algorithm, "sha256"_string);
    EXPECT_EQ(parsed.expected_hash.size(), 32u); // SHA-256 is 32 bytes
}

TEST_CASE(parse_cid_v1_valid)
{
    // Known CIDv1 (Wikipedia mirror)
    auto result = IPC::IPFSVerifier::parse_cid("bafybeigdyrzt5sfp7udm7hu76uh7y26nf3efuylqabf3oclgtqy55fbzdi"_string);
    EXPECT(!result.is_error());

    auto parsed = result.release_value();
    EXPECT_EQ(parsed.version, IPC::CIDVersion::V1);
    EXPECT_EQ(parsed.hash_algorithm, "sha256"_string);
}

TEST_CASE(hash_content_sha256)
{
    // Hash "hello world\n"
    ByteString content = "hello world\n"_string;
    auto result = IPC::IPFSVerifier::hash_content(content.bytes(), "sha256"_string);

    EXPECT(!result.is_error());
    auto hash = result.release_value();
    EXPECT_EQ(hash.size(), 32u);

    // Expected SHA-256 of "hello world\n"
    // echo -n "hello world" | sha256sum
    // a948904f2f0f479b8f8197694b30184b0d2ed1c1cd2a1ec0fb85d299a192a447
    u8 expected_hash[] = {
        0xa9, 0x48, 0x90, 0x4f, 0x2f, 0x0f, 0x47, 0x9b,
        0x8f, 0x81, 0x97, 0x69, 0x4b, 0x30, 0x18, 0x4b,
        0x0d, 0x2e, 0xd1, 0xc1, 0xcd, 0x2a, 0x1e, 0xc0,
        0xfb, 0x85, 0xd2, 0x99, 0xa1, 0x92, 0xa4, 0x47
    };

    EXPECT_EQ(memcmp(hash.data(), expected_hash, 32), 0);
}

TEST_CASE(hash_content_invalid_algorithm)
{
    ByteString content = "test"_string;
    auto result = IPC::IPFSVerifier::hash_content(content.bytes(), "invalid-algo"_string);
    EXPECT(result.is_error());
}

TEST_CASE(verify_content_matching_hash)
{
    // Create a simple CIDv0 structure for testing
    IPC::ParsedCID test_cid;
    test_cid.version = IPC::CIDVersion::V0;
    test_cid.raw_cid = "test-cid"_string;
    test_cid.hash_algorithm = "sha256"_string;

    // Hash of "hello world\n"
    u8 hash_bytes[] = {
        0xa9, 0x48, 0x90, 0x4f, 0x2f, 0x0f, 0x47, 0x9b,
        0x8f, 0x81, 0x97, 0x69, 0x4b, 0x30, 0x18, 0x4b,
        0x0d, 0x2e, 0xd1, 0xc1, 0xcd, 0x2a, 0x1e, 0xc0,
        0xfb, 0x85, 0xd2, 0x99, 0xa1, 0x92, 0xa4, 0x47
    };
    test_cid.expected_hash = MUST(ByteBuffer::copy(ReadonlyBytes { hash_bytes, 32 }));

    // Verify with matching content
    ByteString content = "hello world\n"_string;
    auto result = IPC::IPFSVerifier::verify_content(test_cid, content.bytes());

    EXPECT(!result.is_error());
    EXPECT_EQ(result.value(), true);
}

TEST_CASE(verify_content_mismatching_hash)
{
    // Create a CID with specific hash
    IPC::ParsedCID test_cid;
    test_cid.version = IPC::CIDVersion::V0;
    test_cid.raw_cid = "test-cid"_string;
    test_cid.hash_algorithm = "sha256"_string;

    // Hash of "hello world\n"
    u8 hash_bytes[] = {
        0xa9, 0x48, 0x90, 0x4f, 0x2f, 0x0f, 0x47, 0x9b,
        0x8f, 0x81, 0x97, 0x69, 0x4b, 0x30, 0x18, 0x4b,
        0x0d, 0x2e, 0xd1, 0xc1, 0xcd, 0x2a, 0x1e, 0xc0,
        0xfb, 0x85, 0xd2, 0x99, 0xa1, 0x92, 0xa4, 0x47
    };
    test_cid.expected_hash = MUST(ByteBuffer::copy(ReadonlyBytes { hash_bytes, 32 }));

    // Verify with DIFFERENT content
    ByteString wrong_content = "goodbye world\n"_string;
    auto result = IPC::IPFSVerifier::verify_content(test_cid, wrong_content.bytes());

    EXPECT(!result.is_error());
    EXPECT_EQ(result.value(), false); // Should detect mismatch
}

TEST_CASE(verify_content_cidv1_skips_verification)
{
    // CIDv1 with empty expected_hash should skip verification
    IPC::ParsedCID test_cid;
    test_cid.version = IPC::CIDVersion::V1;
    test_cid.raw_cid = "bafytest"_string;
    test_cid.hash_algorithm = "sha256"_string;
    test_cid.expected_hash = {}; // Empty

    ByteString content = "any content"_string;
    auto result = IPC::IPFSVerifier::verify_content(test_cid, content.bytes());

    EXPECT(!result.is_error());
    EXPECT_EQ(result.value(), true); // Should pass (verification skipped)
}

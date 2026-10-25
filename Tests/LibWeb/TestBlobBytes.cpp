/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/StringView.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/ImmutableBytes.h>
#include <LibGC/Root.h>
#include <LibJS/Runtime/Realm.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/FileAPI/Blob.h>

static void ensure_vm()
{
    static GC::Root<JS::Realm> realm;
    if (!realm)
        realm = Web::Bindings::create_a_simple_javascript_realm();
}

static GC::Ref<Web::FileAPI::Blob> blob_with_bytes(StringView bytes)
{
    ensure_vm();
    return Web::FileAPI::Blob::create(MUST(ByteBuffer::copy(bytes.bytes())), Utf16String {});
}

TEST_CASE(a_blob_shares_its_immutable_bytes)
{
    auto blob = blob_with_bytes("hello"sv);

    auto bytes = MUST(blob->immutable_bytes());
    EXPECT_EQ(StringView { bytes.bytes() }, "hello"sv);
    EXPECT_EQ(blob->raw_bytes().data(), bytes.bytes().data());

    auto again = MUST(blob->immutable_bytes());
    EXPECT_EQ(again.bytes().data(), bytes.bytes().data());
}

TEST_CASE(a_sliced_blob_shares_its_immutable_bytes)
{
    auto blob = blob_with_bytes("hello"sv);
    auto slice = blob->slice_blob(1, 4, {});

    auto bytes = MUST(slice->immutable_bytes());
    EXPECT_EQ(StringView { bytes.bytes() }, "ell"sv);
    EXPECT_EQ(slice->raw_bytes().data(), bytes.bytes().data());
    EXPECT_EQ(StringView { blob->raw_bytes() }, "hello"sv);
}

TEST_CASE(immutable_bytes_stay_in_a_blob_s_shared_memory)
{
    ensure_vm();
    auto buffer = MUST(Core::AnonymousBuffer::create_with_size(3));
    "abc"sv.bytes().copy_to({ buffer.data<u8>(), buffer.size() });
    auto blob = Web::FileAPI::Blob::create(buffer, Utf16String {});

    auto bytes = MUST(blob->immutable_bytes());
    EXPECT_EQ(StringView { bytes.bytes() }, "abc"sv);
    EXPECT_EQ(bytes.bytes().data(), buffer.data<u8>());

    auto shared = MUST(blob->shared_bytes());
    EXPECT_EQ(shared.data<u8>(), buffer.data<u8>());
}

TEST_CASE(shared_bytes_copy_immutable_bytes_into_shared_memory)
{
    auto blob = blob_with_bytes("hello"sv);
    auto bytes = MUST(blob->immutable_bytes());

    auto shared = MUST(blob->shared_bytes());
    EXPECT_EQ(StringView { shared.bytes() }, "hello"sv);
    EXPECT_EQ(blob->raw_bytes().data(), shared.data<u8>());
    EXPECT_EQ(StringView { bytes.bytes() }, "hello"sv);

    auto again = MUST(blob->immutable_bytes());
    EXPECT_EQ(again.bytes().data(), shared.data<u8>());
}

TEST_CASE(an_empty_blob_has_empty_immutable_bytes)
{
    auto blob = blob_with_bytes(""sv);

    auto bytes = MUST(blob->immutable_bytes());
    EXPECT(bytes.is_empty());
    EXPECT_EQ(blob->size(), 0u);
}

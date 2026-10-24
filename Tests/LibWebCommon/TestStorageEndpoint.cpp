/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16String.h>
#include <LibTest/TestCase.h>
#include <LibWebCommon/StorageAPI/StorageAreaId.h>
#include <LibWebCommon/StorageAPI/StorageEndpoint.h>

TEST_CASE(storage_quota_size_counts_utf8_bytes)
{
    EXPECT_EQ(Web::StorageAPI::storage_quota_size(Utf16String {}), 0u);
    EXPECT_EQ(Web::StorageAPI::storage_quota_size("a"_utf16), 1u);
    EXPECT_EQ(Web::StorageAPI::storage_quota_size("é"_utf16), 2u);
    EXPECT_EQ(Web::StorageAPI::storage_quota_size("\U0001f600"_utf16), 4u);
}

TEST_CASE(storage_quota_size_matches_the_utf8_encoding_without_building_it)
{
    // A lone surrogate encodes as three bytes, like to_utf8() writes it.
    char16_t const lone_surrogate[] = { u'a', 0xD800, u'b' };
    Vector<Utf16String> samples = {
        Utf16String {},
        "plain ascii key"_utf16,
        "\u00e9\u00e9 mixed \u4e2d\u6587"_utf16,
        "\U0001f600\U0001f601"_utf16,
        Utf16String::from_utf16(Utf16View { lone_surrogate, 3 }),
    };
    for (auto const& sample : samples) {
        auto encoded = sample.to_utf8();
        EXPECT_EQ(Web::StorageAPI::storage_quota_size(sample), encoded.bytes().size());
    }
    auto one = "a"_utf16;
    EXPECT_EQ(Web::StorageAPI::storage_quota_size(one.utf16_view()), 1u);
}

TEST_CASE(quota_for_registered_endpoints)
{
    EXPECT_EQ(Web::StorageAPI::quota_for(Web::StorageAPI::StorageEndpointType::LocalStorage), Optional<u64> { 5 * MiB });
    EXPECT_EQ(Web::StorageAPI::quota_for(Web::StorageAPI::StorageEndpointType::SessionStorage), Optional<u64> { 5 * MiB });
    EXPECT_EQ(Web::StorageAPI::quota_for(Web::StorageAPI::StorageEndpointType::Caches), Optional<u64> {});
}

TEST_CASE(storage_area_id_is_distinct_and_ordered)
{
    Web::StorageAPI::StorageAreaId first { 1 };
    Web::StorageAPI::StorageAreaId second { 2 };
    EXPECT(first < second);
    EXPECT(first != second);
    static_assert(!IsSame<Web::StorageAPI::StorageAreaId, u64>);
}

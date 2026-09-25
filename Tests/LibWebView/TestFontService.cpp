/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibCore/MappedFile.h>
#include <LibTest/TestCase.h>
#include <LibWebView/FontService.h>
#include <sys/mman.h>

TEST_CASE(font_catalog_is_shared_read_only)
{
    auto font_service = WebView::FontService::create({});
    auto catalog = MUST(font_service->clone_catalog());
    VERIFY(catalog.size > 0);

    // Every helper gets this descriptor. None of them may change the catalog that the others read. On Linux the
    // descriptor stays read-write and seals refuse writable mappings, elsewhere it is opened read-only.
    auto fd = catalog.file.fd();
    EXPECT_EQ(mmap(nullptr, catalog.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0), MAP_FAILED);

    auto* mapping = mmap(nullptr, catalog.size, PROT_READ, MAP_SHARED, fd, 0);
    EXPECT_NE(mapping, MAP_FAILED);
}

namespace WebView {

struct FontServiceTestAccess {
    static Gfx::BrokeredFont materialize(FontService& service, NonnullRefPtr<Gfx::TypefaceSkia> typeface, String key)
    {
        MutexLocker locker(service.m_mutex);
        MUST(service.wait_until_ready());
        return service.materialize_typeface(move(typeface), move(key));
    }
};

}

TEST_CASE(dynamic_matches_reuse_platform_faces_without_merging_equal_metadata)
{
    auto file = MUST(Core::MappedFile::map("../LibGfx/test-inputs/fonts/text.ttf"sv));
    auto first_face = MUST(Gfx::TypefaceSkia::load_from_buffer(file->bytes()));
    // Change a glyph advance without changing the family, style, collection index, or byte length.
    auto second_data = MUST(ByteBuffer::copy(file->bytes()));
    auto bytes = second_data.bytes();
    VERIFY(bytes.size() >= 12);
    size_t table_count = (static_cast<u16>(bytes[4]) << 8) | bytes[5];
    bool changed_metric = false;
    for (size_t index = 0; index < table_count; ++index) {
        size_t entry = 12 + 16 * index;
        VERIFY(entry + 16 <= bytes.size());
        if (bytes[entry] != 'h' || bytes[entry + 1] != 'm' || bytes[entry + 2] != 't' || bytes[entry + 3] != 'x')
            continue;
        u32 offset = (static_cast<u32>(bytes[entry + 8]) << 24)
            | (static_cast<u32>(bytes[entry + 9]) << 16)
            | (static_cast<u32>(bytes[entry + 10]) << 8) | bytes[entry + 11];
        VERIFY(static_cast<size_t>(offset) + 2 <= bytes.size());
        bytes[offset + 1] ^= 1;
        changed_metric = true;
        break;
    }
    VERIFY(changed_metric);
    auto second_face = MUST(Gfx::TypefaceSkia::load_from_buffer(bytes));
    EXPECT_NE(first_face->platform_typeface_id(), second_face->platform_typeface_id());
    EXPECT_EQ(first_face->family(), second_face->family());
    EXPECT_EQ(first_face->weight(), second_face->weight());
    EXPECT_EQ(first_face->width(), second_face->width());
    EXPECT_EQ(first_face->slope(), second_face->slope());
    EXPECT_EQ(first_face->collection_index(), second_face->collection_index());
    EXPECT_EQ(first_face->font_data().size(), second_face->font_data().size());

    auto service = WebView::FontService::create({});
    auto first = WebView::FontServiceTestAccess::materialize(*service, first_face, "first-match"_string);
    auto repeated = WebView::FontServiceTestAccess::materialize(*service, first_face, "second-match"_string);
    auto distinct = WebView::FontServiceTestAccess::materialize(*service, second_face, "distinct-face"_string);
    EXPECT_NE(first.face_id, 0u);
    EXPECT_NE(repeated.face_id, 0u);
    EXPECT_NE(distinct.face_id, 0u);
    EXPECT_EQ(first.face_id, repeated.face_id);
    EXPECT_NE(first.face_id, distinct.face_id);
}

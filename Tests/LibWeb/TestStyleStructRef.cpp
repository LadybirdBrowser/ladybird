/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/CSS/ComputedValues.h>

namespace Web::CSS {

// The referenced payloads are owned by the Rust side and constructed through
// the registered group vtables, so these tests also cover the FFI layout and
// lifecycle contract for real style value group types.

using TableGroup = ComputedValues::InheritedTableValues;
using TextGroup = ComputedValues::InheritedTextValues;

TEST_CASE(default_constructed_refs_share_the_default_payload)
{
    StyleStructRef<TableGroup> a;
    StyleStructRef<TableGroup> b;
    EXPECT(a.is_default());
    EXPECT(b.is_default());
    EXPECT(a.ptr_equals(b));
    EXPECT_EQ(a->border_collapse, to_underlying(InitialValues::border_collapse()));
}

TEST_CASE(access_clones_when_shared)
{
    StyleStructRef<TableGroup> a;
    StyleStructRef<TableGroup> untouched;

    a.access().caption_side = to_underlying(CaptionSide::Bottom);
    EXPECT(!a.is_default());
    EXPECT(!a.ptr_equals(untouched));
    EXPECT_EQ(a->caption_side, to_underlying(CaptionSide::Bottom));

    // The default payload must be unaffected by the mutation.
    EXPECT(untouched.is_default());
    EXPECT_EQ(untouched->caption_side, to_underlying(InitialValues::caption_side()));
    EXPECT_EQ(StyleStructRef<TableGroup>::default_value().caption_side, to_underlying(InitialValues::caption_side()));
}

TEST_CASE(access_does_not_clone_when_unique)
{
    StyleStructRef<TableGroup> a;
    auto* first = &a.access();
    auto* second = &a.access();
    EXPECT_EQ(first, second);
}

TEST_CASE(copies_share_until_mutated)
{
    StyleStructRef<TextGroup> a;
    a.access().word_break = WordBreak::BreakAll;
    a.access().text_shadow.append(ShadowData { .offset_x = 1, .offset_y = 2 });

    StyleStructRef<TextGroup> b(a);
    EXPECT(a.ptr_equals(b));

    b.access().word_break = WordBreak::KeepAll;
    EXPECT(!a.ptr_equals(b));
    EXPECT_EQ(a->word_break, WordBreak::BreakAll);
    EXPECT_EQ(b->word_break, WordBreak::KeepAll);
    EXPECT_EQ(b->text_shadow.size(), 1u);
}

TEST_CASE(assignment_shares_and_releases_old_payload)
{
    StyleStructRef<TableGroup> a;
    a.access().caption_side = to_underlying(CaptionSide::Bottom);

    StyleStructRef<TableGroup> b;
    b.access().empty_cells = to_underlying(EmptyCells::Hide);

    b = a;
    EXPECT(a.ptr_equals(b));
    EXPECT_EQ(b->caption_side, to_underlying(CaptionSide::Bottom));
    EXPECT_EQ(b->empty_cells, to_underlying(InitialValues::empty_cells()));

    auto& self_reference = b;
    b = self_reference;
    EXPECT(a.ptr_equals(b));
}

TEST_CASE(value_equality_across_distinct_payloads)
{
    StyleStructRef<TableGroup> a;
    StyleStructRef<TableGroup> b;
    a.access().border_spacing_horizontal = 9;
    b.access().border_spacing_horizontal = 9;
    EXPECT(!a.ptr_equals(b));
    EXPECT_EQ(a, b);

    b.access().border_spacing_horizontal = 10;
    EXPECT(a != b);
}

}

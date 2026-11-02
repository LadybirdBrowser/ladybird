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

using TableGroup = ComputedValues::InheritedListTableValues;
using TextGroup = ComputedValues::InheritedTextValues;

template<typename T>
static StyleStructRef<T> default_ref()
{
    StyleStructRef<T> ref;
    ref.adopt(ComputedValuesFFI::rust_style_group_create_default(T::style_group_index));
    return ref;
}

TEST_CASE(default_payloads_hold_the_initial_values)
{
    auto a = default_ref<TableGroup>();
    auto b = default_ref<TableGroup>();
    EXPECT(a.is_default());
    EXPECT(b.is_default());
    EXPECT_EQ(a, b);
    EXPECT_EQ(a->border_spacing_horizontal, 0);
}

TEST_CASE(access_clones_when_shared)
{
    auto a = default_ref<TableGroup>();
    auto untouched = a;

    a.access().border_spacing_horizontal = 7;
    EXPECT(!a.is_default());
    EXPECT(!a.ptr_equals(untouched));
    EXPECT_EQ(a->border_spacing_horizontal, 7);

    // The shared payload must be unaffected by the mutation.
    EXPECT(untouched.is_default());
    EXPECT_EQ(untouched->border_spacing_horizontal, 0);
}

TEST_CASE(access_does_not_clone_when_unique)
{
    auto a = default_ref<TableGroup>();
    auto* first = &a.access();
    auto* second = &a.access();
    EXPECT_EQ(first, second);
}

TEST_CASE(copies_share_until_mutated)
{
    auto a = default_ref<TextGroup>();
    a.access().text_transform = to_underlying(TextTransform::Uppercase);

    StyleStructRef<TextGroup> b(a);
    EXPECT(a.ptr_equals(b));

    b.access().text_transform = to_underlying(TextTransform::Lowercase);
    EXPECT(!a.ptr_equals(b));
    EXPECT_EQ(a->text_transform, to_underlying(TextTransform::Uppercase));
    EXPECT_EQ(b->text_transform, to_underlying(TextTransform::Lowercase));
    EXPECT_EQ(b->text_shadow_span().size(), 0u);
}

TEST_CASE(assignment_shares_and_releases_old_payload)
{
    auto a = default_ref<TableGroup>();
    a.access().border_spacing_horizontal = 7;

    auto b = default_ref<TableGroup>();
    b.access().border_spacing_vertical = 5;

    b = a;
    EXPECT(a.ptr_equals(b));
    EXPECT_EQ(b->border_spacing_horizontal, 7);
    EXPECT_EQ(b->border_spacing_vertical, 0);

    auto& self_reference = b;
    b = self_reference;
    EXPECT(a.ptr_equals(b));
}

TEST_CASE(value_equality_across_distinct_payloads)
{
    auto a = default_ref<TableGroup>();
    auto b = default_ref<TableGroup>();
    a.access().border_spacing_horizontal = 9;
    b.access().border_spacing_horizontal = 9;
    EXPECT(!a.ptr_equals(b));
    EXPECT_EQ(a, b);

    b.access().border_spacing_horizontal = 10;
    EXPECT(a != b);
}

TEST_CASE(empty_borrowed_reference_can_be_destroyed)
{
    StyleStructRef<TableGroup> empty;
}

TEST_CASE(empty_borrowed_reference_clones_before_mutation)
{
    auto source = default_ref<TableGroup>();
    source.access().border_spacing_horizontal = 7;
    StyleStructRef<TableGroup> borrowed;
    borrowed.borrow(&*source);
    EXPECT(borrowed.ptr_equals(source));
    borrowed.access().border_spacing_horizontal = 3;
    EXPECT(!borrowed.ptr_equals(source));
    EXPECT_EQ(source->border_spacing_horizontal, 7);
    EXPECT_EQ(borrowed->border_spacing_horizontal, 3);
}

TEST_CASE(copy_of_borrowed_reference_outlives_owner)
{
    auto copy = [] {
        auto owner = default_ref<TableGroup>();
        owner.access().border_spacing_horizontal = 7;
        StyleStructRef<TableGroup> borrowed;
        borrowed.borrow(&*owner);
        return StyleStructRef<TableGroup> { borrowed };
    }();
    EXPECT_EQ(copy->border_spacing_horizontal, 7);
    copy.access().border_spacing_horizontal = 3;
    EXPECT_EQ(copy->border_spacing_horizontal, 3);
}

TEST_CASE(assignment_initializes_empty_reference_with_owned_payload)
{
    auto source = default_ref<TableGroup>();
    source.access().border_spacing_horizontal = 7;
    StyleStructRef<TableGroup> target;
    target = source;
    EXPECT(target.ptr_equals(source));
    source.access().border_spacing_horizontal = 3;
    EXPECT_EQ(target->border_spacing_horizontal, 7);
}

TEST_CASE(assignment_acquires_ownership_of_same_borrowed_payload)
{
    auto owner = default_ref<TableGroup>();
    owner.access().border_spacing_horizontal = 7;
    StyleStructRef<TableGroup> borrowed;
    borrowed.borrow(&*owner);
    borrowed = owner;
    owner.access().border_spacing_horizontal = 3;
    EXPECT_EQ(borrowed->border_spacing_horizontal, 7);
}

}

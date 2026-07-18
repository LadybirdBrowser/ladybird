/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibTest/TestCase.h>
#include <LibWeb/CSS/StyleStructRef.h>

namespace Web::CSS {

struct TestGroup {
    int number { 1 };
    Vector<int> list;

    bool operator==(TestGroup const&) const = default;
};

TEST_CASE(default_constructed_refs_share_the_default_payload)
{
    StyleStructRef<TestGroup> a;
    StyleStructRef<TestGroup> b;
    EXPECT(a.is_default());
    EXPECT(b.is_default());
    EXPECT(a.ptr_equals(b));
    EXPECT_EQ(a->number, 1);
}

TEST_CASE(access_clones_when_shared)
{
    StyleStructRef<TestGroup> a;
    StyleStructRef<TestGroup> untouched;

    a.access().number = 42;
    EXPECT(!a.is_default());
    EXPECT(!a.ptr_equals(untouched));
    EXPECT_EQ(a->number, 42);

    // The default payload must be unaffected by the mutation.
    EXPECT(untouched.is_default());
    EXPECT_EQ(untouched->number, 1);
    EXPECT_EQ(StyleStructRef<TestGroup>::default_value().number, 1);
}

TEST_CASE(access_does_not_clone_when_unique)
{
    StyleStructRef<TestGroup> a;
    auto* first = &a.access();
    auto* second = &a.access();
    EXPECT_EQ(first, second);
}

TEST_CASE(copies_share_until_mutated)
{
    StyleStructRef<TestGroup> a;
    a.access().number = 7;
    a.access().list.append(1);

    StyleStructRef<TestGroup> b(a);
    EXPECT(a.ptr_equals(b));

    b.access().number = 8;
    EXPECT(!a.ptr_equals(b));
    EXPECT_EQ(a->number, 7);
    EXPECT_EQ(b->number, 8);
    EXPECT_EQ(b->list.size(), 1u);
}

TEST_CASE(assignment_shares_and_releases_old_payload)
{
    StyleStructRef<TestGroup> a;
    a.access().number = 5;

    StyleStructRef<TestGroup> b;
    b.access().number = 6;

    b = a;
    EXPECT(a.ptr_equals(b));
    EXPECT_EQ(b->number, 5);

    auto& self_reference = b;
    b = self_reference;
    EXPECT(a.ptr_equals(b));
}

TEST_CASE(value_equality_across_distinct_payloads)
{
    StyleStructRef<TestGroup> a;
    StyleStructRef<TestGroup> b;
    a.access().number = 9;
    b.access().number = 9;
    EXPECT(!a.ptr_equals(b));
    EXPECT_EQ(a, b);

    b.access().number = 10;
    EXPECT(a != b);
}

}

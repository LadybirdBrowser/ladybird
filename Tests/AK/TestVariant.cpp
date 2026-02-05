/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenity.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestSuite.h>

#include <AK/RefPtr.h>
#include <AK/Variant.h>

namespace {

struct Object : public RefCounted<Object> {
};

}

TEST_CASE(basic)
{
    Variant<int, ByteString> the_value { 42 };
    EXPECT(the_value.has<int>());
    EXPECT_EQ(the_value.get<int>(), 42);
    the_value = ByteString("42");
    EXPECT(the_value.has<ByteString>());
    EXPECT_EQ(the_value.get<ByteString>(), "42");
}

TEST_CASE(visit)
{
    bool correct = false;
    Variant<int, ByteString, float> the_value { 42.0f };
    the_value.visit(
        [&](int const&) { correct = false; },
        [&](ByteString const&) { correct = false; },
        [&](float const&) { correct = true; });
    EXPECT(correct);
}

TEST_CASE(visit_const)
{
    bool correct = false;
    Variant<int, ByteString> const the_value { "42"sv };

    the_value.visit(
        [&](ByteString const&) { correct = true; },
        [&](auto&) {},
        [&](auto const&) {});

    EXPECT(correct);

    correct = false;
    auto the_value_but_not_const = the_value;
    the_value_but_not_const.visit(
        [&](ByteString const&) { correct = true; },
        [&](auto&) {});

    EXPECT(correct);

    correct = false;
    the_value_but_not_const.visit(
        [&]<typename T>(T&) { correct = !IsConst<T>; });

    EXPECT(correct);
}

TEST_CASE(destructor)
{
    struct DestructionChecker {
        explicit DestructionChecker(bool& was_destroyed)
            : m_was_destroyed(was_destroyed)
        {
        }

        ~DestructionChecker()
        {
            m_was_destroyed = true;
        }
        bool& m_was_destroyed;
    };

    bool was_destroyed = false;
    {
        Variant<DestructionChecker> test_variant { DestructionChecker { was_destroyed } };
    }
    EXPECT(was_destroyed);

    bool was_destroyed_when_assigned_to = false;
    Variant<DestructionChecker, int> original { DestructionChecker { was_destroyed_when_assigned_to } };
    Variant<DestructionChecker, int> other { 42 };
    original = other;
    EXPECT(was_destroyed_when_assigned_to);
}

TEST_CASE(move_moves)
{
    struct NoCopy {
        AK_MAKE_NONCOPYABLE(NoCopy);
        AK_MAKE_DEFAULT_MOVABLE(NoCopy);

    public:
        NoCopy() = default;
    };

    Variant<NoCopy, int> first_variant { 42 };
    // Should not fail to compile
    first_variant = NoCopy {};

    Variant<NoCopy, int> second_variant = move(first_variant);
    EXPECT(second_variant.has<NoCopy>());
}

TEST_CASE(as)
{
    Variant<i8, i16, i32, i64> one_integer_to_rule_them_all { static_cast<i32>(42) };
    auto fake_integer = one_integer_to_rule_them_all.downcast<i8, i32>();
    EXPECT(fake_integer.has<i32>());
    EXPECT(one_integer_to_rule_them_all.has<i32>());
    EXPECT_EQ(fake_integer.get<i32>(), 42);
    EXPECT_EQ(one_integer_to_rule_them_all.get<i32>(), 42);

    fake_integer = static_cast<i8>(60);
    one_integer_to_rule_them_all = fake_integer.downcast<i8, i16>().downcast<i8, i32, float>().downcast<i8, i16, i32, i64>();
    EXPECT(fake_integer.has<i8>());
    EXPECT(one_integer_to_rule_them_all.has<i8>());
    EXPECT_EQ(fake_integer.get<i8>(), 60);
    EXPECT_EQ(one_integer_to_rule_them_all.get<i8>(), 60);

    using SomeFancyType = Variant<i8, i16>;
    one_integer_to_rule_them_all = fake_integer.downcast<SomeFancyType>();
    EXPECT(fake_integer.has<i8>());
    EXPECT(one_integer_to_rule_them_all.has<i8>());
    EXPECT_EQ(fake_integer.get<i8>(), 60);
    EXPECT_EQ(one_integer_to_rule_them_all.get<i8>(), 60);
}

TEST_CASE(moved_from_state)
{
    // Note: This test requires that Vector's moved-from state be consistent
    //       it need not be in a specific state (though as it is currently implemented,
    //       a moved-from vector is the same as a newly-created vector)
    //       This test does not make assumptions about the state itself, but rather that
    //       it remains consistent when done on different instances.
    //       Should this assumption be broken, we should probably switch to defining a local
    //       class that has fixed semantics, but I doubt the moved-from state of Vector will
    //       change any time soon :P
    Vector<i32> bunch_of_values { 1, 2, 3, 4, 5, 6, 7, 8 };
    Variant<Vector<i32>, Empty> optionally_a_bunch_of_values { Vector<i32> { 1, 2, 3, 4, 5, 6, 7, 8 } };

    {
        [[maybe_unused]] auto devnull_0 = move(bunch_of_values);
        [[maybe_unused]] auto devnull_1 = move(optionally_a_bunch_of_values);
    }

    // The moved-from state should be the same in both cases, and the variant should still contain a moved-from vector.
    // Note: Use after move is intentional.
    EXPECT(optionally_a_bunch_of_values.has<Vector<i32>>());
    auto same_contents = __builtin_memcmp(&bunch_of_values, &optionally_a_bunch_of_values.get<Vector<i32>>(), sizeof(bunch_of_values)) == 0;
    EXPECT(same_contents);
}

TEST_CASE(duplicated_types)
{
    Variant<int, int, int, int> its_just_an_int { 42 };
    EXPECT(its_just_an_int.has<int>());
    EXPECT_EQ(its_just_an_int.get<int>(), 42);
}

TEST_CASE(return_values)
{
    using MyVariant = Variant<int, ByteString, float>;
    {
        MyVariant the_value { 42.0f };

        float value = the_value.visit(
            [&](int const&) { return 1.0f; },
            [&](ByteString const&) { return 2.0f; },
            [&](float const& f) { return f; });
        EXPECT_EQ(value, 42.0f);
    }
    {
        MyVariant the_value { 42 };

        int value = the_value.visit(
            [&](int& i) { return i; },
            [&](ByteString&) { return 2; },
            [&](float&) { return 3; });
        EXPECT_EQ(value, 42);
    }
    {
        MyVariant const the_value { "str" };

        ByteString value = the_value.visit(
            [&](int const&) { return ByteString { "wrong" }; },
            [&](ByteString const& s) { return s; },
            [&](float const&) { return ByteString { "wrong" }; });
        EXPECT_EQ(value, "str");
    }
}

TEST_CASE(return_values_by_reference)
{
    auto ref = adopt_ref_if_nonnull(new (nothrow) Object());
    Variant<int, ByteString, float> the_value { 42.0f };

    auto& value = the_value.visit(
        [&](int const&) -> RefPtr<Object>& { return ref; },
        [&](ByteString const&) -> RefPtr<Object>& { return ref; },
        [&](float const&) -> RefPtr<Object>& { return ref; });

    EXPECT_EQ(ref, value);
    EXPECT_EQ(ref->ref_count(), 1u);
    EXPECT_EQ(value->ref_count(), 1u);
}

struct HoldsInt {
    int i;
};
struct HoldsFloat {
    float f;
};

TEST_CASE(copy_assign)
{
    {
        Variant<int, ByteString, float> the_value { 42.0f };

        VERIFY(the_value.has<float>());
        EXPECT_EQ(the_value.get<float>(), 42.0f);

        int twelve = 12;
        the_value = twelve;
        VERIFY(the_value.has<int>());
        EXPECT_EQ(the_value.get<int>(), 12);

        the_value = ByteString("Hello, world!");
        VERIFY(the_value.has<ByteString>());
        EXPECT_EQ(the_value.get<ByteString>(), "Hello, world!");
    }
    {
        Variant<HoldsInt, ByteString, HoldsFloat> the_value { HoldsFloat { 42.0f } };

        VERIFY(the_value.has<HoldsFloat>());
        EXPECT_EQ(the_value.get<HoldsFloat>().f, 42.0f);

        HoldsInt twelve { 12 };
        the_value = twelve;
        VERIFY(the_value.has<HoldsInt>());
        EXPECT_EQ(the_value.get<HoldsInt>().i, 12);

        the_value = ByteString("Hello, world!");
        VERIFY(the_value.has<ByteString>());
        EXPECT_EQ(the_value.get<ByteString>(), "Hello, world!");
    }
}

TEST_CASE(default_empty)
{
    Variant<Empty, int> my_variant;
    EXPECT(my_variant.has<Empty>());
    EXPECT(!my_variant.has<int>());
}

TEST_CASE(type_list_specialization)
{
    EXPECT_EQ((TypeList<Variant<Empty>>::size), 1u);
    EXPECT_EQ((TypeList<Variant<Empty, int>>::size), 2u);
    EXPECT_EQ((TypeList<Variant<Empty, int, String>>::size), 3u);

    using MyVariant = Variant<Empty, int, String>;
    using MyList = TypeList<MyVariant>;
    EXPECT((IsSame<typename MyList::template Type<0>, Empty>));
    EXPECT((IsSame<typename MyList::template Type<1>, int>));
    EXPECT((IsSame<typename MyList::template Type<2>, String>));
}

TEST_CASE(variant_equality)
{
    using MyVariant = Variant<Empty, int, float>;

    {
        MyVariant variant1 = 1;
        MyVariant variant2 = 1;
        EXPECT_EQ(variant1, variant2);
    }

    {
        MyVariant variant1 = 1;
        MyVariant variant2 = 1.5f;
        EXPECT_NE(variant1, variant2);
    }

    {
        MyVariant variant1 = 1;
        MyVariant variant2;
        EXPECT_NE(variant1, variant2);
    }

    {
        MyVariant variant1;
        MyVariant variant2;
        EXPECT_EQ(variant1, variant2);
    }
}

TEST_CASE(flatten_variant)
{
    using InnerVariant = Variant<Empty, int>;
    using OuterVariant = FlattenVariant<InnerVariant, Variant<float>>;
    using MyVariant = Variant<Empty, int, float>;

    EXPECT_EQ((TypeList<MyVariant>::size), 3u);
    EXPECT_EQ((TypeList<OuterVariant>::size), 3u);
    EXPECT((IsSame<OuterVariant, MyVariant>));

    using OuterList = TypeList<OuterVariant>;
    EXPECT((IsSame<typename OuterList::template Type<0>, Empty>));
    EXPECT((IsSame<typename OuterList::template Type<1>, int>));
    EXPECT((IsSame<typename OuterList::template Type<2>, float>));
}

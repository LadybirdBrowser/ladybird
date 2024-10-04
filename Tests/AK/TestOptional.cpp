/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Daniel Bertalan <dani@danielbertalan.dev>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/Vector.h>

TEST_CASE(basic_optional)
{
    Optional<int> x;
    EXPECT_EQ(x.has_value(), false);
    x = 3;
    EXPECT_EQ(x.has_value(), true);
    EXPECT_EQ(x.value(), 3);
}

TEST_CASE(move_optional)
{
    Optional<int> x;
    EXPECT_EQ(x.has_value(), false);
    x = 3;
    EXPECT_EQ(x.has_value(), true);
    EXPECT_EQ(x.value(), 3);

    Optional<int> y;
    y = move(x);
    EXPECT_EQ(y.has_value(), true);
    EXPECT_EQ(y.value(), 3);
    EXPECT_EQ(x.has_value(), false);
}

TEST_CASE(optional_rvalue_ref_qualified_getters)
{
    struct DontCopyMe {
        DontCopyMe() { }
        ~DontCopyMe() = default;
        DontCopyMe(DontCopyMe&&) = default;
        DontCopyMe& operator=(DontCopyMe&&) = default;
        DontCopyMe(DontCopyMe const&) = delete;
        DontCopyMe& operator=(DontCopyMe const&) = delete;

        int x { 13 };
    };

    auto make_an_optional = []() -> Optional<DontCopyMe> {
        return DontCopyMe {};
    };

    EXPECT_EQ(make_an_optional().value().x, 13);
    EXPECT_EQ(make_an_optional().value_or(DontCopyMe {}).x, 13);
}

TEST_CASE(optional_leak_1)
{
    struct Structure {
        Optional<ByteString> str;
    };

    // This used to leak, it does not anymore.
    Vector<Structure> vec;
    vec.append({ "foo" });
    EXPECT_EQ(vec[0].str.has_value(), true);
    EXPECT_EQ(vec[0].str.value(), "foo");
}

TEST_CASE(short_notation)
{
    Optional<StringView> value = "foo"sv;

    EXPECT_EQ(value->length(), 3u);
    EXPECT_EQ(*value, "foo");
}

TEST_CASE(comparison_without_values)
{
    Optional<StringView> opt0;
    Optional<StringView> opt1;
    Optional<ByteString> opt2;
    EXPECT_EQ(opt0, opt1);
    EXPECT_EQ(opt0, opt2);
}

TEST_CASE(comparison_with_values)
{
    Optional<StringView> opt0;
    Optional<StringView> opt1 = "foo"sv;
    Optional<ByteString> opt2 = "foo"sv;
    Optional<StringView> opt3 = "bar"sv;
    EXPECT_NE(opt0, opt1);
    EXPECT_EQ(opt1, opt2);
    EXPECT_NE(opt1, opt3);
}

TEST_CASE(comparison_to_underlying_types)
{
    Optional<ByteString> opt0;
    EXPECT_NE(opt0, ByteString());
    EXPECT_NE(opt0, "foo");

    Optional<StringView> opt1 = "foo"sv;
    EXPECT_EQ(opt1, "foo");
    EXPECT_NE(opt1, "bar");
    EXPECT_EQ(opt1, ByteString("foo"));
}

TEST_CASE(comparison_with_numeric_types)
{
    Optional<u8> opt0;
    EXPECT_NE(opt0, 0);
    Optional<u8> opt1 = 7;
    EXPECT_EQ(opt1, 7);
    EXPECT_EQ(opt1, 7.0);
    EXPECT_EQ(opt1, 7u);
    EXPECT_NE(opt1, -2);
}

TEST_CASE(test_copy_ctor_and_dtor_called)
{
#ifdef AK_HAVE_CONDITIONALLY_TRIVIAL
    static_assert(IsTriviallyDestructible<Optional<u8>>);
    static_assert(IsTriviallyCopyable<Optional<u8>>);
    static_assert(IsTriviallyCopyConstructible<Optional<u8>>);
    static_assert(IsTriviallyCopyAssignable<Optional<u8>>);
    // These can't be trivial as we have to clear the original object.
    static_assert(!IsTriviallyMoveConstructible<Optional<u8>>);
    static_assert(!IsTriviallyMoveAssignable<Optional<u8>>);
#endif

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

    static_assert(!IsTriviallyDestructible<Optional<DestructionChecker>>);

    bool was_destroyed = false;
    {
        Optional<DestructionChecker> test_optional = DestructionChecker { was_destroyed };
    }
    EXPECT(was_destroyed);

    struct CopyChecker {
        explicit CopyChecker(bool& was_copy_constructed)
            : m_was_copy_constructed(was_copy_constructed)
        {
        }

        CopyChecker(CopyChecker const& other)
            : m_was_copy_constructed(other.m_was_copy_constructed)
        {
            m_was_copy_constructed = true;
        }

        bool& m_was_copy_constructed;
    };

    static_assert(IsCopyConstructible<Optional<CopyChecker>>);
    static_assert(!IsTriviallyCopyConstructible<Optional<CopyChecker>>);

    bool was_copy_constructed = false;
    Optional<CopyChecker> copy1 = CopyChecker { was_copy_constructed };
    Optional<CopyChecker> copy2 = copy1;
    EXPECT(was_copy_constructed);

    struct MoveChecker {
        explicit MoveChecker(bool& was_move_constructed)
            : m_was_move_constructed(was_move_constructed)
        {
        }

        MoveChecker(MoveChecker const& other)
            : m_was_move_constructed(other.m_was_move_constructed)
        {
            EXPECT(false);
        }

        MoveChecker(MoveChecker&& other)
            : m_was_move_constructed(other.m_was_move_constructed)
        {
            m_was_move_constructed = true;
        }

        bool& m_was_move_constructed;
    };
    static_assert(IsMoveConstructible<Optional<MoveChecker>>);
    static_assert(!IsTriviallyMoveConstructible<Optional<MoveChecker>>);

    bool was_moved = false;
    Optional<MoveChecker> move1 = MoveChecker { was_moved };
    Optional<MoveChecker> move2 = move(move1);
    EXPECT(was_moved);

#ifdef AK_HAVE_CONDITIONALLY_TRIVIAL
    struct NonDestructible {
        ~NonDestructible() = delete;
    };
    static_assert(!IsDestructible<Optional<NonDestructible>>);
#endif
}

TEST_CASE(basic_optional_reference)
{
    Optional<int&> x;
    EXPECT_EQ(x.has_value(), false);
    int a = 3;
    x = a;
    EXPECT_EQ(x.has_value(), true);
    EXPECT_EQ(x.value(), 3);
    EXPECT_EQ(&x.value(), &a);

    Optional<int const&> y;
    EXPECT_EQ(y.has_value(), false);
    int b = 3;
    y = b;
    EXPECT_EQ(y.has_value(), true);
    EXPECT_EQ(y.value(), 3);
    EXPECT_EQ(&y.value(), &b);
    static_assert(IsConst<RemoveReference<decltype(y.value())>>);
}

TEST_CASE(move_optional_reference)
{
    Optional<int&> x;
    EXPECT_EQ(x.has_value(), false);
    int b = 3;
    x = b;
    EXPECT_EQ(x.has_value(), true);
    EXPECT_EQ(x.value(), 3);

    Optional<int&> y;
    y = move(x);
    EXPECT_EQ(y.has_value(), true);
    EXPECT_EQ(y.value(), 3);
    EXPECT_EQ(x.has_value(), false);
}

TEST_CASE(short_notation_reference)
{
    StringView test = "foo"sv;
    Optional<StringView&> value = test;

    EXPECT_EQ(value->length(), 3u);
    EXPECT_EQ(*value, "foo");
}

TEST_CASE(comparison_reference)
{
    StringView test = "foo"sv;
    Optional<StringView&> opt0;
    Optional<StringView const&> opt1 = test;
    Optional<ByteString> opt2 = "foo"sv;
    Optional<StringView> opt3 = "bar"sv;

    EXPECT_NE(opt0, opt1);
    EXPECT_EQ(opt1, opt2);
    EXPECT_NE(opt1, opt3);
}

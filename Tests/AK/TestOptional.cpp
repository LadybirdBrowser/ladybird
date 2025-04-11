/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Daniel Bertalan <dani@danielbertalan.dev>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/ByteString.h>
#include <AK/FlyString.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>

class NonCopyable {
    AK_MAKE_NONCOPYABLE(NonCopyable);
    AK_MAKE_DEFAULT_MOVABLE(NonCopyable);

public:
    constexpr NonCopyable() { }
    ~NonCopyable() = default;

    int x { 13 };
};

class NonTriviallyCopyable {
    AK_MAKE_DEFAULT_COPYABLE(NonTriviallyCopyable);
    AK_MAKE_DEFAULT_MOVABLE(NonTriviallyCopyable);

public:
    constexpr NonTriviallyCopyable() = default;
    ~NonTriviallyCopyable() = default;

    ByteString x { "13" };
};

class TriviallyCopyable {
    AK_MAKE_DEFAULT_COPYABLE(TriviallyCopyable);
    AK_MAKE_DEFAULT_MOVABLE(TriviallyCopyable);

public:
    constexpr TriviallyCopyable() = default;
    ~TriviallyCopyable() = default;

    int x { 13 };
};

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
}

TEST_CASE(optional_rvalue_ref_qualified_getters)
{
    auto make_an_optional = []() -> Optional<NonCopyable> {
        return NonCopyable {};
    };

    EXPECT_EQ(make_an_optional().value().x, 13);
    EXPECT_EQ(make_an_optional().value_or(NonCopyable {}).x, 13);
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

TEST_CASE(test_constexpr)
{
    int i = 13;
    NonCopyable dcm {};

    EXPECT_CONSTEVAL(Optional<int> {});
    EXPECT_CONSTEVAL(Optional<NonCopyable> {});
    EXPECT_CONSTEVAL(Optional<int const> {});
    EXPECT_CONSTEVAL(Optional<NonCopyable const> {});
    EXPECT_CONSTEVAL(Optional<int&> {});
    EXPECT_CONSTEVAL(Optional<NonCopyable&> {});
    EXPECT_CONSTEVAL(Optional<int const&> {});
    EXPECT_CONSTEVAL(Optional<NonCopyable const&> {});

    EXPECT_CONSTEVAL(Optional<int> { 13 });
    EXPECT_CONSTEVAL(Optional<NonCopyable> { NonCopyable {} });
    EXPECT_CONSTEVAL(Optional<int const> { 13 });
    EXPECT_CONSTEVAL(Optional<NonCopyable const> { NonCopyable {} });
    EXPECT_CONSTEVAL(Optional<int&> { i });
    EXPECT_CONSTEVAL(Optional<NonCopyable&> { dcm });
    EXPECT_CONSTEVAL(Optional<int const&> { 13 });
    EXPECT_CONSTEVAL(Optional<NonCopyable const&> { NonCopyable {} });

    static_assert(!Optional<int> {}.has_value());
    static_assert(!Optional<NonCopyable> {}.has_value());
    static_assert(!Optional<int const> {}.has_value());
    static_assert(!Optional<NonCopyable const> {}.has_value());
    static_assert(!Optional<int&> {}.has_value());
    static_assert(!Optional<NonCopyable&> {}.has_value());
    static_assert(!Optional<int const&> {}.has_value());
    static_assert(!Optional<NonCopyable const&> {}.has_value());

    static_assert(Optional<int> { 13 }.has_value());
    static_assert(Optional<NonCopyable> { NonCopyable {} }.has_value());
    static_assert(Optional<int const> { 13 }.has_value());
    static_assert(Optional<NonCopyable const> { NonCopyable {} }.has_value());
    static_assert(Optional<int&> { i }.has_value());
    static_assert(Optional<NonCopyable&> { dcm }.has_value());
    static_assert(Optional<int const&> { 13 }.has_value());
    static_assert(Optional<NonCopyable const&> { NonCopyable {} }.has_value());

    static_assert(Optional<int> { 13 }.value() == 13);
    static_assert(Optional<NonCopyable> { NonCopyable {} }.value().x == 13);
    static_assert(Optional<int const> { 13 }.value() == 13);
    static_assert(Optional<int const&> { 13 }.value() == 13);
    static_assert(Optional<NonCopyable const&> { NonCopyable {} }.value().x == 13);

    static_assert(!(Optional<int> { 1 } = {}).has_value(), "Assigning a `{}` should clear the Optional, even for scalar types^^");
}

TEST_CASE(non_trivial_destructor_is_called_on_move_assignment)
{
    static int foo_destruction_count = 0;

    struct Foo {
        Foo() { }
        Foo(Foo&&) = default;
        ~Foo()
        {
            ++foo_destruction_count;
        }

        Foo& operator=(Foo&&) = default;
    };
    static_assert(!IsTriviallyMoveAssignable<Optional<Foo>>);

    Optional<Foo> foo = Foo {}; // 1. The immediate value needs to be destroyed
    Optional<Foo> foo2;
    foo = AK::move(foo2); // 2. The move releases the value, which destroys the moved-from stored value

    EXPECT_EQ(foo_destruction_count, 2);

    // As Optional<Foo> does not trivially move, moved-from values are empty
    // Ignoring the fact that we are touching a moved from value here
    EXPECT_EQ(foo.has_value(), false);
}

TEST_CASE(test_copy_ctor_and_dtor_called)
{
    static_assert(IsTriviallyDestructible<Optional<u8>>);
    static_assert(IsTriviallyCopyable<Optional<u8>>);
    static_assert(IsTriviallyCopyConstructible<Optional<u8>>);
    static_assert(IsTriviallyCopyAssignable<Optional<u8>>);
    static_assert(IsTriviallyMoveConstructible<Optional<u8>>);
    static_assert(IsTriviallyMoveAssignable<Optional<u8>>);
    static_assert(IsTriviallyCopyConstructible<Optional<int&>>);
    static_assert(IsTriviallyCopyAssignable<Optional<int&>>);
    static_assert(IsTriviallyDestructible<Optional<int&>>);

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

    struct NonDestructible {
        ~NonDestructible() = delete;
    };
    static_assert(!IsDestructible<Optional<NonDestructible>>);
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

TEST_CASE(uninitialized_constructor)
{
    static bool was_constructed = false;
    struct Internal {
        Internal() { was_constructed = true; }
    };

    struct ShouldNotBeDefaultConstructed {
        bool m_default_constructed { true };
        Internal m_internal;
        ShouldNotBeDefaultConstructed() = default;
        ShouldNotBeDefaultConstructed(bool)
            : m_default_constructed(false)
        {
        }
    };
    static_assert(IsConstructible<ShouldNotBeDefaultConstructed>);

    Optional<ShouldNotBeDefaultConstructed> opt;
    EXPECT(!was_constructed);
    EXPECT(!opt.has_value());

    opt = ShouldNotBeDefaultConstructed { true };
    EXPECT(was_constructed);
    EXPECT(opt.has_value());
    EXPECT(!opt.value().m_default_constructed);
}

consteval bool test_constexpr()
{
    Optional<int> none;
    if (none.has_value())
        return false;

    Optional<int> x;
    x = 3;
    if (!x.has_value())
        return false;

    if (x.value() != 3)
        return false;

    Optional<int> y;
    y = x.release_value();
    if (!y.has_value())
        return false;

    if (y.value() != 3)
        return false;

    if (x.has_value())
        return false;

    return true;
}
static_assert(test_constexpr());

template<typename To, typename From>
struct CheckAssignments;

template<typename To, typename From>
requires(requires { declval<To>() = declval<From>(); })
struct CheckAssignments<To, From> {
    static constexpr bool allowed = true;
};

template<typename To, typename From>
requires(!requires { declval<To>() = declval<From>(); })
struct CheckAssignments<To, From> {
    static constexpr bool allowed = false;
};

static_assert(CheckAssignments<Optional<int>, int>::allowed);
static_assert(!CheckAssignments<Optional<int*>, double*>::allowed);

static_assert(CheckAssignments<Optional<int&>, int&>::allowed);
static_assert(!CheckAssignments<Optional<int&>, int const&>::allowed);
static_assert(!CheckAssignments<Optional<int&>, int&&>::allowed);
static_assert(!CheckAssignments<Optional<int&>, int const&&>::allowed);

static_assert(CheckAssignments<Optional<int const&>, int&>::allowed);
static_assert(CheckAssignments<Optional<int const&>, int const&>::allowed);
static_assert(CheckAssignments<Optional<int const&>, int&&>::allowed);       // Lifetime extension
static_assert(CheckAssignments<Optional<int const&>, int const&&>::allowed); // Lifetime extension

static_assert(CheckAssignments<Optional<NonTriviallyCopyable const&>, NonTriviallyCopyable&>::allowed);
static_assert(CheckAssignments<Optional<NonTriviallyCopyable const&>, NonTriviallyCopyable const&>::allowed);
static_assert(CheckAssignments<Optional<NonTriviallyCopyable const&>, NonTriviallyCopyable&&>::allowed);       // Lifetime extension
static_assert(CheckAssignments<Optional<NonTriviallyCopyable const&>, NonTriviallyCopyable const&&>::allowed); // Lifetime extension

static_assert(CheckAssignments<Optional<TriviallyCopyable const&>, TriviallyCopyable>::allowed);
static_assert(CheckAssignments<Optional<TriviallyCopyable const&>, TriviallyCopyable const&>::allowed);
static_assert(CheckAssignments<Optional<TriviallyCopyable const&>, Optional<TriviallyCopyable>>::allowed);
static_assert(CheckAssignments<Optional<TriviallyCopyable const&>, Optional<TriviallyCopyable const&>>::allowed);
static_assert(CheckAssignments<Optional<TriviallyCopyable>, Optional<TriviallyCopyable const&>>::allowed);

static_assert(CheckAssignments<Optional<NonTriviallyCopyable const&>, NonTriviallyCopyable>::allowed);
static_assert(CheckAssignments<Optional<NonTriviallyCopyable const&>, NonTriviallyCopyable const&>::allowed);
static_assert(CheckAssignments<Optional<NonTriviallyCopyable const&>, Optional<NonTriviallyCopyable>>::allowed);
static_assert(CheckAssignments<Optional<NonTriviallyCopyable const&>, Optional<NonTriviallyCopyable const&>>::allowed);
static_assert(!CheckAssignments<Optional<NonTriviallyCopyable>, Optional<NonTriviallyCopyable const&>>::allowed);

TEST_CASE(nontrivially_copyable_assignment)
{
    {
        TriviallyCopyable x {};
        Optional<TriviallyCopyable const&> y = x;
        Optional<TriviallyCopyable> z = y; // Can copy implicitly
        EXPECT_EQ(z->x, 13);
    }

    {
        NonTriviallyCopyable x {};
        Optional<NonTriviallyCopyable const&> y = x;
        Optional<NonTriviallyCopyable> z = y.copy(); // Have to copy explicitly
        EXPECT_EQ(z->x, "13");
    }

    {
        NonTriviallyCopyable x {};
        Optional<NonTriviallyCopyable const&> y = x;
        Optional<NonTriviallyCopyable> z = Optional<NonTriviallyCopyable>(y); // Explicit copy constructor is still defined
        EXPECT_EQ(z->x, "13");
    }
}

TEST_CASE(string_specialization)
{
    EXPECT_EQ(sizeof(Optional<String>), sizeof(String));

    {
        Optional<String> foo;

        EXPECT(!foo.has_value());

        foo = "long_enough_to_be_allocated"_string;

        EXPECT(foo.has_value());
        EXPECT_EQ(foo.value(), "long_enough_to_be_allocated"sv);
    }

    {
        Optional<String> foo = "initial_value"_string;

        EXPECT(foo.has_value());
        EXPECT_EQ(foo.value(), "initial_value"sv);

        foo = "long_enough_to_be_allocated"_string;

        EXPECT(foo.has_value());
        EXPECT_EQ(foo.value(), "long_enough_to_be_allocated"sv);
    }

    {
        Optional<String> foo;

        EXPECT(!foo.has_value());

        String bar = "long_enough_to_be_allocated"_string;
        foo = bar;

        EXPECT(foo.has_value());
        EXPECT_EQ(foo.value(), "long_enough_to_be_allocated"sv);
    }

    {
        Optional<String> foo;

        EXPECT(!foo.has_value());

        Optional<String> bar = "long_enough_to_be_allocated"_string;
        foo = bar;

        EXPECT(foo.has_value());
        EXPECT_EQ(foo.value(), "long_enough_to_be_allocated"sv);
        EXPECT(bar.has_value());
        EXPECT_EQ(bar.value(), "long_enough_to_be_allocated"sv);
    }

    {
        Optional<String> foo;

        EXPECT(!foo.has_value());

        foo = Optional<String> { "long_enough_to_be_allocated"_string };

        EXPECT(foo.has_value());
        EXPECT_EQ(foo.value(), "long_enough_to_be_allocated"sv);
    }

    {
        Optional<String> foo = "long_enough_to_be_allocated"_string;

        EXPECT_EQ(foo.value_or("fallback_value"_string), "long_enough_to_be_allocated"sv);
    }

    {
        Optional<String> foo;

        EXPECT_EQ(foo.value_or("fallback_value"_string), "fallback_value"sv);
    }

    {
        EXPECT_EQ((Optional<String> { "long_enough_to_be_allocated"_string }).value_or("fallback_value"_string), "long_enough_to_be_allocated"sv);
    }

    {
        EXPECT_EQ((Optional<String> {}).value_or("fallback_value"_string), "fallback_value"sv);
    }
}

TEST_CASE(flystring_specialization)
{
    EXPECT_EQ(sizeof(Optional<FlyString>), sizeof(FlyString));

    {
        Optional<FlyString> foo;

        EXPECT(!foo.has_value());

        foo = "long_enough_to_be_allocated"_fly_string;

        EXPECT(foo.has_value());
        EXPECT_EQ(foo.value(), "long_enough_to_be_allocated"sv);
    }

    {
        Optional<FlyString> foo = "initial_value"_fly_string;

        EXPECT(foo.has_value());
        EXPECT_EQ(foo.value(), "initial_value"sv);

        foo = "long_enough_to_be_allocated"_fly_string;

        EXPECT(foo.has_value());
        EXPECT_EQ(foo.value(), "long_enough_to_be_allocated"sv);
    }

    {
        Optional<FlyString> foo;

        EXPECT(!foo.has_value());

        FlyString bar = "long_enough_to_be_allocated"_fly_string;
        foo = bar;

        EXPECT(foo.has_value());
        EXPECT_EQ(foo.value(), "long_enough_to_be_allocated"sv);
    }

    {
        Optional<FlyString> foo;

        EXPECT(!foo.has_value());

        Optional<FlyString> bar = "long_enough_to_be_allocated"_fly_string;
        foo = bar;

        EXPECT(bar.has_value());
        EXPECT_EQ(bar.value(), "long_enough_to_be_allocated"sv);
        EXPECT(foo.has_value());
        EXPECT_EQ(foo.value(), "long_enough_to_be_allocated"sv);
    }

    {
        Optional<FlyString> foo;

        EXPECT(!foo.has_value());

        foo = Optional<FlyString> { "long_enough_to_be_allocated"_fly_string };

        EXPECT(foo.has_value());
        EXPECT_EQ(foo.value(), "long_enough_to_be_allocated"sv);
    }

    {
        Optional<FlyString> foo = "long_enough_to_be_allocated"_fly_string;

        EXPECT_EQ(foo.value_or("fallback_value"_fly_string), "long_enough_to_be_allocated"sv);
    }

    {
        Optional<FlyString> foo;

        EXPECT_EQ(foo.value_or("fallback_value"_fly_string), "fallback_value"sv);
    }

    {
        EXPECT_EQ((Optional<FlyString> { "long_enough_to_be_allocated"_fly_string }).value_or("fallback_value"_fly_string), "long_enough_to_be_allocated"sv);
    }

    {
        EXPECT_EQ((Optional<FlyString> {}).value_or("fallback_value"_fly_string), "fallback_value"sv);
    }
}

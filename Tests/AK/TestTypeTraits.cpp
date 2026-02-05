/*
 * Copyright (c) 2020-2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/TypeList.h>

#define STATIC_EXPECT_EQ(lhs, rhs) \
    static_assert(IsSame<lhs, rhs>, "");

#define STATIC_EXPECT_FALSE(Expression) \
    static_assert(!Expression, "");

#define STATIC_EXPECT_TRUE(Expression) \
    static_assert(Expression, "");

#define EXPECT_TRAIT_TRUE(trait, ...)                                     \
    for_each_type<TypeList<__VA_ARGS__>>([]<typename T>(TypeWrapper<T>) { \
        STATIC_EXPECT_TRUE(trait<T>);                                     \
    })

#define EXPECT_TRAIT_FALSE(trait, ...)                                    \
    for_each_type<TypeList<__VA_ARGS__>>([]<typename T>(TypeWrapper<T>) { \
        STATIC_EXPECT_FALSE(trait<T>);                                    \
    })

#define EXPECT_EQ_WITH_TRAIT(trait, ListA, ListB)                                                   \
    for_each_type_zipped<ListA, ListB>([]<typename A, typename B>(TypeWrapper<A>, TypeWrapper<B>) { \
        STATIC_EXPECT_EQ(trait<A>, B);                                                              \
    })

#define EXPECT_VARIADIC_TRAIT_TRUE(trait, ...) \
    static_assert(trait<__VA_ARGS__>)

#define EXPECT_VARIADIC_TRAIT_FALSE(trait, ...) \
    static_assert(!trait<__VA_ARGS__>)

enum class Enummer : u8 {
    Dummy
};

TEST_CASE(FundamentalTypeClassification)
{
    EXPECT_TRAIT_TRUE(IsVoid, void);
    EXPECT_TRAIT_FALSE(IsVoid, int, Empty, nullptr_t);

    EXPECT_TRAIT_TRUE(IsNullPointer, nullptr_t);
    EXPECT_TRAIT_FALSE(IsNullPointer, void, int, Empty, decltype(0));

    EXPECT_TRAIT_TRUE(IsFloatingPoint, float, double, long double);
    EXPECT_TRAIT_FALSE(IsFloatingPoint, int, Empty, nullptr_t, void);

    EXPECT_TRAIT_TRUE(IsArithmetic, float, double, long double, bool, size_t);
    EXPECT_TRAIT_TRUE(IsArithmetic, char, signed char, unsigned char, char8_t, char16_t, char32_t);
    EXPECT_TRAIT_TRUE(IsArithmetic, short, int, long, long long);
    EXPECT_TRAIT_TRUE(IsArithmetic, unsigned short, unsigned int, unsigned long, unsigned long long);

    EXPECT_TRAIT_FALSE(IsArithmetic, void, nullptr_t, Empty);

    EXPECT_TRAIT_TRUE(IsFundamental, void, nullptr_t);
    EXPECT_TRAIT_TRUE(IsFundamental, float, double, long double, bool, size_t);
    EXPECT_TRAIT_TRUE(IsFundamental, char, signed char, unsigned char, char8_t, char16_t, char32_t);
    EXPECT_TRAIT_TRUE(IsFundamental, short, int, long, long long);
    EXPECT_TRAIT_TRUE(IsFundamental, unsigned short, unsigned int, unsigned long, unsigned long long);

    EXPECT_TRAIT_FALSE(IsFundamental, Empty, int*, int&);

    EXPECT_TRAIT_FALSE(IsSigned, unsigned);
    EXPECT_TRAIT_FALSE(IsSigned, unsigned short);
    EXPECT_TRAIT_FALSE(IsSigned, unsigned char);
    EXPECT_TRAIT_FALSE(IsSigned, unsigned long);
    EXPECT_TRAIT_TRUE(IsSigned, int);
    EXPECT_TRAIT_TRUE(IsSigned, short);
    EXPECT_TRAIT_TRUE(IsSigned, long);

    EXPECT_TRAIT_TRUE(IsUnsigned, unsigned);
    EXPECT_TRAIT_TRUE(IsUnsigned, unsigned short);
    EXPECT_TRAIT_TRUE(IsUnsigned, unsigned char);
    EXPECT_TRAIT_TRUE(IsUnsigned, unsigned long);
    EXPECT_TRAIT_FALSE(IsUnsigned, int);
    EXPECT_TRAIT_FALSE(IsUnsigned, short);
    EXPECT_TRAIT_FALSE(IsUnsigned, long);

    EXPECT_TRAIT_TRUE(IsEnum, Enummer);
    EXPECT_TRAIT_FALSE(IsEnum, Empty);
    EXPECT_TRAIT_FALSE(IsEnum, int);
    EXPECT_TRAIT_FALSE(IsEnum, void);
    EXPECT_TRAIT_FALSE(IsEnum, nullptr_t);
}

TEST_CASE(AddConst)
{
    // clang-format off
    using NoConstList =  TypeList<int,       const int, Empty,       const Empty>;
    using YesConstList = TypeList<const int, const int, const Empty, const Empty>;
    // clang-format on

    EXPECT_EQ_WITH_TRAIT(AddConst, NoConstList, YesConstList);
}

TEST_CASE(UnderlyingType)
{
    using Type = UnderlyingType<Enummer>;

    STATIC_EXPECT_EQ(Type, u8);
}

TEST_CASE(RemoveCVReference)
{
    using TestTypeList = TypeList<int, int&, int const&, int volatile&, int const volatile&, int&&, int const&&, int volatile&&, int const volatile&&>;
    using ResultTypeList = TypeList<int, int, int, int, int, int, int, int, int>;

    EXPECT_EQ_WITH_TRAIT(RemoveCVReference, TestTypeList, ResultTypeList);
}

TEST_CASE(AddReference)
{
    STATIC_EXPECT_EQ(AddLvalueReference<int>, int&);
    STATIC_EXPECT_EQ(AddLvalueReference<int&>, int&);
    STATIC_EXPECT_EQ(AddLvalueReference<int&&>, int&);

    STATIC_EXPECT_EQ(AddRvalueReference<int>, int&&);
    STATIC_EXPECT_EQ(AddRvalueReference<int&>, int&);
    STATIC_EXPECT_EQ(AddRvalueReference<int&&>, int&&);

    STATIC_EXPECT_EQ(AddLvalueReference<void>, void);
}

TEST_CASE(IsConvertible)
{
    struct A {
    };
    struct B {
        B(A);
    };
    struct C {
        A a;
        operator A() { return a; }
    };
    struct D {
    };

    EXPECT_VARIADIC_TRAIT_TRUE(IsConvertible, A, B);
    EXPECT_VARIADIC_TRAIT_FALSE(IsConvertible, B, A);
    EXPECT_VARIADIC_TRAIT_TRUE(IsConvertible, C, A);
    EXPECT_VARIADIC_TRAIT_FALSE(IsConvertible, A, C);
    EXPECT_VARIADIC_TRAIT_FALSE(IsConvertible, D, A);
    EXPECT_VARIADIC_TRAIT_FALSE(IsConvertible, A, D);
}

TEST_CASE(IsAssignable)
{
    EXPECT_VARIADIC_TRAIT_FALSE(IsAssignable, int, int);
    EXPECT_VARIADIC_TRAIT_TRUE(IsAssignable, int&, int);
    EXPECT_VARIADIC_TRAIT_FALSE(IsAssignable, int, void);

    struct A {
    };
    EXPECT_TRAIT_TRUE(IsCopyAssignable, A);
    EXPECT_TRAIT_TRUE(IsTriviallyCopyAssignable, A);
    EXPECT_TRAIT_TRUE(IsMoveAssignable, A);
    EXPECT_TRAIT_TRUE(IsTriviallyMoveAssignable, A);

    struct B {
        B& operator=(B const&) { return *this; }
        B& operator=(B&&) { return *this; }
    };
    EXPECT_TRAIT_TRUE(IsCopyAssignable, B);
    EXPECT_TRAIT_FALSE(IsTriviallyCopyAssignable, B);
    EXPECT_TRAIT_TRUE(IsMoveAssignable, B);
    EXPECT_TRAIT_FALSE(IsTriviallyMoveAssignable, B);

    struct C {
        C& operator=(C const&) = delete;
        C& operator=(C&&) = delete;
    };
    EXPECT_TRAIT_FALSE(IsCopyAssignable, C);
    EXPECT_TRAIT_FALSE(IsTriviallyCopyAssignable, C);
    EXPECT_TRAIT_FALSE(IsMoveAssignable, C);
    EXPECT_TRAIT_FALSE(IsTriviallyMoveAssignable, C);
}

TEST_CASE(IsConstructible)
{
    struct A {
    };
    EXPECT_TRAIT_TRUE(IsCopyConstructible, A);
    EXPECT_TRAIT_TRUE(IsTriviallyCopyConstructible, A);
    EXPECT_TRAIT_TRUE(IsMoveConstructible, A);
    EXPECT_TRAIT_TRUE(IsTriviallyMoveConstructible, A);

    struct B {
        B(B const&)
        {
        }
        B(B&&)
        {
        }
    };
    EXPECT_TRAIT_TRUE(IsCopyConstructible, B);
    EXPECT_TRAIT_FALSE(IsTriviallyCopyConstructible, B);
    EXPECT_TRAIT_TRUE(IsMoveConstructible, B);
    EXPECT_TRAIT_FALSE(IsTriviallyMoveConstructible, B);

    struct C {
        C(C const&) = delete;
        C(C&&) = delete;
    };
    EXPECT_TRAIT_FALSE(IsCopyConstructible, C);
    EXPECT_TRAIT_FALSE(IsTriviallyCopyConstructible, C);
    EXPECT_TRAIT_FALSE(IsMoveConstructible, C);
    EXPECT_TRAIT_FALSE(IsTriviallyMoveConstructible, C);

    struct D {
        D(int);
    };
    EXPECT_VARIADIC_TRAIT_TRUE(IsConstructible, D, int);
    EXPECT_VARIADIC_TRAIT_TRUE(IsConstructible, D, char);
    EXPECT_VARIADIC_TRAIT_FALSE(IsConstructible, D, char const*);
    EXPECT_VARIADIC_TRAIT_FALSE(IsConstructible, D, void);
}

TEST_CASE(IsDestructible)
{
    struct A {
    };
    EXPECT_TRAIT_TRUE(IsDestructible, A);
    EXPECT_TRAIT_TRUE(IsTriviallyDestructible, A);
    struct B {
        ~B()
        {
        }
    };
    EXPECT_TRAIT_TRUE(IsDestructible, B);
    EXPECT_TRAIT_FALSE(IsTriviallyDestructible, B);
    struct C {
        ~C() = delete;
    };
    EXPECT_TRAIT_FALSE(IsDestructible, C);
    EXPECT_TRAIT_FALSE(IsTriviallyDestructible, C);
}

TEST_CASE(CommonType)
{
    using TCommon0 = CommonType<int, float, char>;
    EXPECT_VARIADIC_TRAIT_TRUE(IsSame, TCommon0, float);

    using TCommon1 = CommonType<int, int, int, char>;
    EXPECT_VARIADIC_TRAIT_TRUE(IsSame, TCommon1, int);

    struct Foo {
    };
    using TCommon2 = CommonType<Foo, Foo, Foo>;
    EXPECT_VARIADIC_TRAIT_TRUE(IsSame, TCommon2, Foo);

    struct Bar {
        operator Foo();
    };
    using TCommon3 = CommonType<Bar, Foo, Bar>;
    EXPECT_VARIADIC_TRAIT_TRUE(IsSame, TCommon3, Foo);
}

/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Badge.h>

namespace {

struct MultipleBadgeUserA {
    static void call_two_argument_badge();
    static void call_three_argument_badge();
    static void call_four_argument_badge();
    static void call_five_argument_badge();
};

struct MultipleBadgeUserB {
    static void call_two_argument_badge();
};

struct MultipleBadgeUserC {
    static void call_three_argument_badge();
};

struct MultipleBadgeUserD {
    static void call_four_argument_badge();
};

struct MultipleBadgeUserE {
    static void call_five_argument_badge();
};

struct UnrelatedBadgeUser {
};

struct BaseBadgeUser {
    static void call_base_badge();
    static void call_multi_base_badge();
};

struct DerivedBadgeUser : BaseBadgeUser {
    static void call_base_badge();
    static void call_multi_base_badge();
};

static void accepts_two_argument_badge(Badge<MultipleBadgeUserA, MultipleBadgeUserB>)
{
}

static void accepts_three_argument_badge(Badge<MultipleBadgeUserA, MultipleBadgeUserB, MultipleBadgeUserC>)
{
}

static void accepts_four_argument_badge(Badge<MultipleBadgeUserA, MultipleBadgeUserB, MultipleBadgeUserC, MultipleBadgeUserD>)
{
}

static void accepts_five_argument_badge(Badge<MultipleBadgeUserA, MultipleBadgeUserB, MultipleBadgeUserC, MultipleBadgeUserD, MultipleBadgeUserE>)
{
}

static void accepts_base_badge(Badge<BaseBadgeUser>)
{
}

static void accepts_multi_base_badge(Badge<BaseBadgeUser, MultipleBadgeUserB>)
{
}

void MultipleBadgeUserA::call_two_argument_badge()
{
    accepts_two_argument_badge({});
}

void MultipleBadgeUserA::call_three_argument_badge()
{
    accepts_three_argument_badge({});
}

void MultipleBadgeUserA::call_four_argument_badge()
{
    accepts_four_argument_badge({});
}

void MultipleBadgeUserA::call_five_argument_badge()
{
    accepts_five_argument_badge({});
}

void MultipleBadgeUserB::call_two_argument_badge()
{
    accepts_two_argument_badge({});
}

void MultipleBadgeUserC::call_three_argument_badge()
{
    accepts_three_argument_badge({});
}

void MultipleBadgeUserD::call_four_argument_badge()
{
    accepts_four_argument_badge({});
}

void MultipleBadgeUserE::call_five_argument_badge()
{
    accepts_five_argument_badge({});
}

void BaseBadgeUser::call_base_badge()
{
    accepts_base_badge({});
}

void BaseBadgeUser::call_multi_base_badge()
{
    accepts_multi_base_badge({});
}

void DerivedBadgeUser::call_base_badge()
{
    accepts_base_badge(Badge<DerivedBadgeUser> {});
}

void DerivedBadgeUser::call_multi_base_badge()
{
    accepts_multi_base_badge(Badge<DerivedBadgeUser> {});
}

}

TEST_CASE(should_provide_underlying_type)
{
    static_assert(IsSame<int, Badge<int>::Type>);
}

TEST_CASE(should_allow_multiple_underlying_types)
{
    static_assert(!IsConstructible<Badge<MultipleBadgeUserA, MultipleBadgeUserB>>);
    static_assert(!IsConstructible<Badge<MultipleBadgeUserA, MultipleBadgeUserB, MultipleBadgeUserC>>);
    static_assert(!IsConstructible<Badge<MultipleBadgeUserA, MultipleBadgeUserB, MultipleBadgeUserC, MultipleBadgeUserD>>);
    static_assert(!IsConstructible<Badge<MultipleBadgeUserA, MultipleBadgeUserB, MultipleBadgeUserC, MultipleBadgeUserD, MultipleBadgeUserE>>);
    static_assert(IsConstructible<
        Badge<MultipleBadgeUserA, MultipleBadgeUserB, MultipleBadgeUserC, MultipleBadgeUserD, MultipleBadgeUserE>,
        Badge<MultipleBadgeUserA> const&>);
    static_assert(IsConstructible<
        Badge<MultipleBadgeUserA, MultipleBadgeUserB>,
        Badge<MultipleBadgeUserA> const&>);
    static_assert(!IsConstructible<Badge<MultipleBadgeUserA, MultipleBadgeUserB>, Badge<UnrelatedBadgeUser>>);

    MultipleBadgeUserA::call_two_argument_badge();
    MultipleBadgeUserA::call_three_argument_badge();
    MultipleBadgeUserA::call_four_argument_badge();
    MultipleBadgeUserA::call_five_argument_badge();
    MultipleBadgeUserB::call_two_argument_badge();
    MultipleBadgeUserC::call_three_argument_badge();
    MultipleBadgeUserD::call_four_argument_badge();
    MultipleBadgeUserE::call_five_argument_badge();
}

TEST_CASE(should_allow_derived_types_to_create_base_badges)
{
    static_assert(IsConstructible<
        Badge<BaseBadgeUser>,
        Badge<DerivedBadgeUser> const&>);
    static_assert(IsConstructible<
        Badge<BaseBadgeUser, MultipleBadgeUserB>,
        Badge<BaseBadgeUser> const&>);
    static_assert(IsConstructible<
        Badge<BaseBadgeUser, MultipleBadgeUserB>,
        Badge<DerivedBadgeUser> const&>);
    static_assert(IsConstructible<
        Badge<BaseBadgeUser, MultipleBadgeUserB, MultipleBadgeUserC, MultipleBadgeUserD, MultipleBadgeUserE>,
        Badge<DerivedBadgeUser> const&>);
    static_assert(!IsConstructible<
        Badge<DerivedBadgeUser>,
        Badge<BaseBadgeUser> const&>);
    static_assert(!IsConstructible<
        Badge<BaseBadgeUser>,
        Badge<BaseBadgeUser, MultipleBadgeUserB> const&>);
    static_assert(!IsConstructible<
        Badge<BaseBadgeUser>,
        Badge<UnrelatedBadgeUser> const&>);

    BaseBadgeUser::call_base_badge();
    BaseBadgeUser::call_multi_base_badge();
    DerivedBadgeUser::call_base_badge();
    DerivedBadgeUser::call_multi_base_badge();
}

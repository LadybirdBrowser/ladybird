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
    static_assert(!IsConstructible<Badge<MultipleBadgeUserA, MultipleBadgeUserB>, Badge<MultipleBadgeUserA>>);
    static_assert(!IsConstructible<Badge<MultipleBadgeUserA, MultipleBadgeUserB, MultipleBadgeUserC, MultipleBadgeUserD, MultipleBadgeUserE>>);
    static_assert(!IsConstructible<
        Badge<MultipleBadgeUserA, MultipleBadgeUserB, MultipleBadgeUserC, MultipleBadgeUserD, MultipleBadgeUserE>,
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

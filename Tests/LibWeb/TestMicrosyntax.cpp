/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/HTML/Dates.h>

TEST_CASE(week_number_of_the_last_day)
{
    EXPECT_EQ(Web::HTML::week_number_of_the_last_day(1998), (u32)53);
    EXPECT_EQ(Web::HTML::week_number_of_the_last_day(1999), (u32)52);
    EXPECT_EQ(Web::HTML::week_number_of_the_last_day(2000), (u32)52);
    EXPECT_EQ(Web::HTML::week_number_of_the_last_day(2001), (u32)52);
    EXPECT_EQ(Web::HTML::week_number_of_the_last_day(2002), (u32)52);
    EXPECT_EQ(Web::HTML::week_number_of_the_last_day(2003), (u32)52);
    EXPECT_EQ(Web::HTML::week_number_of_the_last_day(2004), (u32)53);
    EXPECT_EQ(Web::HTML::week_number_of_the_last_day(2005), (u32)52);
    EXPECT_EQ(Web::HTML::week_number_of_the_last_day(2006), (u32)52);
    EXPECT_EQ(Web::HTML::week_number_of_the_last_day(2007), (u32)52);
    EXPECT_EQ(Web::HTML::week_number_of_the_last_day(2008), (u32)52);
    EXPECT_EQ(Web::HTML::week_number_of_the_last_day(2009), (u32)53);
}

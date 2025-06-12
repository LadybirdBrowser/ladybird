/*
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Array.h>
#include <AK/OptionParser.h>
#include <AK/String.h>
#include <AK/Vector.h>

TEST_CASE(string_option)
{
    ByteString short_options = "";
    int index_of_found_long_option = -1;
    Vector<OptionParser::Option> long_options;
    long_options.append(
        { "string_opt"sv,
            OptionParser::ArgumentRequirement::HasRequiredArgument,
            &index_of_found_long_option,
            0 });

    Array<StringView, 3> argument_array({ "app"sv, "--string_opt"sv, "string_opt_value"sv });
    Span<StringView> arguments(argument_array);
    size_t next_argument_index = 1;

    OptionParser parser;
    auto result = parser.getopt(arguments.slice(1), short_options, long_options, {});

    // found a long option
    EXPECT_EQ(result.result, 0);
    // found long option at index 0
    EXPECT_EQ(index_of_found_long_option, 0);
    // 2 args consumed: option name and value
    EXPECT_EQ(result.consumed_args, static_cast<size_t>(2));
    // option has a value
    EXPECT_EQ(result.optarg_value, "string_opt_value");

    next_argument_index += result.consumed_args;
    // we are past the end
    EXPECT_EQ(next_argument_index, static_cast<size_t>(3));
}

TEST_CASE(string_option_then_positional)
{
    ByteString short_options = "";
    int index_of_found_long_option = -1;
    Vector<OptionParser::Option> long_options;
    long_options.append(
        { "string_opt"sv,
            OptionParser::ArgumentRequirement::HasRequiredArgument,
            &index_of_found_long_option,
            0 });

    Array<StringView, 4> argument_array({ "app"sv, "--string_opt"sv, "string_opt_value"sv, "positional"sv });
    Span<StringView> arguments(argument_array);
    size_t next_argument_index = 1;

    OptionParser parser;
    auto result = parser.getopt(arguments.slice(1), short_options, long_options, {});

    // found a long option
    EXPECT_EQ(result.result, 0);
    // found long option at index 0
    EXPECT_EQ(index_of_found_long_option, 0);
    // 2 args consumed: option name and value
    EXPECT_EQ(result.consumed_args, static_cast<size_t>(2));
    // option has a value
    EXPECT_EQ(result.optarg_value, "string_opt_value");

    next_argument_index += result.consumed_args;
    // we are at "positional" index of arguments vector
    EXPECT_EQ(next_argument_index, static_cast<size_t>(3));
    EXPECT_EQ(arguments[next_argument_index], "positional");

    result = parser.getopt(arguments.slice(1), short_options, long_options, {});
    // there's no more options
    EXPECT_EQ(result.result, -1);
}

TEST_CASE(positional_then_string_option)
{
    ByteString short_options = "";
    int index_of_found_long_option = -1;
    Vector<OptionParser::Option> long_options;
    long_options.append(
        { "string_opt"sv,
            OptionParser::ArgumentRequirement::HasRequiredArgument,
            &index_of_found_long_option,
            0 });

    Array<StringView, 4> argument_array({ "app"sv, "positional"sv, "--string_opt"sv, "string_opt_value"sv });
    Span<StringView> arguments(argument_array);
    size_t next_argument_index = 1;

    OptionParser parser;
    auto result = parser.getopt(arguments.slice(1), short_options, long_options, {});

    // found a long option
    EXPECT_EQ(result.result, 0);
    // found long option at index 0
    EXPECT_EQ(index_of_found_long_option, 0);
    // 2 args consumed: option name and value
    EXPECT_EQ(result.consumed_args, static_cast<size_t>(2));
    // option has a value
    EXPECT_EQ(result.optarg_value, "string_opt_value");

    next_argument_index += result.consumed_args;
    // we are at "positional" index of arguments vector
    EXPECT_EQ(next_argument_index, static_cast<size_t>(3));
    EXPECT_EQ(arguments[next_argument_index], "positional");

    result = parser.getopt(arguments.slice(1), short_options, long_options, {});
    // there's no more options
    EXPECT_EQ(result.result, -1);
}

TEST_CASE(positional_then_string_option_then_bool_option)
{
    // #22759: Positional arguments were sometimes incorrectly not shifted, leading to an incorrect parse.

    ByteString short_options = "";
    int index_of_found_long_option = -1;
    Vector<OptionParser::Option> long_options;
    long_options.append(
        { "string_opt"sv,
            OptionParser::ArgumentRequirement::HasRequiredArgument,
            &index_of_found_long_option,
            0 });
    long_options.append(
        { "bool_opt"sv,
            OptionParser::ArgumentRequirement::NoArgument,
            &index_of_found_long_option,
            1 });

    Array<StringView, 5> argument_array({ "app"sv, "positional"sv, "--string_opt"sv, "string_opt_value"sv, "--bool_opt"sv });
    Span<StringView> arguments(argument_array);
    size_t next_argument_index = 1;

    OptionParser parser;
    auto result = parser.getopt(arguments.slice(1), short_options, long_options, {});
    // found a long option
    EXPECT_EQ(result.result, 0);
    // found long option at index 0
    EXPECT_EQ(index_of_found_long_option, 0);
    // 2 args consumed: option name and value
    EXPECT_EQ(result.consumed_args, static_cast<size_t>(2));
    // option has a value
    EXPECT_EQ(result.optarg_value, "string_opt_value");

    next_argument_index += result.consumed_args;
    EXPECT_EQ(next_argument_index, static_cast<size_t>(3));
    // positional argument has been shifted here
    EXPECT_EQ(arguments[next_argument_index], "positional");

    result = parser.getopt(arguments.slice(1), short_options, long_options, {});
    // found another long option
    EXPECT_EQ(result.result, 0);
    // found long option at index 1
    EXPECT_EQ(index_of_found_long_option, 1);
    // 1 arg consumed: option name
    EXPECT_EQ(result.consumed_args, static_cast<size_t>(1));

    next_argument_index += result.consumed_args;
    // "positional" argument has been shifted here
    EXPECT_EQ(next_argument_index, static_cast<size_t>(4));
    EXPECT_EQ(arguments[next_argument_index], "positional");

    result = parser.getopt(arguments.slice(1), short_options, long_options, {});
    // there's no more options
    EXPECT_EQ(result.result, -1);
}

/*
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OptionParser.h>

namespace AK {

void OptionParser::reset_state()
{
    m_arg_index = 0;
    m_consumed_args = 0;
    m_index_into_multioption_argument = 0;
    m_stop_on_first_non_option = false;
}

OptionParser::GetOptResult OptionParser::getopt(Span<StringView> args, StringView short_options, Span<Option const> long_options, Optional<int&> out_long_option_index)
{
    m_args = args;
    m_short_options = short_options;
    m_long_options = long_options;
    m_out_long_option_index = out_long_option_index;

    // In the following case:
    // $ foo bar -o baz
    // we want to parse the option (-o baz) first, and leave the argument (bar)
    // in argv after we return -1 when invoked the second time. So we reorder
    // argv to put options first and positional arguments next. To turn this
    // behavior off, start the short options spec with a "+". This is a GNU
    // extension that we support.
    m_stop_on_first_non_option = short_options.starts_with('+');

    bool should_reorder_argv = !m_stop_on_first_non_option;
    int res = -1;

    bool found_an_option = find_next_option();
    auto arg = current_arg();

    if (!found_an_option) {
        res = -1;
        if (arg == "--")
            m_consumed_args = 1;
        else
            m_consumed_args = 0;
    } else {
        // Alright, so we have an option on our hands!
        bool is_long_option = arg.starts_with("--"sv);
        if (is_long_option)
            res = handle_long_option();
        else
            res = handle_short_option();

        // If we encountered an error, return immediately.
        if (res == '?') {
            return {
                .result = '?',
                .optopt_value = m_optopt_value,
                .optarg_value = m_optarg_value,
                .consumed_args = 0,
            };
        }
    }

    if (should_reorder_argv)
        shift_argv();

    m_arg_index += m_consumed_args;

    return {
        .result = res,
        .optopt_value = m_optopt_value,
        .optarg_value = m_optarg_value,
        .consumed_args = m_consumed_args,
    };
}

Optional<OptionParser::ArgumentRequirement> OptionParser::lookup_short_option_requirement(char option) const
{
    Vector<StringView> parts = m_short_options.split_view(option, SplitBehavior::KeepEmpty);

    VERIFY(parts.size() <= 2);
    if (parts.size() < 2) {
        // Haven't found the option in the spec.
        return {};
    }

    if (parts[1].starts_with("::"sv)) {
        // If an option is followed by two colons, it optionally accepts an
        // argument.
        return ArgumentRequirement::HasOptionalArgument;
    }
    if (parts[1].starts_with(':')) {
        // If it's followed by one colon, it requires an argument.
        return ArgumentRequirement::HasRequiredArgument;
    }
    // Otherwise, it doesn't accept arguments.
    return ArgumentRequirement::NoArgument;
}

int OptionParser::handle_short_option()
{
    StringView arg = current_arg();
    VERIFY(arg.starts_with('-'));

    if (m_index_into_multioption_argument == 0) {
        // Just starting to parse this argument, skip the "-".
        m_index_into_multioption_argument = 1;
    }
    char option = arg[m_index_into_multioption_argument];
    m_index_into_multioption_argument++;

    auto maybe_requirement = lookup_short_option_requirement(option);
    if (!maybe_requirement.has_value()) {
        m_optopt_value = option;
        reportln("Unrecognized option \x1b[1m-{:c}\x1b[22m", option);
        return '?';
    }

    auto argument_requirement = *maybe_requirement;

    // Let's see if we're at the end of this argument already.
    if (m_index_into_multioption_argument < arg.length()) {
        // This not yet the end.
        if (argument_requirement == ArgumentRequirement::NoArgument) {
            m_optarg_value = {};
            m_consumed_args = 0;
        } else {
            // Treat the rest of the argument as the value, the "-ovalue"
            // syntax.
            m_optarg_value = m_args[m_arg_index].substring_view(m_index_into_multioption_argument);
            // Next time, process the next argument.
            m_index_into_multioption_argument = 0;
            m_consumed_args = 1;
        }
    } else {
        m_index_into_multioption_argument = 0;
        if (argument_requirement != ArgumentRequirement::HasRequiredArgument) {
            m_optarg_value = StringView();
            m_consumed_args = 1;
        } else if (m_arg_index + 1 < m_args.size()) {
            // Treat the next argument as a value, the "-o value" syntax.
            m_optarg_value = m_args[m_arg_index + 1];
            m_consumed_args = 2;
        } else {
            reportln("Missing value for option \x1b[1m-{:c}\x1b[22m", option);
            return '?';
        }
    }

    return option;
}

Optional<OptionParser::Option const&> OptionParser::lookup_long_option(StringView arg) const
{
    for (size_t index = 0; index < m_long_options.size(); index++) {
        auto& option = m_long_options[index];

        if (!arg.starts_with(option.name))
            continue;

        // It would be better to not write out the index at all unless we're
        // sure we've found the right option, but whatever.
        if (m_out_long_option_index.has_value())
            *m_out_long_option_index = index;

        // Can either be "--option" or "--option=value".
        if (arg.length() == option.name.length()) {
            m_optarg_value = {};
            return option;
        }

        if (arg[option.name.length()] == '=') {
            m_optarg_value = arg.substring_view(option.name.length() + 1);
            return option;
        }
    }

    return {};
}

int OptionParser::handle_long_option()
{
    VERIFY(current_arg().starts_with("--"sv));

    // We cannot set optopt to anything sensible for long options, so set it to 0.
    m_optopt_value = 0;

    auto option = lookup_long_option(m_args[m_arg_index].substring_view(2));
    if (!option.has_value()) {
        reportln("Unrecognized option \x1b[1m{}\x1b[22m", m_args[m_arg_index]);
        return '?';
    }
    // lookup_long_option() will also set an override for optarg if the value of the option is
    // specified using "--option=value" syntax.

    // Figure out whether this option needs and/or has a value (also called "an
    // argument", but let's not call it that to distinguish it from argv
    // elements).
    switch (option->requirement) {
    case ArgumentRequirement::NoArgument:
        if (m_optarg_value.has_value()) {
            reportln("Option \x1b[1m--{}\x1b[22m doesn't accept an argument", option->name);
            return '?';
        }
        m_consumed_args = 1;
        break;
    case ArgumentRequirement::HasOptionalArgument:
        m_consumed_args = 1;
        break;
    case ArgumentRequirement::HasRequiredArgument:
        if (m_optarg_value.has_value()) {
            // Value specified using "--option=value" syntax.
            m_consumed_args = 1;
        } else if (m_arg_index + 1 < m_args.size()) {
            // Treat the next argument as a value in "--option value" syntax.
            m_optarg_value = m_args[m_arg_index + 1];
            m_consumed_args = 2;
        } else {
            reportln("Missing value for option \x1b[1m--{}\x1b[22m", option->name);
            return '?';
        }
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    // Now that we've figured the value out, see about reporting this option to
    // our caller.
    if (option->flag != nullptr) {
        *option->flag = option->val;
        return 0;
    }
    return option->val;
}

void OptionParser::shift_argv()
{
    // We've just parsed an option (which perhaps has a value).
    // Put the option (along with its value, if any) in front of other arguments.
    if (m_consumed_args == 0 && m_skipped_arguments == 0) {
        // Nothing to do!
        return;
    }
    // x -a b c d
    //   ---- consumed
    // ->
    // -a b x c d

    StringView buffer[2]; // We consume at most 2 arguments in one call.
    Span<StringView> buffer_bytes { buffer, array_size(buffer) };
    m_args.slice(m_arg_index, m_consumed_args).copy_to(buffer_bytes);
    m_args.slice(m_arg_index - m_skipped_arguments, m_skipped_arguments).copy_to(m_args.slice(m_arg_index + m_consumed_args - m_skipped_arguments));
    buffer_bytes.slice(0, m_consumed_args).copy_to(m_args.slice(m_arg_index - m_skipped_arguments, m_consumed_args));

    // m_arg_index took into account m_skipped_arguments (both are inc in find_next_option)
    // so now we have to make m_arg_index point to the beginning of skipped arguments
    m_arg_index -= m_skipped_arguments;
    // and let's forget about skipped arguments
    m_skipped_arguments = 0;
}

bool OptionParser::find_next_option()
{
    for (m_skipped_arguments = 0; m_arg_index < m_args.size(); m_skipped_arguments++, m_arg_index++) {
        StringView arg = current_arg();
        // Anything that doesn't start with a "-" is not an option.
        // As a special case, a single "-" is not an option either.
        // (It's typically used by programs to refer to stdin).
        if (!arg.starts_with('-') || arg == "-") {
            if (m_stop_on_first_non_option)
                return false;
            continue;
        }

        // As another special case, a "--" is not an option either, and we stop
        // looking for further options if we encounter it.
        if (arg == "--")
            return false;
        // Otherwise, we have found an option!
        return true;
    }

    // Reached the end and still found no options.
    return false;
}

}

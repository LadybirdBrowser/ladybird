/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, Matthias Zimmerman <matthias291999@gmail.com>
 * Copyright (c) 2024, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Environment.h"
#include <AK/ByteString.h>

#if defined(AK_OS_MACOS) || defined(AK_OS_IOS)
#    include <crt_externs.h>
#elif !defined(AK_OS_WINDOWS)
extern "C" char** environ;
#endif

namespace Core::Environment {

char** raw_environ()
{
#if defined(AK_OS_MACOS) || defined(AK_OS_IOS)
    return *_NSGetEnviron();
#else
    return environ;
#endif
}

Entry Entry::from_chars(char const* input)
{
    return Entry::from_string({ input, strlen(input) });
}

Entry Entry::from_string(StringView input)
{
    auto split_index = input.find('=');
    if (!split_index.has_value()) {
        return Entry {
            .full_entry = input,
            .name = input,
            .value = ""sv,
        };
    }

    return Entry {
        .full_entry = input,
        .name = input.substring_view(0, *split_index),
        .value = input.substring_view(*split_index + 1),
    };
}

EntryIterator EntryIterator::begin()
{
    return EntryIterator(0);
}

EntryIterator EntryIterator::end()
{
    auto environment = raw_environ();

    size_t env_count = 0;
    for (size_t i = 0; environment[i]; ++i)
        ++env_count;
    return EntryIterator(env_count);
}

EntryIterator entries()
{
    return EntryIterator::begin();
}

size_t size()
{
    auto environment = raw_environ();

    size_t environ_size = 0;
    while (environment[environ_size])
        ++environ_size;

    return environ_size;
}

bool has(StringView name)
{
    return get(name).has_value();
}

Optional<StringView> get(StringView name, [[maybe_unused]] SecureOnly secure)
{
    StringBuilder builder;
    builder.append(name);
    builder.append('\0');
    // Note the explicit null terminators above.

    // FreeBSD < 14, Android, generic BSDs, and Windows do not support secure_getenv.
#if (defined(__FreeBSD__) && __FreeBSD__ >= 14) || (!defined(AK_OS_BSD_GENERIC) && !defined(AK_OS_ANDROID) && !defined(AK_OS_WINDOWS))
    char* result;
    if (secure == SecureOnly::Yes) {
        result = ::secure_getenv(builder.string_view().characters_without_null_termination());
    } else {
        result = ::getenv(builder.string_view().characters_without_null_termination());
    }
#else
    char* result = ::getenv(builder.string_view().characters_without_null_termination());
#endif
    if (result)
        return StringView { result, strlen(result) };
    return {};
}

ErrorOr<void> set(StringView name, StringView value, Overwrite overwrite)
{
    auto builder = TRY(StringBuilder::create());
    TRY(builder.try_append(name));
    TRY(builder.try_append('\0'));
    TRY(builder.try_append(value));
    TRY(builder.try_append('\0'));
    // Note the explicit null terminators above.
    auto c_name = builder.string_view().characters_without_null_termination();
    auto c_value = c_name + name.length() + 1;
#ifndef AK_OS_WINDOWS
    auto rc = ::setenv(c_name, c_value, overwrite == Overwrite::Yes ? 1 : 0);
#else
    if (overwrite == Overwrite::No && getenv(c_name))
        return {};
    auto rc = _putenv_s(c_name, c_value);
#endif
    if (rc < 0)
        return Error::from_errno(errno);
    return {};
}

ErrorOr<void> unset(StringView name)
{
    auto builder = TRY(StringBuilder::create());
    TRY(builder.try_append(name));
    TRY(builder.try_append('\0'));
    // Note the explicit null terminator above.
    auto c_name = builder.string_view().characters_without_null_termination();

#ifndef AK_OS_WINDOWS
    auto rc = ::unsetenv(c_name);
#else
    auto rc = _putenv_s(c_name, "");
#endif
    if (rc < 0)
        return Error::from_errno(errno);
    return {};
}

ErrorOr<void> put(StringView env)
{
#if defined(AK_OS_SERENITY)
    auto rc = ::serenity_putenv(env.characters_without_null_termination(), env.length());
#elif defined(AK_OS_WINDOWS)
    ByteString str = env;
    auto rc = ::putenv(str.characters());
#else
    // Leak somewhat unavoidable here due to the putenv API.
    auto leaked_new_env = strndup(env.characters_without_null_termination(), env.length());
    auto rc = ::putenv(leaked_new_env);
#endif
    if (rc < 0)
        return Error::from_errno(errno);
    return {};
}

ErrorOr<void> clear()
{
#if (defined(__FreeBSD__) && __FreeBSD__ < 14)
    environ = nullptr;
#elif defined(AK_OS_BSD_GENERIC) && !defined(AK_OS_FREEBSD)
    auto environment = raw_environ();
    for (size_t environ_size = 0; environment[environ_size]; ++environ_size) {
        environment[environ_size] = NULL;
    }
#elif defined(AK_OS_WINDOWS)
    // environ = 0 doesn't work on Windows
    while (auto str = environ[0]) {
        auto eq = strchr(str, '=');
        VERIFY(eq);
        size_t name_len = eq - str;
        auto result = unset({ str, name_len });
        if (result.is_error())
            return result;
    }
#else
    auto rc = ::clearenv();
    if (rc < 0)
        return Error::from_errno(errno);
#endif
    return {};
}

}

/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/LexicalPath.h>
#include <AK/Platform.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/StringUtils.h>
#include <LibCore/Environment.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <stdlib.h>

#if !defined(AK_OS_WINDOWS)
#    include <pwd.h>
#    include <unistd.h>
#endif

namespace Core {

static Optional<StringView> get_environment_if_not_empty(StringView name)
{
    auto maybe_value = Core::Environment::get(name);
    if (maybe_value.has_value() && maybe_value->trim_whitespace().is_empty())
        return {};
    return maybe_value;
}

ByteString StandardPaths::home_directory()
{
#if defined(AK_OS_WINDOWS)
    ByteString path = getenv("USERPROFILE");
#else
    if (auto* home_env = getenv("HOME"))
        return LexicalPath::canonicalized_path(home_env);

    auto* pwd = getpwuid(getuid());
    ByteString path = pwd ? pwd->pw_dir : "/";
    endpwent();
#endif
    return LexicalPath::canonicalized_path(path);
}

ByteString StandardPaths::desktop_directory()
{
#if !defined(AK_OS_WINDOWS)
    if (auto desktop_directory = get_environment_if_not_empty("XDG_DESKTOP_DIR"sv); desktop_directory.has_value())
        return LexicalPath::canonicalized_path(*desktop_directory);
#endif

    StringBuilder builder;
    builder.append(home_directory());
    builder.append("/Desktop"sv);
    return LexicalPath::canonicalized_path(builder.to_byte_string());
}

ByteString StandardPaths::documents_directory()
{
#if !defined(AK_OS_WINDOWS)
    if (auto documents_directory = get_environment_if_not_empty("XDG_DOCUMENTS_DIR"sv); documents_directory.has_value())
        return LexicalPath::canonicalized_path(*documents_directory);
#endif

    StringBuilder builder;
    builder.append(home_directory());
    builder.append("/Documents"sv);
    return LexicalPath::canonicalized_path(builder.to_byte_string());
}

ByteString StandardPaths::downloads_directory()
{
#if !defined(AK_OS_WINDOWS)
    if (auto downloads_directory = get_environment_if_not_empty("XDG_DOWNLOAD_DIR"sv); downloads_directory.has_value())
        return LexicalPath::canonicalized_path(*downloads_directory);
#endif

    StringBuilder builder;
    builder.append(home_directory());
    builder.append("/Downloads"sv);
    return LexicalPath::canonicalized_path(builder.to_byte_string());
}

ByteString StandardPaths::music_directory()
{
#if !defined(AK_OS_WINDOWS)
    if (auto music_directory = get_environment_if_not_empty("XDG_MUSIC_DIR"sv); music_directory.has_value())
        return LexicalPath::canonicalized_path(*music_directory);
#endif

    StringBuilder builder;
    builder.append(home_directory());
    builder.append("/Music"sv);
    return LexicalPath::canonicalized_path(builder.to_byte_string());
}

ByteString StandardPaths::pictures_directory()
{
#if !defined(AK_OS_WINDOWS)
    if (auto pictures_directory = get_environment_if_not_empty("XDG_PICTURES_DIR"sv); pictures_directory.has_value())
        return LexicalPath::canonicalized_path(*pictures_directory);
#endif

    StringBuilder builder;
    builder.append(home_directory());
    builder.append("/Pictures"sv);
    return LexicalPath::canonicalized_path(builder.to_byte_string());
}

ByteString StandardPaths::videos_directory()
{
#if !defined(AK_OS_WINDOWS)
    if (auto videos_directory = get_environment_if_not_empty("XDG_VIDEOS_DIR"sv); videos_directory.has_value())
        return LexicalPath::canonicalized_path(*videos_directory);
#endif

    StringBuilder builder;
    builder.append(home_directory());
#if defined(AK_OS_MACOS)
    builder.append("/Movies"sv);
#else
    builder.append("/Videos"sv);
#endif
    return LexicalPath::canonicalized_path(builder.to_byte_string());
}

ByteString StandardPaths::config_directory()
{
#ifdef AK_OS_WINDOWS
    dbgln("Core::StandardPaths::config_directory() is not implemented");
    VERIFY_NOT_REACHED();
#endif
    if (auto config_directory = get_environment_if_not_empty("XDG_CONFIG_HOME"sv); config_directory.has_value())
        return LexicalPath::canonicalized_path(*config_directory);

    StringBuilder builder;
    builder.append(home_directory());
#if defined(AK_OS_MACOS)
    builder.append("/Library/Preferences"sv);
#elif defined(AK_OS_HAIKU)
    builder.append("/config/settings"sv);
#else
    builder.append("/.config"sv);
#endif
    return LexicalPath::canonicalized_path(builder.to_byte_string());
}

ByteString StandardPaths::user_data_directory()
{
#ifdef AK_OS_WINDOWS
    return ByteString::formatted("{}/Ladybird"sv, getenv("LOCALAPPDATA"));
#endif
    if (auto data_directory = get_environment_if_not_empty("XDG_DATA_HOME"sv); data_directory.has_value())
        return LexicalPath::canonicalized_path(*data_directory);

    StringBuilder builder;
    builder.append(home_directory());
#if defined(AK_OS_SERENITY)
    builder.append("/.data"sv);
#elif defined(AK_OS_MACOS)
    builder.append("/Library/Application Support"sv);
#elif defined(AK_OS_HAIKU)
    builder.append("/config/non-packaged/data"sv);
#else
    builder.append("/.local/share"sv);
#endif

    return LexicalPath::canonicalized_path(builder.to_byte_string());
}

Vector<ByteString> StandardPaths::system_data_directories()
{
#ifdef AK_OS_WINDOWS
    dbgln("Core::StandardPaths::system_data_directories() is not implemented");
    VERIFY_NOT_REACHED();
#endif
    auto data_directories = get_environment_if_not_empty("XDG_DATA_DIRS"sv).value_or("/usr/local/share:/usr/share"sv);
    Vector<ByteString> paths;
    data_directories.for_each_split_view(':', SplitBehavior::Nothing, [&paths](auto data_directory) {
        paths.append(LexicalPath::canonicalized_path(data_directory));
    });
    return paths;
}

ErrorOr<ByteString> StandardPaths::runtime_directory()
{
#if !defined(AK_OS_WINDOWS)
    if (auto data_directory = get_environment_if_not_empty("XDG_RUNTIME_DIR"sv); data_directory.has_value())
        return LexicalPath::canonicalized_path(*data_directory);
#endif

    StringBuilder builder;

#if defined(AK_OS_MACOS)
    builder.append(home_directory());
    builder.append("/Library/Application Support"sv);
#elif defined(AK_OS_HAIKU)
    builder.append("/boot/system/var/shared_memory"sv);
#elif defined(AK_OS_LINUX)
    auto uid = getuid();
    builder.appendff("/run/user/{}", uid);
#elif defined(AK_OS_WINDOWS)
    builder.appendff("{}", getenv("TEMP"));
#else
    // Just create a directory in /tmp that's owned by us with 0700
    auto uid = getuid();
    builder.appendff("/tmp/runtime_{}", uid);
    auto error_or_stat = System::stat(builder.string_view());
    if (error_or_stat.is_error()) {
        MUST(System::mkdir(builder.string_view(), 0700));
    } else {
        auto stat = error_or_stat.release_value();
        VERIFY(S_ISDIR(stat.st_mode));
        if ((stat.st_mode & 0777) != 0700)
            warnln("{} has unexpected mode flags {}", builder.string_view(), stat.st_mode);
    }
#endif

    return LexicalPath::canonicalized_path(builder.to_byte_string());
}

ByteString StandardPaths::tempfile_directory()
{
#if defined(AK_OS_WINDOWS)
    return getenv("TEMP");
#else
    return "/tmp";
#endif
}

}

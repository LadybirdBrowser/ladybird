/*
 * Copyright (c) 2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
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
#include <LibCore/SessionManagement.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <pwd.h>
#include <stdlib.h>
#include <unistd.h>

#if defined(AK_OS_HAIKU)
#    include <FindDirectory.h>
#endif

namespace Core {

ByteString StandardPaths::home_directory()
{
    if (auto* home_env = getenv("HOME"))
        return LexicalPath::canonicalized_path(home_env);

    auto* pwd = getpwuid(getuid());
    ByteString path = pwd ? pwd->pw_dir : "/";
    endpwent();
    return LexicalPath::canonicalized_path(path);
}

ByteString StandardPaths::desktop_directory()
{
    if (auto* desktop_directory = getenv("XDG_DESKTOP_DIR"))
        return LexicalPath::canonicalized_path(desktop_directory);

    StringBuilder builder;
    builder.append(home_directory());
    builder.append("/Desktop"sv);
    return LexicalPath::canonicalized_path(builder.to_byte_string());
}

ByteString StandardPaths::documents_directory()
{
    if (auto* documents_directory = getenv("XDG_DOCUMENTS_DIR"))
        return LexicalPath::canonicalized_path(documents_directory);

    StringBuilder builder;
    builder.append(home_directory());
    builder.append("/Documents"sv);
    return LexicalPath::canonicalized_path(builder.to_byte_string());
}

ByteString StandardPaths::downloads_directory()
{
    if (auto* downloads_directory = getenv("XDG_DOWNLOAD_DIR"))
        return LexicalPath::canonicalized_path(downloads_directory);

    StringBuilder builder;
    builder.append(home_directory());
    builder.append("/Downloads"sv);
    return LexicalPath::canonicalized_path(builder.to_byte_string());
}

ByteString StandardPaths::music_directory()
{
    if (auto* music_directory = getenv("XDG_MUSIC_DIR"))
        return LexicalPath::canonicalized_path(music_directory);

    StringBuilder builder;
    builder.append(home_directory());
    builder.append("/Music"sv);
    return LexicalPath::canonicalized_path(builder.to_byte_string());
}

ByteString StandardPaths::pictures_directory()
{
    if (auto* pictures_directory = getenv("XDG_PICTURES_DIR"))
        return LexicalPath::canonicalized_path(pictures_directory);

    StringBuilder builder;
    builder.append(home_directory());
    builder.append("/Pictures"sv);
    return LexicalPath::canonicalized_path(builder.to_byte_string());
}

ByteString StandardPaths::videos_directory()
{
    if (auto* videos_directory = getenv("XDG_VIDEOS_DIR"))
        return LexicalPath::canonicalized_path(videos_directory);

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
    if (auto* config_directory = getenv("XDG_CONFIG_HOME"))
        return LexicalPath::canonicalized_path(config_directory);

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

ByteString StandardPaths::data_directory()
{
    if (auto* data_directory = getenv("XDG_DATA_HOME"))
        return LexicalPath::canonicalized_path(data_directory);

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

ErrorOr<ByteString> StandardPaths::runtime_directory()
{
    if (auto* data_directory = getenv("XDG_RUNTIME_DIR"))
        return LexicalPath::canonicalized_path(data_directory);

    StringBuilder builder;

#if defined(AK_OS_SERENITY)
    auto sid = TRY(Core::SessionManagement::root_session_id());
    builder.appendff("/tmp/session/{}", sid);
#elif defined(AK_OS_MACOS)
    builder.append(home_directory());
    builder.append("/Library/Application Support"sv);
#elif defined(AK_OS_HAIKU)
    builder.append("/boot/system/var/shared_memory"sv);
#elif defined(AK_OS_LINUX)
    auto uid = getuid();
    builder.appendff("/run/user/{}", uid);
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
    return "/tmp";
}

ErrorOr<Vector<String>> StandardPaths::font_directories()
{
#if defined(AK_OS_HAIKU)
    Vector<String> paths_vector;
    char** paths;
    size_t paths_count;
    if (find_paths(B_FIND_PATH_FONTS_DIRECTORY, NULL, &paths, &paths_count) == B_OK) {
        for (size_t i = 0; i < paths_count; ++i) {
            StringBuilder builder;
            builder.append(paths[i], strlen(paths[i]));
            paths_vector.append(TRY(builder.to_string()));
        }
    }
    return paths_vector;
#else
    auto paths = Vector { {
#    if defined(AK_OS_SERENITY)
        "/res/fonts"_string,
#    elif defined(AK_OS_MACOS)
        "/System/Library/Fonts"_string,
        "/Library/Fonts"_string,
        TRY(String::formatted("{}/Library/Fonts"sv, home_directory())),
#    elif defined(AK_OS_ANDROID)
        // FIXME: We should be using the ASystemFontIterator NDK API here.
        // There is no guarantee that this will continue to exist on future versions of Android.
        "/system/fonts"_string,
#    else
        TRY(String::formatted("{}/fonts"sv, data_directory())),
        TRY(String::formatted("{}/X11/fonts"sv, data_directory())),
#    endif
    } };
#    if !(defined(AK_OS_SERENITY) || defined(AK_OS_MACOS))
    auto data_directories = Core::Environment::get("XDG_DATA_DIRS"sv).value_or("/usr/local/share:/usr/share"sv);
    TRY(data_directories.for_each_split_view(':', SplitBehavior::Nothing, [&paths](auto data_directory) -> ErrorOr<void> {
        paths.append(TRY(String::formatted("{}/fonts"sv, data_directory)));
        paths.append(TRY(String::formatted("{}/X11/fonts"sv, data_directory)));
        return {};
    }));
#    endif
    return paths;
#endif
}

}

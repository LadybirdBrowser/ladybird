/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/Platform.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/File.h>
#include <LibCore/Process.h>
#include <LibCore/Resource.h>
#include <LibCore/ResourceImplementationFile.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWeb/HTML/SelectedFile.h>
#include <LibWebView/Utilities.h>

#define TOKENCAT(x, y) x##y
#define STRINGIFY(x) TOKENCAT(x, sv)

namespace WebView {

// This is expected to be set from the build scripts, if a packager desires
#if defined(LADYBIRD_LIBEXECDIR)
static constexpr auto libexec_path = STRINGIFY(LADYBIRD_LIBEXECDIR);
#else
static constexpr auto libexec_path = "libexec"sv;
#endif

ByteString& s_ladybird_resource_root = *new ByteString;
static auto& s_ladybird_binary_path = *new Optional<ByteString>;

Optional<ByteString>& s_mach_server_name = *new Optional<ByteString>;

Optional<ByteString const&> mach_server_name()
{
    if (s_mach_server_name.has_value())
        return *s_mach_server_name;
    return {};
}

void set_mach_server_name(ByteString name)
{
    s_mach_server_name = move(name);
}

ByteString mach_server_name_for_process(StringView process_name, pid_t pid)
{
    return ByteString::formatted("org.ladybird.{}.helper.{}", process_name, pid);
}

static ErrorOr<ByteString> application_directory()
{
    if (s_ladybird_binary_path.has_value())
        return *s_ladybird_binary_path;

    auto current_executable_path = TRY(Core::System::current_executable_path());
    return LexicalPath::dirname(current_executable_path);
}

[[gnu::used]] static LexicalPath find_prefix(LexicalPath const& application_directory);
static LexicalPath find_prefix(LexicalPath const& application_directory)
{
    if (application_directory.string().ends_with(libexec_path)) {
        // Strip libexec_path if it's there
        return LexicalPath(application_directory.string().substring_view(0, application_directory.string().length() - libexec_path.length()));
    }

    // Otherwise, we are in $prefix/bin
    return application_directory.parent();
}

void platform_init(Optional<ByteString> ladybird_binary_path)
{
    s_ladybird_binary_path = move(ladybird_binary_path);

    s_ladybird_resource_root = [] {
        auto home = Core::Environment::get("XDG_CONFIG_HOME"sv)
                        .value_or_lazy_evaluated_optional([]() { return Core::Environment::get("HOME"sv); });
        if (home.has_value()) {
            auto home_lagom = ByteString::formatted("{}/.lagom", home);
            if (FileSystem::is_directory(home_lagom))
                return home_lagom;
        }
        auto app_dir = MUST(application_directory());
#ifdef AK_OS_MACOS
        return LexicalPath(app_dir).parent().append("Resources"sv).string();
#else
        return find_prefix(LexicalPath(app_dir)).append("share/Lagom"sv).string();
#endif
    }();

    Core::ResourceImplementation::install(make<Core::ResourceImplementationFile>(MUST(String::from_byte_string(s_ladybird_resource_root))));
}

ErrorOr<Vector<ByteString>> get_paths_for_helper_process(StringView process_name)
{
    auto application_path = TRY(application_directory());
    Vector<ByteString> paths;

#if !defined(AK_OS_MACOS) && !defined(AK_OS_WINDOWS)
    auto prefix = find_prefix(LexicalPath(application_path));
    TRY(paths.try_append(LexicalPath::join(prefix.string(), libexec_path, process_name).string()));
    TRY(paths.try_append(LexicalPath::join(prefix.string(), "bin"sv, process_name).string()));
#endif
    TRY(paths.try_append(ByteString::formatted("{}/{}", application_path, process_name)));
    TRY(paths.try_append(ByteString::formatted("./{}", process_name)));
    // NOTE: Add platform-specific paths here
    return paths;
}

ErrorOr<void> handle_attached_debugger()
{
#if defined(AK_OS_LINUX)
    // Let's ignore SIGINT if we're being debugged because GDB incorrectly forwards the signal to us even when it's set
    // to "nopass". See https://sourceware.org/bugzilla/show_bug.cgi?id=9425 for details.
    if (TRY(Core::Process::is_being_debugged())) {
        dbgln("Debugger is attached, ignoring SIGINT");
        TRY(Core::System::signal(SIGINT, SIG_IGN));
    }
#endif

    return {};
}

ErrorOr<Web::HTML::SelectedFile> create_selected_file(ByteString const& file_path)
{
    // FIXME: Implement the File and Directory Entries API.
    //        https://wicg.github.io/entries-api/
    if (FileSystem::is_directory(file_path))
        return Error::from_string_literal("Only files may currently be selected");

    // https://html.spec.whatwg.org/multipage/input.html#file-upload-state-(type=file):concept-input-file-path
    // Filenames must not contain path components, even in the case that a user has selected an entire directory
    // hierarchy or multiple files with the same name from different directories.
    auto name = LexicalPath::basename(file_path);

    auto file = TRY(Core::File::open(file_path, Core::File::OpenMode::Read));
    return Web::HTML::SelectedFile { move(name), IPC::File::adopt_file(move(file)) };
}

ErrorOr<JsonObject> read_json_file(ByteString const& path)
{
    auto file = Core::File::open(path, Core::File::OpenMode::Read);
    if (file.is_error()) {
        if (file.error().is_errno() && file.error().code() == ENOENT)
            return JsonObject {};
        return file.release_error();
    }

    auto contents = TRY(file.value()->read_until_eof());
    auto json = TRY(JsonValue::from_string(contents));

    if (!json.is_object())
        return Error::from_string_literal("Expected parsed JSON value to be an object");
    return move(json.as_object());
}

ErrorOr<void> write_json_file(ByteString const& path, JsonValue const& value)
{
    auto directory = LexicalPath { path }.parent();
    TRY(Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes));

    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Write));
    TRY(file->write_until_depleted(value.serialized()));

    return {};
}

}

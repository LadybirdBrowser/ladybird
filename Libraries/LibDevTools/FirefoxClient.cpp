/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/ByteString.h>
#include <AK/LexicalPath.h>
#include <AK/NeverDestroyed.h>
#include <LibCore/ConfigFile.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibDevTools/FirefoxClient.h>
#include <LibFileSystem/FileSystem.h>

#if !defined(AK_OS_WINDOWS)
#    include <unistd.h>
#endif

namespace DevTools {

#if defined(AK_OS_LINUX)
static constexpr auto PROFILE_NAME = "LadybirdDevTools"sv;
#endif

static constexpr auto PROFILE_PREFS = R"~~~(
user_pref("app.update.auto", false);
user_pref("browser.aboutwelcome.enabled", false);
user_pref("browser.sessionstore.resume_from_crash", false);
user_pref("browser.shell.checkDefaultBrowser", false);
user_pref("browser.startup.couldRestoreSession.count", -1);
user_pref("browser.startup.homepage_override.mstone", "ignore");
user_pref("browser.tabs.inTitlebar", 0);
user_pref("browser.warnOnQuit", false);
user_pref("datareporting.policy.dataSubmissionEnabled", false);
user_pref("devtools.aboutdebugging.network-locations", "[]");
user_pref("devtools.chrome.enabled", true);
user_pref("devtools.debugger.prompt-connection", false);
user_pref("devtools.debugger.remote-enabled", true);
user_pref("termsofuse.bypassNotification", true);
user_pref("toolkit.legacyUserProfileCustomizations.stylesheets", true);
user_pref("toolkit.startup.max_resumed_crashes", -1);
)~~~"sv;

static constexpr auto USER_CHROME_CSS = R"~~~(
#nav-bar,
#PersonalToolbar,
#TabsToolbar {
    visibility: collapse !important;
}
)~~~"sv;

static LexicalPath const& profile_directory()
{
    static NeverDestroyed<LexicalPath> path = LexicalPath::join(Core::StandardPaths::user_data_directory(), "Ladybird"sv, "DevTools"sv, "firefox-profile"sv);
    return *path;
}

static ErrorOr<void> prepare_firefox_profile(LexicalPath const& profile)
{
    auto open_regular_file = [](Core::Directory const& directory, StringView filename) -> ErrorOr<NonnullOwnPtr<Core::File>> {
        auto file = TRY(directory.open(filename, Core::File::OpenMode::ReadWrite | Core::File::OpenMode::NoFollow));
        if (!S_ISREG(TRY(file->stat()).st_mode))
            return Error::from_errno(EINVAL);

        TRY(file->truncate(0));
        return file;
    };

    // Below we go out of our way to avoid following symbolic links in any existing profile. So entries such as
    // "user.js" cannot redirect writes outside the profile.
    auto profile_directory = TRY(Core::Directory::create(profile, Core::Directory::CreateDirectories::Yes));

    auto prefs = TRY(open_regular_file(profile_directory, "user.js"sv));
    TRY(prefs->write_until_depleted(PROFILE_PREFS.bytes()));

    auto chrome = profile.append("chrome"sv);
    TRY(Core::Directory::create(chrome, Core::Directory::CreateDirectories::Yes));

    auto chrome_file = TRY(profile_directory.open("chrome"sv, Core::File::OpenMode::Read | Core::File::OpenMode::NoFollow));
    if (!S_ISDIR(TRY(chrome_file->stat()).st_mode))
        return Error::from_errno(ENOTDIR);
    auto chrome_directory = TRY(Core::Directory::adopt_fd(chrome_file->leak_fd(), move(chrome)));

    auto user_chrome = TRY(open_regular_file(chrome_directory, "userChrome.css"sv));
    TRY(user_chrome->write_until_depleted(USER_CHROME_CSS.bytes()));

    return {};
}

struct FirefoxInstallation {
    LexicalPath executable;
    Vector<ByteString> arguments;
    Optional<LexicalPath> profiles_ini;
};

#if defined(AK_OS_LINUX)
static bool flatpak_firefox_is_installed(LexicalPath const& flatpak)
{
    auto process = Core::Process::spawn({
        .name = "find-firefox-flatpak"sv,
        .executable = flatpak.string(),
        .die_with_parent = true,
        .arguments = {
            "info"sv,
            "org.mozilla.firefox"sv,
        },
        .file_actions = {
            Core::FileAction::OpenFile { "/dev/null", Core::File::OpenMode::Write, STDOUT_FILENO },
            Core::FileAction::OpenFile { "/dev/null", Core::File::OpenMode::Write, STDERR_FILENO },
        },
    });
    if (process.is_error())
        return false;

    auto exit_code = process.value().wait_for_termination();
    return !exit_code.is_error() && exit_code.value() == 0;
}

static ErrorOr<Optional<LexicalPath>> find_firefox_profile(LexicalPath const& profiles_ini, StringView profile_name)
{
    auto profiles = TRY(Core::ConfigFile::open(profiles_ini.string()));

    for (auto const& group : profiles->groups()) {
        if (!group.starts_with("Profile"sv) || profiles->read_entry(group, "Name"sv) != profile_name)
            continue;

        auto path = profiles->read_entry(group, "Path"sv);
        if (path.is_empty())
            continue;

        if (!profiles->read_bool_entry(group, "IsRelative"sv))
            return LexicalPath { move(path) };

        return LexicalPath::join(profiles_ini.dirname(), path);
    }

    return OptionalNone {};
}

static ErrorOr<LexicalPath> ensure_confined_firefox_profile(FirefoxInstallation const& installation)
{
    VERIFY(installation.profiles_ini.has_value());

    auto profile = TRY(find_firefox_profile(*installation.profiles_ini, PROFILE_NAME));
    if (!profile.has_value()) {
        auto arguments = installation.arguments;
        arguments.append("--createprofile"sv);
        arguments.append(PROFILE_NAME);

        auto process = TRY(Core::Process::spawn({
            .name = "create-devtools-profile"sv,
            .executable = installation.executable.string(),
            .die_with_parent = true,
            .arguments = arguments,
        }));
        if (TRY(process.wait_for_termination()) != 0)
            return Error::from_string_literal("Firefox failed to create the Ladybird DevTools profile");

        profile = TRY(find_firefox_profile(*installation.profiles_ini, PROFILE_NAME));
        if (!profile.has_value())
            return Error::from_string_literal("Firefox did not register the Ladybird DevTools profile");
    }

    auto profiles_directory = LexicalPath { installation.profiles_ini->dirname() };
    if (!profile->is_child_of(profiles_directory))
        return Error::from_string_literal("The Ladybird DevTools profile is outside the Firefox sandbox");

    return profile.release_value();
}
#endif

static Optional<FirefoxInstallation> firefox_installation_from_binary(StringView candidate)
{
    if (!FileSystem::is_regular_file(candidate))
        return {};
#if !defined(AK_OS_WINDOWS)
    if (Core::System::access(candidate, X_OK).is_error())
        return {};
#endif

    auto canonical_path = FileSystem::real_path(candidate);
    if (canonical_path.is_error())
        return {};

#if defined(AK_OS_LINUX)
    auto executable = LexicalPath { canonical_path.value() };
    if (executable.basename() == "flatpak"sv) {
        if (!flatpak_firefox_is_installed(executable))
            return {};

        return FirefoxInstallation {
            .executable = move(executable),
            .arguments = {
                "run"sv,
                "--env=MOZ_GTK_TITLEBAR_DECORATION=system"sv,
                "org.mozilla.firefox"sv,
            },
            .profiles_ini = LexicalPath::join(Core::StandardPaths::home_directory(), ".var"sv, "app"sv, "org.mozilla.firefox"sv, ".mozilla"sv, "firefox"sv, "profiles.ini"sv),
        };
    }

    if (canonical_path.value() == "/usr/bin/snap"sv) {
        return FirefoxInstallation {
            .executable = LexicalPath { candidate },
            .arguments = {},
            .profiles_ini = LexicalPath::join(Core::StandardPaths::home_directory(), "snap"sv, "firefox"sv, "common"sv, ".mozilla"sv, "firefox"sv, "profiles.ini"sv),
        };
    }

    if (candidate.starts_with("/snap/"sv) || canonical_path.value().starts_with("/snap/"sv))
        return {};
    if (candidate.starts_with("/var/lib/flatpak/"sv) || canonical_path.value().starts_with("/var/lib/flatpak/"sv))
        return {};
    if (candidate.contains("/.var/app/org.mozilla.firefox/"sv) || canonical_path.value().contains("/.var/app/org.mozilla.firefox/"sv))
        return {};
#endif

    return FirefoxInstallation {
        .executable = LexicalPath { canonical_path.release_value() },
        .arguments = {},
        .profiles_ini = {},
    };
}

static Optional<FirefoxInstallation> find_firefox_in_path()
{
    static constexpr auto EXECUTABLE_NAMES = to_array<StringView>({
        "firefox"sv,
#if defined(AK_OS_LINUX)
        "flatpak"sv,
#endif
    });

    auto search_path = Core::Environment::get("PATH"sv).value_or(DEFAULT_PATH_SV);

    for (auto executable_name : EXECUTABLE_NAMES) {
        for (auto directory : search_path.split_view(':')) {
            auto candidate = LexicalPath::join(directory, executable_name);

            if (auto installation = firefox_installation_from_binary(candidate.string()); installation.has_value())
                return installation;
        }
    }

    return {};
}

static Optional<FirefoxInstallation> find_firefox_installation()
{
#if defined(AK_OS_MACOS)
    static constexpr auto APPLICATION_NAME = "Firefox.app"sv;

    auto candidate = LexicalPath::join("/Applications"sv, APPLICATION_NAME, "Contents"sv, "MacOS"sv, "firefox"sv);
    if (auto installation = firefox_installation_from_binary(candidate.string()); installation.has_value())
        return installation;

    return find_firefox_in_path();
#else
    return find_firefox_in_path();
#endif
}

NonnullOwnPtr<FirefoxClient> FirefoxClient::create()
{
    return adopt_own(*new FirefoxClient());
}

FirefoxClient::~FirefoxClient()
{
    if (is_running())
        (void)Core::Process::terminate_process(m_process->pid(), Core::Process::TerminationMode::Graceful);
}

bool FirefoxClient::is_running() const
{
#if defined(AK_OS_WINDOWS)
    // FIXME: Implement actual live-process detection for this PID on Windows.
    return m_process.has_value();
#else
    return m_process.has_value() && !Core::System::kill(m_process->pid(), 0).is_error();
#endif
}

ErrorOr<void> FirefoxClient::ensure_running(u16 port, u64 tab_id)
{
    if (is_running())
        return {};

    auto installation = find_firefox_installation();
    if (!installation.has_value())
        return Error::from_string_literal("No Firefox installation was found");

    auto arguments = installation->arguments;
    arguments.append("-no-remote"sv);
#if defined(AK_OS_MACOS)
    arguments.append("-foreground"sv);
#endif

#if defined(AK_OS_LINUX)
    if (installation->profiles_ini.has_value()) {
        auto profile = TRY(ensure_confined_firefox_profile(*installation));
        TRY(prepare_firefox_profile(profile));

        arguments.append("-P"sv);
        arguments.append(PROFILE_NAME);
    } else
#endif
    {
        TRY(prepare_firefox_profile(profile_directory()));

        arguments.append("-profile"sv);
        arguments.append(profile_directory().string());
    }

    arguments.extend({
        "--width"sv,
        "1280"sv,
        "--height"sv,
        "850"sv,
        ByteString::formatted("about:devtools-toolbox?type=tab&id={}&host=localhost&port={}", tab_id, port),
    });

    auto executable = installation->executable.string();
    bool search_for_executable_in_path = false;

#if defined(AK_OS_LINUX)
    if (installation->executable.basename() != "flatpak"sv) {
        arguments.prepend({
            "MOZ_GTK_TITLEBAR_DECORATION=system"sv,
            move(executable),
        });
        executable = "env";
        search_for_executable_in_path = true;
    }
#endif

    m_process = TRY(Core::Process::spawn({
        .name = "devtools-client"sv,
        .executable = move(executable),
        .search_for_executable_in_path = search_for_executable_in_path,
        .die_with_parent = true,
        .arguments = arguments,
    }));

    return {};
}

}

/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/AnyOf.h>
#include <AK/CharacterTypes.h>
#include <AK/GenericShorthands.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/StringBuilder.h>
#include <LibCore/BrowserVersion.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/Timer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibHTTP/HeaderList.h>
#include <LibRequests/NetworkError.h>
#include <LibRequests/Request.h>
#include <LibRequests/RequestClient.h>
#include <LibURL/Parser.h>
#include <LibWebCommon/Loader/UserAgent.h>
#include <LibWebCommon/WebView/Utilities.h>
#include <LibWebView/Application.h>
#include <LibWebView/Settings.h>
#include <LibWebView/UpdateCheck.h>
#include <LibWebView/Utilities.h>

#if defined(AK_OS_MACOS)
#    include <sys/sysctl.h>
#endif

#if defined(AK_OS_WINDOWS) && defined(LADYBIRD_HAS_UPDATE_CHECK)
#    include <AK/Windows.h>
#endif

namespace WebView {

#if defined(LADYBIRD_HAS_UPDATE_CHECK)

static constexpr size_t maximum_feed_size = 4 * MiB;
static constexpr auto request_timeout = AK::Duration::from_seconds(30);

// A timer waits at most this long, and then works out again how long until the next check is due.
static constexpr auto longest_timer_wait = AK::Duration::from_seconds(24 * 60 * 60);

// Restarting Ladybird soon after a check does not check again.
static constexpr auto shortest_wait_before_check_at_start = AK::Duration::from_seconds(5 * 60);

// The system updates a package by itself, so an automatic check only reminds the user of a release that the system has
// left uninstalled for this long.
static constexpr auto package_update_grace_period = AK::Duration::from_seconds(14 * 24 * 60 * 60);

static constexpr i64 STATE_VERSION = 1;
static constexpr auto VERSION_KEY = "version"sv;
static constexpr auto FIRST_START_KEY = "firstStart"sv;
static constexpr auto LAST_CHECK_KEY = "lastCheck"sv;
static constexpr auto SKIPPED_VERSION_KEY = "skippedVersion"sv;

Optional<Vector<u64>> parse_version(StringView version)
{
    Vector<u64> parts;
    for (auto part : version.split_view('.', SplitBehavior::KeepEmpty)) {
        if (part.is_empty() || !all_of(part, is_ascii_digit))
            return {};
        auto number = part.to_number<u64>();
        if (!number.has_value())
            return {};
        parts.append(*number);
    }
    if (parts.is_empty())
        return {};
    return parts;
}

static int compare_versions(Vector<u64> const& a, Vector<u64> const& b)
{
    for (size_t i = 0; i < max(a.size(), b.size()); ++i) {
        auto a_part = i < a.size() ? a[i] : 0;
        auto b_part = i < b.size() ? b[i] : 0;
        if (a_part != b_part)
            return a_part < b_part ? -1 : 1;
    }
    return 0;
}

bool is_newer_version(StringView candidate, StringView current)
{
    auto candidate_version = parse_version(candidate);
    auto current_version = parse_version(current);
    if (!candidate_version.has_value() || !current_version.has_value())
        return false;
    return compare_versions(*candidate_version, *current_version) > 0;
}

#endif

// https://www.freedesktop.org/software/systemd/man/latest/os-release.html
OSRelease parse_os_release(StringView contents)
{
    OSRelease result;
    for (auto line : contents.lines()) {
        line = line.trim_whitespace();
        if (line.is_empty() || line.starts_with('#'))
            continue;

        auto equals = line.find('=');
        if (!equals.has_value())
            continue;
        auto key = line.substring_view(0, *equals);
        auto value = line.substring_view(*equals + 1);
        if (value.length() >= 2 && (value[0] == '"' || value[0] == '\'') && value[value.length() - 1] == value[0])
            value = value.substring_view(1, value.length() - 2);

        auto string = String::from_utf8(value);
        if (string.is_error())
            continue;
        if (key == "ID"sv)
            result.id = string.release_value();
        else if (key == "VERSION_ID"sv)
            result.version_id = string.release_value();
    }
    return result;
}

Optional<InstallMethod> parse_package_format(StringView contents)
{
    auto format = contents.trim_whitespace();
    if (format == "DEB"sv)
        return InstallMethod::DEB;
    if (format == "RPM"sv)
        return InstallMethod::RPM;
    if (format == "AppImage"sv)
        return InstallMethod::AppImage;
    return {};
}

static InstallMethod current_install_method()
{
#if defined(AK_OS_MACOS)
    return InstallMethod::MacOSApp;
#else
    if (FileSystem::exists("/.flatpak-info"sv))
        return InstallMethod::Flatpak;

    // The Linux packages say which kind they are in a file of their own, which an archive of the same build lacks.
    auto marker = LexicalPath::join(s_ladybird_resource_root, "package-format"sv).string();
    if (auto file = Core::File::open(marker, Core::File::OpenMode::Read); !file.is_error()) {
        if (auto contents = file.value()->read_until_eof(); !contents.is_error()) {
            if (auto install_method = parse_package_format(contents.value()); install_method.has_value())
                return *install_method;
        }
    }
    return InstallMethod::Source;
#endif
}

static OSRelease current_os_release()
{
    for (auto path : { "/etc/os-release"sv, "/usr/lib/os-release"sv }) {
        auto file = Core::File::open(path, Core::File::OpenMode::Read);
        if (file.is_error())
            continue;
        auto contents = file.value()->read_until_eof();
        if (!contents.is_error())
            return parse_os_release(contents.value());
    }
    return {};
}

static Optional<String> current_macos_version()
{
#if defined(AK_OS_MACOS)
    char version[32] {};
    size_t size = sizeof(version);
    if (sysctlbyname("kern.osproductversion", version, &size, nullptr, 0) != 0)
        return {};
    auto string = String::from_utf8(StringView { version, strnlen(version, sizeof(version)) });
    if (string.is_error())
        return {};
    return string.release_value();
#else
    return {};
#endif
}

#if defined(AK_OS_WINDOWS) && defined(LADYBIRD_HAS_UPDATE_CHECK)
// GetVersionEx reports an older version of Windows to an application that does not declare in its manifest that it
// supports the newer one. RtlGetVersion always reports the version that runs.
static Optional<String> current_windows_version()
{
    using RtlGetVersionFunction = NTSTATUS(WINAPI*)(PRTL_OSVERSIONINFOW);

    auto* ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll)
        return {};
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wcast-function-type-mismatch"
    auto rtl_get_version = reinterpret_cast<RtlGetVersionFunction>(GetProcAddress(ntdll, "RtlGetVersion"));
#    pragma clang diagnostic pop
    if (!rtl_get_version)
        return {};

    RTL_OSVERSIONINFOW info {};
    info.dwOSVersionInfoSize = sizeof(info);
    if (rtl_get_version(&info) != STATUS_SUCCESS)
        return {};
    return MUST(String::formatted("{}.{}.{}", info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber));
}
#endif

SystemDescription describe_current_system()
{
    SystemDescription system;
    system.install_method = current_install_method();
#if ARCH(X86_64)
    system.architecture = Architecture::X86_64;
#elif ARCH(AARCH64)
    system.architecture = Architecture::AArch64;
#endif
    if (system.install_method != InstallMethod::MacOSApp && system.install_method != InstallMethod::Flatpak)
        system.os_release = current_os_release();
    system.macos_version = current_macos_version();
    return system;
}

#if defined(LADYBIRD_HAS_UPDATE_CHECK)

// releases.json names architectures the way that each kind of package does.
static Optional<StringView> architecture_name(InstallMethod install_method, Architecture architecture)
{
    if (architecture == Architecture::Other)
        return {};

    switch (install_method) {
    case InstallMethod::MacOSApp:
        return architecture == Architecture::AArch64 ? "arm64"sv : "x86_64"sv;
    case InstallMethod::DEB:
        return architecture == Architecture::AArch64 ? "arm64"sv : "amd64"sv;
    case InstallMethod::RPM:
    case InstallMethod::Flatpak:
    case InstallMethod::AppImage:
        return architecture == Architecture::AArch64 ? "aarch64"sv : "x86_64"sv;
    case InstallMethod::Source:
        return {};
    }
    VERIFY_NOT_REACHED();
}

static bool contains_string(Optional<JsonArray const&> array, StringView string)
{
    return array.has_value() && any_of(array->values(), [&](JsonValue const& value) {
        return value.is_string() && value.as_string() == string;
    });
}

static bool repository_installs_on(Optional<JsonObject const&> repository, SystemDescription const& system, StringView architecture)
{
    if (!repository.has_value() || !system.os_release.id.has_value() || !system.os_release.version_id.has_value())
        return false;

    auto systems = repository->get_array("systems"sv);
    return systems.has_value() && any_of(systems->values(), [&](JsonValue const& value) {
        if (!value.is_object())
            return false;
        auto const& entry = value.as_object();
        auto id = entry.get_string("id"sv);
        auto version_id = entry.get_string("version_id"sv);
        return id.has_value() && *id == *system.os_release.id && version_id.has_value() && *version_id == *system.os_release.version_id
            && contains_string(entry.get_array("architectures"sv), architecture);
    });
}

static bool runs_on_macos_version(JsonObject const& download, Optional<String> const& macos_version)
{
    auto minimum = download.get_string("minimum_macos_version"sv);
    if (!minimum.has_value() || !macos_version.has_value())
        return true;

    auto minimum_version = parse_version(*minimum);
    auto current_version = parse_version(*macos_version);
    if (!minimum_version.has_value() || !current_version.has_value())
        return true;
    return compare_versions(*current_version, *minimum_version) >= 0;
}

static bool release_installs_on(JsonObject const& release, SystemDescription const& system)
{
    auto architecture = architecture_name(system.install_method, system.architecture);
    auto downloads = release.get_array("downloads"sv);
    auto repositories = release.get_object("repositories"sv);

    auto has_download = [&](auto matches) {
        return downloads.has_value() && any_of(downloads->values(), [&](JsonValue const& value) {
            return value.is_object() && matches(value.as_object());
        });
    };

    switch (system.install_method) {
    case InstallMethod::MacOSApp:
        return architecture.has_value() && has_download([&](JsonObject const& download) {
            return download.get_string("platform"sv) == "macOS"sv && download.get_string("architecture"sv) == *architecture
                && runs_on_macos_version(download, system.macos_version);
        });
    case InstallMethod::DEB:
        return architecture.has_value() && repositories.has_value()
            && repository_installs_on(repositories->get_object("apt"sv), system, *architecture);
    case InstallMethod::RPM:
        return architecture.has_value() && repositories.has_value()
            && repository_installs_on(repositories->get_object("rpm"sv), system, *architecture);
    case InstallMethod::Flatpak: {
        if (!architecture.has_value() || !repositories.has_value())
            return false;
        auto flatpak = repositories->get_object("flatpak"sv);
        return flatpak.has_value() && contains_string(flatpak->get_array("architectures"sv), *architecture);
    }
    case InstallMethod::AppImage:
        return architecture.has_value() && has_download([&](JsonObject const& download) {
            auto name = download.get_string("name"sv);
            return download.get_string("platform"sv) == "Linux"sv && download.get_string("architecture"sv) == *architecture
                && name.has_value() && name->bytes_as_string_view().ends_with(".AppImage"sv);
        });
    case InstallMethod::Source:
        return has_download([](JsonObject const& download) { return download.get_string("platform"sv) == "source"sv; });
    }
    VERIFY_NOT_REACHED();
}

ErrorOr<Optional<String>> newest_release_for_system(StringView releases_json, SystemDescription const& system, Optional<UnixDateTime> published_before)
{
    auto json = TRY(JsonValue::from_string(releases_json));
    if (!json.is_object())
        return Error::from_string_literal("releases.json is not a JSON object");
    auto releases = json.as_object().get_array("releases"sv);
    if (!releases.has_value())
        return Error::from_string_literal("releases.json lists no releases");

    Optional<String> newest;
    Vector<u64> newest_version;
    for (auto const& value : releases->values()) {
        if (!value.is_object())
            continue;
        auto const& release = value.as_object();
        auto version = release.get_string("version"sv);
        if (!version.has_value())
            continue;
        auto parsed_version = parse_version(*version);
        if (!parsed_version.has_value() || !release_installs_on(release, system))
            continue;
        if (published_before.has_value()) {
            auto published = UnixDateTime::parse("%Y-%m-%dT%H:%M:%SZ"sv, release.get_string("published"sv).value_or({}), true);
            if (!published.has_value() || *published > *published_before)
                continue;
        }
        if (!newest.has_value() || compare_versions(*parsed_version, newest_version) > 0) {
            newest = *version;
            newest_version = parsed_version.release_value();
        }
    }
    return newest;
}

AK::Duration time_until_next_update_check(UnixDateTime last_check, UnixDateTime now, AK::Duration interval)
{
    auto elapsed = now - last_check;
    if (elapsed < AK::Duration::zero() || elapsed >= interval)
        return AK::Duration::zero();
    return interval - elapsed;
}

struct CountedPeriods {
    bool _new { false };
    bool day { false };
    bool week { false };
    bool month { false };
};

// Days, weeks and months begin at midnight UTC. Weeks start on Monday, like those of ISO 8601, and the Unix epoch fell
// on a Thursday.
static CountedPeriods periods_begun_since(UnixDateTime earlier, UnixDateTime now)
{
    auto days = [](UnixDateTime time) { return time.seconds_since_epoch() / (24 * 60 * 60); };
    auto weeks = [&](UnixDateTime time) { return (days(time) + 3) / 7; };
    auto month = [](UnixDateTime time) { return time.to_byte_string("%Y-%m"sv, UnixDateTime::LocalTime::No); };
    return { .day = days(now) > days(earlier), .week = weeks(now) > weeks(earlier), .month = month(now) > month(earlier) };
}

static StringView install_method_name(InstallMethod install_method)
{
    switch (install_method) {
    case InstallMethod::MacOSApp:
        return "app"sv;
    case InstallMethod::DEB:
        return "deb"sv;
    case InstallMethod::RPM:
        return "rpm"sv;
    case InstallMethod::Flatpak:
        return "flatpak"sv;
    case InstallMethod::AppImage:
        return "appimage"sv;
    case InstallMethod::Source:
        return "source"sv;
    }
    VERIFY_NOT_REACHED();
}

static void append_member(StringBuilder& builder, StringView key, StringView value)
{
    if (!builder.is_empty())
        builder.append(", "sv);
    builder.append(key);
    if (!value.is_empty())
        builder.appendff("={}", value);
}

static void append_string_member(StringBuilder& builder, StringView key, StringView value)
{
    if (value.is_empty() || !all_of(value, [](char c) { return c >= 0x20 && c <= 0x7e; }))
        return;

    StringBuilder string;
    string.append('"');
    for (auto c : value) {
        if (c == '"' || c == '\\')
            string.append('\\');
        string.append(c);
    }
    string.append('"');
    append_member(builder, key, string.string_view());
}

// The header is a dictionary of structured field values (RFC 9651), which the server parses like any other. It tells
// which kind of installation checks, and nothing that tells one installation apart from another of the same kind.
static ByteString update_check_header(SystemDescription const& system, StringView browser_version, CountedPeriods counted)
{
    StringBuilder builder;
    append_string_member(builder, "version"sv, browser_version);

#    if defined(AK_OS_MACOS)
    append_member(builder, "os"sv, "macos"sv);
    if (system.macos_version.has_value())
        append_string_member(builder, "os-version"sv, system.macos_version->bytes_as_string_view().find_first_split_view('.'));
#    elif defined(AK_OS_LINUX)
    append_member(builder, "os"sv, "linux"sv);
    if (system.os_release.version_id.has_value())
        append_string_member(builder, "os-version"sv, *system.os_release.version_id);
    if (system.os_release.id.has_value())
        append_string_member(builder, "distro"sv, *system.os_release.id);
#    elif defined(AK_OS_WINDOWS)
    append_member(builder, "os"sv, "windows"sv);
    if (auto windows_version = current_windows_version(); windows_version.has_value())
        append_string_member(builder, "os-version"sv, *windows_version);
#    endif

    if (system.architecture == Architecture::X86_64)
        append_member(builder, "arch"sv, "x86_64"sv);
    else if (system.architecture == Architecture::AArch64)
        append_member(builder, "arch"sv, "aarch64"sv);

    append_member(builder, "pkg"sv, install_method_name(system.install_method));
    if (counted._new)
        append_member(builder, "new"sv, {});
    if (counted.day)
        append_member(builder, "daily"sv, {});
    if (counted.week)
        append_member(builder, "weekly"sv, {});
    if (counted.month)
        append_member(builder, "monthly"sv, {});
    return builder.to_byte_string();
}

String release_page_url(StringView version)
{
    return MUST(String::formatted("https://github.com/LadybirdBrowser/ladybird/releases/tag/{}", version));
}

static AK::Duration update_check_interval()
{
    auto minutes = Application::settings().config_variable_as_integer<u32>(ConfigVariableID::UpdateCheckInterval);
    return AK::Duration::from_seconds(static_cast<i64>(minutes) * 60);
}

UpdateChecker::UpdateChecker(ByteString state_path)
    : m_state_path(move(state_path))
{
}

UpdateChecker::~UpdateChecker()
{
    if (auto request = move(m_request))
        request->stop();
}

UpdateChecker::State UpdateChecker::read_state() const
{
    auto json = read_json_file(m_state_path);
    if (json.is_error() || json.value().get_integer<i64>(VERSION_KEY) != STATE_VERSION)
        return {};

    State state;
    if (auto first_start = json.value().get_integer<i64>(FIRST_START_KEY); first_start.has_value())
        state.first_start = UnixDateTime::from_milliseconds_since_epoch(*first_start);
    if (auto last_check = json.value().get_integer<i64>(LAST_CHECK_KEY); last_check.has_value())
        state.last_check = UnixDateTime::from_milliseconds_since_epoch(*last_check);
    if (auto skipped_version = json.value().get_string(SKIPPED_VERSION_KEY); skipped_version.has_value())
        state.skipped_version = *skipped_version;
    return state;
}

void UpdateChecker::write_state(State const& state) const
{
    JsonObject json;
    json.set(VERSION_KEY, STATE_VERSION);
    if (state.first_start.has_value())
        json.set(FIRST_START_KEY, state.first_start->milliseconds_since_epoch());
    if (state.last_check.has_value())
        json.set(LAST_CHECK_KEY, state.last_check->milliseconds_since_epoch());
    if (state.skipped_version.has_value())
        json.set(SKIPPED_VERSION_KEY, *state.skipped_version);
    if (auto result = write_json_file(m_state_path, json); result.is_error())
        warnln("Unable to save the state of update checks: {}", result.error());
}

// The last check is kept here as well, so that one that could not be saved still counts. Once a check took place here,
// a saved check that lies ahead of now comes from a clock that was turned back, and a state file that cannot be written
// would otherwise keep making every check due at once.
Optional<UnixDateTime> UpdateChecker::last_check() const
{
    auto saved = read_state().last_check;
    if (!m_last_check.has_value())
        return saved;
    if (!saved.has_value() || *saved > UnixDateTime::now())
        return m_last_check;
    return max(*saved, *m_last_check);
}

// A first start waits for the interval as a check does, so that nothing is fetched before the user could turn checks off.
// Once a check took place here, it alone counts, as the state file may still hold a first start that it could not drop.
UnixDateTime UpdateChecker::next_check_waits_from() const
{
    auto epoch = UnixDateTime::epoch();
    if (m_last_check.has_value())
        return last_check().value_or(epoch);
    auto first_start = max(read_state().first_start.value_or(epoch), m_first_start.value_or(epoch));
    return max(last_check().value_or(epoch), first_start);
}

void UpdateChecker::start()
{
    if (!BROWSER_IS_RELEASE_BUILD)
        return;
    m_started = true;

    if (auto state = read_state(); !state.first_start.has_value() && !state.last_check.has_value()) {
        state.first_start = m_first_start = UnixDateTime::now();
        write_state(state);
        schedule();
        return;
    }

    // Every start after the first checks right away, so that a security release is noticed as soon as Ladybird starts
    // instead of up to an interval later.
    auto previous_check = last_check();
    auto recently_checked = previous_check.has_value()
        && time_until_next_update_check(*previous_check, UnixDateTime::now(), min(update_check_interval(), shortest_wait_before_check_at_start)) > AK::Duration::zero();
    if (!recently_checked && Application::settings().automatic_update_checks_allowed())
        check(UpdateCheckTrigger::Automatic);
    else
        schedule();
}

void UpdateChecker::settings_changed()
{
    if (m_started)
        schedule();
}

void UpdateChecker::schedule()
{
    if (m_timer)
        m_timer->stop();
    if (!m_started || !Application::settings().automatic_update_checks_allowed())
        return;

    auto wait = min(time_until_next_update_check(next_check_waits_from(), UnixDateTime::now(), update_check_interval()), longest_timer_wait);

    m_timer = Core::Timer::create_single_shot(wait, [weak_this = make_weak_ptr()] {
        if (weak_this)
            weak_this->check_if_due();
    });
    m_timer->start();
}

void UpdateChecker::check_if_due()
{
    if (!m_started || !Application::settings().automatic_update_checks_allowed())
        return;

    // Another instance of Ladybird may have checked meanwhile.
    if (time_until_next_update_check(next_check_waits_from(), UnixDateTime::now(), update_check_interval()) > AK::Duration::zero()) {
        schedule();
        return;
    }
    check(UpdateCheckTrigger::Automatic);
}

void UpdateChecker::check(UpdateCheckTrigger trigger)
{
    if (trigger == UpdateCheckTrigger::UserInitiated && !BROWSER_IS_RELEASE_BUILD) {
        report({ .kind = UpdateCheckResult::Kind::DevelopmentBuild, .update = {} }, trigger);
        return;
    }

    if (!m_waiting_triggers.contains_slow(trigger))
        m_waiting_triggers.append(trigger);
    if (m_request)
        return;

    m_check_started = UnixDateTime::now();
    auto client = Application::request_server_client(IsPrivate::No);
    if (client.is_error()) {
        finish(client.release_error());
        return;
    }

    // The first check of each day, week and month counts this installation once for it, and the very first check counts
    // it for all three and marks the installation as new.
    CountedPeriods counted;
    if (BROWSER_IS_RELEASE_BUILD) {
        if (auto previous_check = last_check(); previous_check.has_value())
            counted = periods_begun_since(*previous_check, m_check_started);
        else
            counted = { ._new = true, .day = true, .week = true, .month = true };
    }

    auto headers = HTTP::HeaderList::create();
    headers->set({ "User-Agent"sv, Web::default_user_agent });
    headers->set({ "Ladybird-Update-Check"sv, update_check_header(describe_current_system(), BROWSER_VERSION ""sv, counted) });
    m_request = client.value()->start_request("GET"sv, URL::Parser::basic_parse(RELEASES_FEED_URL).release_value(), {}, *headers, {},
        HTTP::CacheMode::NoStore, HTTP::Cookie::IncludeCredentials::No);
    if (!m_request) {
        finish(Error::from_string_literal("Unable to start the request for releases.json"));
        return;
    }

    m_timeout = Core::Timer::create_single_shot(request_timeout, [weak_this = make_weak_ptr()] {
        if (weak_this && weak_this->m_request)
            weak_this->stop_request(Error::from_string_literal("The request for releases.json timed out"));
    });
    m_timeout->start();

    // The body is taken as it arrives, so that one larger than any releases.json stops the request before it grows any
    // further.
    m_payload.clear();
    m_request->set_unbuffered_request_callbacks(
        [weak_this = make_weak_ptr()](NonnullRefPtr<HTTP::HeaderList>, Optional<u32> response_code, Optional<String> const&, Requests::CacheState) {
            if (!weak_this || !weak_this->m_request)
                return;
            if (!response_code.has_value() || *response_code < 200 || *response_code >= 300) {
                warnln("Unable to check for updates: received HTTP status {}", response_code.value_or(0));
                weak_this->stop_request(Error::from_string_literal("The server did not send releases.json"));
            }
        },
        [weak_this = make_weak_ptr()](Requests::ResponseData data) {
            if (weak_this)
                weak_this->receive(data.bytes());
        },
        [weak_this = make_weak_ptr()](Core::ImmutableBytes data) {
            if (weak_this)
                weak_this->receive(data.bytes());
        },
        [weak_this = make_weak_ptr()](u64, Requests::RequestTimingInfo const&, Optional<Requests::NetworkError> network_error) {
            if (!weak_this || !weak_this->m_request)
                return;

            // A request cannot be destroyed from inside its own callback.
            Core::deferred_invoke([request = move(weak_this->m_request)] { });

            auto payload = move(weak_this->m_payload);
            if (network_error.has_value()) {
                warnln("Unable to check for updates: {}", Requests::network_error_to_string(*network_error));
                weak_this->finish(Error::from_string_literal("The request for releases.json failed"));
            } else {
                weak_this->finish(StringView { payload.bytes() });
            }
        });
}

void UpdateChecker::receive(ReadonlyBytes data)
{
    if (!m_request)
        return;
    if (data.size() > maximum_feed_size - m_payload.size()) {
        stop_request(Error::from_string_literal("releases.json is too large"));
        return;
    }
    if (auto result = m_payload.try_append(data); result.is_error())
        stop_request(result.release_error());
}

void UpdateChecker::stop_request(Error error)
{
    if (auto request = move(m_request)) {
        request->stop();

        // A request cannot be destroyed from inside its own callback.
        Core::deferred_invoke([request = move(request)] { });
    }
    m_payload.clear();
    finish(move(error));
}

void UpdateChecker::finish(ErrorOr<StringView> payload)
{
    if (m_timeout)
        m_timeout->stop();
    m_timeout = nullptr;
    auto triggers = move(m_waiting_triggers);

    auto system = describe_current_system();
    auto is_package = first_is_one_of(system.install_method, InstallMethod::DEB, InstallMethod::RPM, InstallMethod::Flatpak);
    Optional<String> newest;
    Optional<String> overdue;
    auto succeeded = false;
    if (payload.is_error()) {
        warnln("Unable to check for updates: {}", payload.error());
    } else if (auto release = newest_release_for_system(payload.value(), system); release.is_error()) {
        warnln("Unable to check for updates: {}", release.error());
    } else {
        newest = release.release_value();
        overdue = is_package ? MUST(newest_release_for_system(payload.value(), system, UnixDateTime::now() - package_update_grace_period)) : newest;
        succeeded = true;
    }

    // A check that failed is tried again once the interval has passed, like one that succeeded. A check took place when
    // it started, as that is when it counted the installation. Later checks wait from it alone, as a first start that the
    // clock was turned back past would otherwise make every check due at once.
    if (BROWSER_IS_RELEASE_BUILD) {
        auto state = read_state();
        state.first_start = m_first_start = {};
        state.last_check = m_last_check = m_check_started;
        write_state(state);
    }

    // An automatic check of a package reminds the user once a release has gone uninstalled for the grace period, and then
    // names the newest release, as that is the one that the system's updater installs.
    for (auto trigger : triggers) {
        UpdateCheckResult result;
        auto outdated_by = trigger == UpdateCheckTrigger::Automatic ? overdue : newest;
        if (!succeeded)
            result.kind = UpdateCheckResult::Kind::Failed;
        else if (outdated_by.has_value() && (trigger == UpdateCheckTrigger::Debug || is_newer_version(*outdated_by, BROWSER_VERSION ""sv)))
            result = { .kind = UpdateCheckResult::Kind::UpdateAvailable, .update = AvailableUpdate { *newest, system.install_method } };
        else
            result.kind = UpdateCheckResult::Kind::UpToDate;
        report(result, trigger);
    }
    schedule();
}

void UpdateChecker::report(UpdateCheckResult const& result, UpdateCheckTrigger trigger)
{
    if (trigger == UpdateCheckTrigger::Automatic) {
        if (result.kind != UpdateCheckResult::Kind::UpdateAvailable)
            return;
        if (read_state().skipped_version == result.update->version)
            return;
    }
    if (on_result)
        on_result(result, trigger);
}

void UpdateChecker::skip_version(String const& version)
{
    auto state = read_state();
    state.skipped_version = version;
    write_state(state);
}

#endif

}

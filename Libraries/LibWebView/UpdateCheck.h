/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <AK/Weakable.h>
#include <LibCore/Forward.h>
#include <LibRequests/Forward.h>
#include <LibWebView/Export.h>

namespace WebView {

enum class InstallMethod : u8 {
    MacOSApp,
    DEB,
    RPM,
    Flatpak,
    Source,
};

enum class Architecture : u8 {
    X86_64,
    AArch64,
    Other,
};

struct OSRelease {
    Optional<String> id;
    Optional<String> version_id;
};

struct SystemDescription {
    InstallMethod install_method { InstallMethod::Source };
    Architecture architecture { Architecture::Other };
    OSRelease os_release;
    Optional<String> macos_version;
};

struct AvailableUpdate {
    String version;
    InstallMethod install_method { InstallMethod::Source };
};

enum class UpdateCheckTrigger : u8 {
    Automatic,
    UserInitiated,
    Debug,
};

struct UpdateCheckResult {
    enum class Kind : u8 {
        UpdateAvailable,
        UpToDate,
        DevelopmentBuild,
        Failed,
    };

    Kind kind { Kind::Failed };
    Optional<AvailableUpdate> update;
};

WEBVIEW_API OSRelease parse_os_release(StringView);
WEBVIEW_API Optional<InstallMethod> parse_package_format(StringView);
WEBVIEW_API SystemDescription describe_current_system();

#if defined(LADYBIRD_HAS_UPDATE_CHECK)

constexpr StringView RELEASES_FEED_URL = "https://download.ladybird.org/releases.json"sv;

WEBVIEW_API Optional<Vector<u64>> parse_version(StringView);
WEBVIEW_API bool is_newer_version(StringView candidate, StringView current);
WEBVIEW_API ErrorOr<Optional<String>> newest_release_for_system(StringView releases_json, SystemDescription const&, Optional<UnixDateTime> published_before = {});
WEBVIEW_API AK::Duration time_until_next_update_check(UnixDateTime last_check, UnixDateTime now, AK::Duration interval);
WEBVIEW_API String release_page_url(StringView version);

class WEBVIEW_API UpdateChecker : public Weakable<UpdateChecker> {
    AK_MAKE_NONCOPYABLE(UpdateChecker);
    AK_MAKE_NONMOVABLE(UpdateChecker);

public:
    AK_ALLOC_WITH_KMALLOC;

    explicit UpdateChecker(ByteString state_path);
    ~UpdateChecker();

    void start();
    void settings_changed();
    void check(UpdateCheckTrigger);
    void skip_version(String const&);

    Function<void(UpdateCheckResult const&, UpdateCheckTrigger)> on_result;

private:
    struct State {
        Optional<UnixDateTime> first_start;
        Optional<UnixDateTime> last_check;
        Optional<String> skipped_version;
    };

    State read_state() const;
    void write_state(State const&) const;
    Optional<UnixDateTime> last_check() const;
    UnixDateTime next_check_waits_from() const;

    void schedule();
    void check_if_due();
    void receive(ReadonlyBytes);
    void stop_request(Error);
    void finish(ErrorOr<StringView> payload);
    void report(UpdateCheckResult const&, UpdateCheckTrigger);

    ByteString m_state_path;
    bool m_started { false };
    Optional<UnixDateTime> m_first_start;
    Optional<UnixDateTime> m_last_check;
    UnixDateTime m_check_started;

    RefPtr<Core::Timer> m_timer;
    RefPtr<Core::Timer> m_timeout;
    RefPtr<Requests::Request> m_request;
    ByteBuffer m_payload;
    Vector<UpdateCheckTrigger> m_waiting_triggers;
};

#endif

}

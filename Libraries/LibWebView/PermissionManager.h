/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <AK/kmalloc.h>
#include <LibURL/Origin.h>
#include <LibWebCommon/Permissions/Permissions.h>
#include <LibWebView/Export.h>

namespace WebView {

class PermissionManager;

// A decision the user made for a site. AllowOnce lasts only while the site is in use; the others last until the user
// changes them.
enum class PermissionDecision : u8 {
    Allow,
    AllowOnce,
    Block,
};

struct PermissionTimeouts {
    // A one-time grant ends once no tab has shown the site for this long.
    AK::Duration background { AK::Duration::from_seconds(5ll * 60) };
    // A one-time grant never lasts longer than this.
    AK::Duration one_time_maximum { AK::Duration::from_seconds(16ll * 60 * 60) };
    // How long a site may not ask again after the user has turned its prompts away too many times.
    AK::Duration embargo { AK::Duration::from_seconds(7ll * 24 * 60 * 60) };
};

struct SiteDecision {
    URL::Origin origin;
    PermissionDecision decision;
};

class WEBVIEW_API PermissionManagerObserver {
public:
    virtual ~PermissionManagerObserver() = default;
    virtual void permissions_changed(PermissionManager const&) = 0;
};

// Every permission decision of one browsing session, keyed by the top-level origin it was made for. The ordinary
// session's decisions persist in a file; a private session's live in memory only, and it also honors every block the
// ordinary session holds.
class WEBVIEW_API PermissionManager final
    : public PermissionManagerObserver
    , public Weakable<PermissionManager> {
    AK_MAKE_NONCOPYABLE(PermissionManager);
    AK_MAKE_NONMOVABLE(PermissionManager);

public:
    AK_ALLOC_WITH_KMALLOC;

    static NonnullOwnPtr<PermissionManager> create_for_ordinary_session(ByteString persistence_path);
    static NonnullOwnPtr<PermissionManager> create_for_private_session(PermissionManager& ordinary_session);

    virtual ~PermissionManager() override;

    Web::Permissions::EffectiveState effective_state(Web::Permissions::PermissionName, URL::Origin const& top_level_origin) const;
    Optional<PermissionDecision> decision(Web::Permissions::PermissionName, URL::Origin const& top_level_origin) const;

    // Moves with every decision made for a site, however it was made, so an answer given to a prompt can tell if
    // anything was decided while the prompt was up.
    u64 decision_generation(Web::Permissions::PermissionName, URL::Origin const& top_level_origin) const;

    // An empty decision returns the site to asking.
    void set_decision(Web::Permissions::PermissionName, URL::Origin const& top_level_origin, Optional<PermissionDecision>);
    void reset_origin(URL::Origin const& top_level_origin);
    void record_prompt_outcome(Web::Permissions::PermissionName, URL::Origin const& top_level_origin, Web::Permissions::PromptOutcome);

    // Each view reports the origin of the top-level document it shows, and if it's visible; one-time grants end by these.
    void set_view_presence(u64 view_id, URL::Origin const& top_level_origin, bool visible);
    void remove_view(u64 view_id);

    // What a site with no decision of its own gets: Prompt, or Denied when the user has turned the feature off.
    void set_global_default(Web::Permissions::PermissionName, Web::Permissions::EffectiveState);
    void set_os_denies_geolocation(bool);

    Web::Permissions::PermissionSnapshot snapshot() const;
    // Only the given sites' states, for a process that shows those sites and no others.
    Web::Permissions::PermissionSnapshot snapshot_for(Vector<URL::Origin> const& top_level_origins) const;
    Vector<SiteDecision> site_decisions(Web::Permissions::PermissionName) const;

    // The earliest time at which run_expiry() has something to expire.
    Optional<UnixDateTime> next_deadline() const;
    Function<void()> on_deadlines_changed;
    void run_expiry();

    void register_observer(PermissionManagerObserver&);
    void unregister_observer(PermissionManagerObserver&);

    // Makes the given time pass for one origin's entries only, so concurrently running tests don't disturb each other.
    void age_origin_for_testing(URL::Origin const&, AK::Duration);
    void set_clock_for_testing(Function<UnixDateTime()>);
    void set_timeouts_for_testing(PermissionTimeouts);

private:
    struct Entry {
        Web::Permissions::PermissionName name;
        URL::Origin origin;
        Optional<PermissionDecision> decision;
        UnixDateTime decided_at;
        Optional<UnixDateTime> hidden_since;
        u8 dismissals { 0 };
        u8 ignores { 0 };
        Optional<UnixDateTime> embargo_until;

        bool is_empty() const { return !decision.has_value() && dismissals == 0 && ignores == 0 && !embargo_until.has_value(); }
    };

    struct ViewPresence {
        URL::Origin origin;
        bool visible { false };
    };

    enum class OriginPresence : u8 {
        Absent,
        Hidden,
        Visible,
    };

    PermissionManager(Optional<ByteString> persistence_path, WeakPtr<PermissionManager> ordinary_session);

    virtual void permissions_changed(PermissionManager const&) override;

    UnixDateTime now() const;
    bool embargo_is_live(Entry const&) const;
    Web::Permissions::EffectiveState global_default(Web::Permissions::PermissionName) const;
    bool os_denies_geolocation() const;
    bool ordinary_session_blocks(Web::Permissions::PermissionName, URL::Origin const&) const;

    Entry const* find_entry(Web::Permissions::PermissionName, URL::Origin const&) const;
    Entry& ensure_entry(Web::Permissions::PermissionName, URL::Origin const&);
    void remove_empty_entries();

    OriginPresence presence_of(URL::Origin const&) const;
    bool apply_presence();

    void did_change();
    void bump_generation(Web::Permissions::PermissionName, URL::Origin const&);
    void load();
    void persist() const;

    Optional<ByteString> m_persistence_path;
    // The ordinary session's manager, which a private session's reads its inherited blocks and defaults from. Weak,
    // since the two sessions are torn down in no set order.
    WeakPtr<PermissionManager> m_ordinary_session;

    Vector<Entry> m_entries;
    HashMap<u64, ViewPresence> m_views;
    HashMap<String, u64> m_generations;

    Web::Permissions::EffectiveState m_geolocation_default { Web::Permissions::EffectiveState::Prompt };
    Web::Permissions::EffectiveState m_microphone_default { Web::Permissions::EffectiveState::Prompt };
    bool m_os_denies_geolocation { false };

    u64 m_revision { 0 };
    Vector<PermissionManagerObserver*> m_observers;

    Function<UnixDateTime()> m_clock;
    PermissionTimeouts m_timeouts;
};

}

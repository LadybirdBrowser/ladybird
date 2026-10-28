/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibURL/Parser.h>
#include <LibWebView/PermissionManager.h>
#include <LibWebView/Utilities.h>

namespace WebView {

using Web::Permissions::EffectiveState;
using Web::Permissions::PermissionName;
using Web::Permissions::PromptOutcome;

static constexpr u8 DISMISSALS_BEFORE_EMBARGO = 3;
static constexpr u8 IGNORES_BEFORE_EMBARGO = 4;
static constexpr i64 PERSISTENCE_VERSION = 1;

static StringView decision_to_string(PermissionDecision decision)
{
    switch (decision) {
    case PermissionDecision::Allow:
        return "allow"sv;
    case PermissionDecision::AllowOnce:
        return "allow-once"sv;
    case PermissionDecision::Block:
        return "block"sv;
    }
    VERIFY_NOT_REACHED();
}

static Optional<PermissionDecision> persisted_decision_from_string(StringView decision)
{
    if (decision == "allow"sv)
        return PermissionDecision::Allow;
    if (decision == "block"sv)
        return PermissionDecision::Block;
    return {};
}

static Optional<URL::Origin> origin_from_string(StringView serialized_origin)
{
    auto url = URL::Parser::basic_parse(serialized_origin);
    if (!url.has_value())
        return {};
    auto origin = url->origin();
    if (origin.is_opaque())
        return {};
    return origin;
}

NonnullOwnPtr<PermissionManager> PermissionManager::create_for_ordinary_session(ByteString persistence_path)
{
    auto manager = adopt_own(*new PermissionManager(move(persistence_path), {}));
    manager->load();
    return manager;
}

NonnullOwnPtr<PermissionManager> PermissionManager::create_for_private_session(PermissionManager& ordinary_session)
{
    VERIFY(!ordinary_session.m_ordinary_session);
    auto manager = adopt_own(*new PermissionManager({}, ordinary_session.make_weak_ptr<PermissionManager>()));
    ordinary_session.register_observer(*manager);
    return manager;
}

PermissionManager::PermissionManager(Optional<ByteString> persistence_path, WeakPtr<PermissionManager> ordinary_session)
    : m_persistence_path(move(persistence_path))
    , m_ordinary_session(move(ordinary_session))
{
}

PermissionManager::~PermissionManager()
{
    if (m_ordinary_session)
        m_ordinary_session->unregister_observer(*this);
}

void PermissionManager::permissions_changed(PermissionManager const&)
{
    // The ordinary session changed, and a block there may change what this private session reports.
    did_change();
}

UnixDateTime PermissionManager::now() const
{
    if (m_clock)
        return m_clock();
    if (m_ordinary_session)
        return m_ordinary_session->now();
    return UnixDateTime::now();
}

bool PermissionManager::embargo_is_live(Entry const& entry) const
{
    return entry.embargo_until.has_value() && now() < *entry.embargo_until;
}

EffectiveState PermissionManager::global_default(PermissionName name) const
{
    if (m_ordinary_session)
        return m_ordinary_session->global_default(name);
    switch (name) {
    case PermissionName::Geolocation:
        return m_geolocation_default;
    case PermissionName::Microphone:
        return m_microphone_default;
    }
    VERIFY_NOT_REACHED();
}

bool PermissionManager::os_denies_geolocation() const
{
    if (m_ordinary_session)
        return m_ordinary_session->os_denies_geolocation();
    return m_os_denies_geolocation;
}

bool PermissionManager::ordinary_session_blocks(PermissionName name, URL::Origin const& origin) const
{
    return m_ordinary_session && m_ordinary_session->decision(name, origin) == PermissionDecision::Block;
}

PermissionManager::Entry const* PermissionManager::find_entry(PermissionName name, URL::Origin const& origin) const
{
    for (auto const& entry : m_entries) {
        if (entry.name == name && entry.origin.is_same_origin(origin))
            return &entry;
    }
    return nullptr;
}

PermissionManager::Entry& PermissionManager::ensure_entry(PermissionName name, URL::Origin const& origin)
{
    if (auto const* entry = find_entry(name, origin))
        return const_cast<Entry&>(*entry);
    m_entries.append({ .name = name, .origin = origin, .decision = {}, .decided_at = now(), .hidden_since = {}, .dismissals = 0, .ignores = 0, .embargo_until = {} });
    return m_entries.last();
}

void PermissionManager::remove_empty_entries()
{
    m_entries.remove_all_matching([](auto const& entry) { return entry.is_empty(); });
}

EffectiveState PermissionManager::effective_state(PermissionName name, URL::Origin const& origin) const
{
    if (ordinary_session_blocks(name, origin))
        return EffectiveState::Denied;

    if (auto const* entry = find_entry(name, origin)) {
        if (entry->decision == PermissionDecision::Block)
            return EffectiveState::Denied;

        // AD-HOC: While the user's dismissals keep a site from asking, it reads as denied, though nothing is stored as
        //         a decision.
        if (embargo_is_live(*entry))
            return EffectiveState::Denied;

        if (entry->decision.has_value()) {
            // A feature the user turned off for every site, or the system denies the browser, stays off for a site
            // the user allowed too; the decision is kept for when it's back on.
            if (global_default(name) == EffectiveState::Denied)
                return EffectiveState::Denied;
            if (name == PermissionName::Geolocation && os_denies_geolocation())
                return EffectiveState::Denied;
            return EffectiveState::Granted;
        }
    }

    return global_default(name);
}

static String generation_key(PermissionName name, URL::Origin const& origin)
{
    return MUST(String::formatted("{} {}", Web::Permissions::to_string(name), origin.serialize()));
}

// A private session's generation moves with the ordinary session's for the same site too, so an answer given while the
// ordinary session blocked the site (and even unblocked it again) is stale.
u64 PermissionManager::decision_generation(PermissionName name, URL::Origin const& origin) const
{
    auto generation = m_generations.get(generation_key(name, origin)).value_or(0);
    if (m_ordinary_session)
        generation += m_ordinary_session->decision_generation(name, origin);
    return generation;
}

void PermissionManager::bump_generation(PermissionName name, URL::Origin const& origin)
{
    auto key = generation_key(name, origin);
    m_generations.set(key, m_generations.get(key).value_or(0) + 1);
}

// A private session reads the ordinary session's blocks as decisions of its own, since they bind it as well.
Optional<PermissionDecision> PermissionManager::decision(PermissionName name, URL::Origin const& origin) const
{
    if (ordinary_session_blocks(name, origin))
        return PermissionDecision::Block;
    if (auto const* entry = find_entry(name, origin))
        return entry->decision;
    return {};
}

void PermissionManager::set_decision(PermissionName name, URL::Origin const& origin, Optional<PermissionDecision> decision)
{
    if (origin.is_opaque())
        return;
    bump_generation(name, origin);

    auto const* existing = find_entry(name, origin);
    if (existing && existing->decision == decision && existing->dismissals == 0 && existing->ignores == 0 && !existing->embargo_until.has_value())
        return;
    if (!existing && !decision.has_value())
        return;

    auto& entry = ensure_entry(name, origin);
    entry.decision = decision;
    entry.decided_at = now();
    entry.hidden_since = {};
    entry.dismissals = 0;
    entry.ignores = 0;
    entry.embargo_until = {};
    if (decision == PermissionDecision::AllowOnce && presence_of(origin) == OriginPresence::Hidden)
        entry.hidden_since = now();

    remove_empty_entries();
    did_change();
}

void PermissionManager::reset_origin(URL::Origin const& origin)
{
    for (auto name : { PermissionName::Geolocation, PermissionName::Microphone })
        bump_generation(name, origin);

    auto removed = m_entries.remove_all_matching([&](auto const& entry) { return entry.origin.is_same_origin(origin); });
    if (removed)
        did_change();
}

void PermissionManager::record_prompt_outcome(PermissionName name, URL::Origin const& origin, PromptOutcome outcome)
{
    if (origin.is_opaque())
        return;

    switch (outcome) {
    case PromptOutcome::AllowAlways:
        set_decision(name, origin, PermissionDecision::Allow);
        return;
    case PromptOutcome::AllowOnce:
        set_decision(name, origin, PermissionDecision::AllowOnce);
        return;
    case PromptOutcome::Block:
        set_decision(name, origin, PermissionDecision::Block);
        return;
    case PromptOutcome::Cancelled:
        return;
    case PromptOutcome::Dismissed:
    case PromptOutcome::Ignored:
        break;
    }

    bump_generation(name, origin);

    auto& entry = ensure_entry(name, origin);
    if (outcome == PromptOutcome::Dismissed)
        ++entry.dismissals;
    else
        ++entry.ignores;

    if (entry.dismissals >= DISMISSALS_BEFORE_EMBARGO || entry.ignores >= IGNORES_BEFORE_EMBARGO) {
        entry.dismissals = 0;
        entry.ignores = 0;
        entry.embargo_until = now() + m_timeouts.embargo;
    }

    did_change();
}

PermissionManager::OriginPresence PermissionManager::presence_of(URL::Origin const& origin) const
{
    auto presence = OriginPresence::Absent;
    for (auto const& it : m_views) {
        if (!it.value.origin.is_same_origin(origin))
            continue;
        if (it.value.visible)
            return OriginPresence::Visible;
        presence = OriginPresence::Hidden;
    }
    return presence;
}

bool PermissionManager::apply_presence()
{
    auto changed = false;
    auto deadlines_moved = false;
    for (auto& entry : m_entries) {
        if (entry.decision != PermissionDecision::AllowOnce)
            continue;

        switch (presence_of(entry.origin)) {
        case OriginPresence::Absent:
            entry.decision = {};
            entry.hidden_since = {};
            changed = true;
            break;
        case OriginPresence::Hidden:
            if (!entry.hidden_since.has_value()) {
                entry.hidden_since = now();
                deadlines_moved = true;
            }
            break;
        case OriginPresence::Visible:
            if (entry.hidden_since.has_value()) {
                entry.hidden_since = {};
                deadlines_moved = true;
            }
            break;
        }
    }

    if (changed)
        remove_empty_entries();
    if (deadlines_moved && !changed && on_deadlines_changed)
        on_deadlines_changed();
    return changed;
}

void PermissionManager::set_view_presence(u64 view_id, URL::Origin const& origin, bool visible)
{
    m_views.set(view_id, { origin, visible });
    if (apply_presence())
        did_change();
}

void PermissionManager::remove_view(u64 view_id)
{
    if (!m_views.remove(view_id))
        return;
    if (apply_presence())
        did_change();
}

void PermissionManager::set_global_default(PermissionName name, EffectiveState state)
{
    VERIFY(!m_ordinary_session);
    auto& current = name == PermissionName::Geolocation ? m_geolocation_default : m_microphone_default;
    if (current == state)
        return;
    current = state;
    did_change();
}

void PermissionManager::set_os_denies_geolocation(bool denies)
{
    VERIFY(!m_ordinary_session);
    if (m_os_denies_geolocation == denies)
        return;
    m_os_denies_geolocation = denies;
    did_change();
}

Web::Permissions::PermissionSnapshot PermissionManager::snapshot() const
{
    Web::Permissions::PermissionSnapshot snapshot;
    snapshot.revision = m_revision;
    snapshot.geolocation_default = global_default(PermissionName::Geolocation);
    snapshot.microphone_default = global_default(PermissionName::Microphone);

    auto add_entry = [&](PermissionName name, URL::Origin const& origin) {
        auto state = effective_state(name, origin);
        if (state == global_default(name))
            return;
        for (auto const& existing : snapshot.entries) {
            if (existing.name == name && existing.top_level_origin.is_same_origin(origin))
                return;
        }
        snapshot.entries.append({ origin, name, state });
    };

    for (auto const& entry : m_entries)
        add_entry(entry.name, entry.origin);
    if (m_ordinary_session) {
        for (auto const& entry : m_ordinary_session->m_entries) {
            if (entry.decision == PermissionDecision::Block)
                add_entry(entry.name, entry.origin);
        }
    }

    return snapshot;
}

Web::Permissions::PermissionSnapshot PermissionManager::snapshot_for(Vector<URL::Origin> const& origins) const
{
    auto full = snapshot();
    full.entries.remove_all_matching([&](auto const& entry) {
        return !origins.contains_slow(entry.top_level_origin);
    });
    full.origins = origins;
    return full;
}

Vector<SiteDecision> PermissionManager::site_decisions(PermissionName name) const
{
    Vector<SiteDecision> decisions;
    for (auto const& entry : m_entries) {
        if (entry.name == name && entry.decision.has_value())
            decisions.append({ entry.origin, *entry.decision });
    }
    return decisions;
}

Optional<UnixDateTime> PermissionManager::next_deadline() const
{
    Optional<UnixDateTime> deadline;
    auto consider = [&](UnixDateTime candidate) {
        if (!deadline.has_value() || candidate < *deadline)
            deadline = candidate;
    };

    for (auto const& entry : m_entries) {
        if (entry.decision == PermissionDecision::AllowOnce) {
            consider(entry.decided_at + m_timeouts.one_time_maximum);
            if (entry.hidden_since.has_value())
                consider(*entry.hidden_since + m_timeouts.background);
        }
        if (entry.embargo_until.has_value())
            consider(*entry.embargo_until);
    }
    return deadline;
}

void PermissionManager::run_expiry()
{
    auto current_time = now();
    auto changed = false;

    for (auto& entry : m_entries) {
        if (entry.decision == PermissionDecision::AllowOnce) {
            auto lifetime_ended = current_time >= entry.decided_at + m_timeouts.one_time_maximum;
            auto hidden_too_long = entry.hidden_since.has_value() && current_time >= *entry.hidden_since + m_timeouts.background;
            if (lifetime_ended || hidden_too_long) {
                entry.decision = {};
                entry.hidden_since = {};
                changed = true;
            }
        }
        if (entry.embargo_until.has_value() && current_time >= *entry.embargo_until) {
            entry.embargo_until = {};
            changed = true;
        }
    }

    if (changed) {
        remove_empty_entries();
        did_change();
    }
}

void PermissionManager::register_observer(PermissionManagerObserver& observer)
{
    m_observers.append(&observer);
}

void PermissionManager::unregister_observer(PermissionManagerObserver& observer)
{
    m_observers.remove_first_matching([&](auto* candidate) { return candidate == &observer; });
}

void PermissionManager::age_origin_for_testing(URL::Origin const& origin, AK::Duration duration)
{
    for (auto& entry : m_entries) {
        if (!entry.origin.is_same_origin(origin))
            continue;
        entry.decided_at -= duration;
        if (entry.hidden_since.has_value())
            *entry.hidden_since -= duration;
        if (entry.embargo_until.has_value())
            *entry.embargo_until -= duration;
    }
    run_expiry();
}

void PermissionManager::set_clock_for_testing(Function<UnixDateTime()> clock)
{
    m_clock = move(clock);
}

void PermissionManager::set_timeouts_for_testing(PermissionTimeouts timeouts)
{
    m_timeouts = timeouts;
}

void PermissionManager::did_change()
{
    ++m_revision;
    persist();

    auto observers = m_observers;
    for (auto* observer : observers)
        observer->permissions_changed(*this);
}

void PermissionManager::load()
{
    if (!m_persistence_path.has_value())
        return;

    auto json = read_json_file(*m_persistence_path);
    if (json.is_error())
        return;
    if (json.value().get_integer<i64>("version"sv) != PERSISTENCE_VERSION)
        return;

    auto read_key = [](JsonObject const& object) -> Optional<Entry> {
        auto origin = object.get_string("origin"sv).map([](auto const& string) { return origin_from_string(string); });
        auto name = object.get_string("permission"sv).map([](auto const& string) { return Web::Permissions::permission_name_from_string(string); });
        if (!origin.has_value() || !origin->has_value() || !name.has_value() || !name->has_value())
            return {};
        return Entry { .name = **name, .origin = origin->release_value(), .decision = {}, .decided_at = UnixDateTime::epoch(), .hidden_since = {}, .dismissals = 0, .ignores = 0, .embargo_until = {} };
    };

    if (auto decisions = json.value().get_array("decisions"sv); decisions.has_value()) {
        decisions->for_each([&](JsonValue const& value) {
            if (!value.is_object())
                return;
            auto entry = read_key(value.as_object());
            auto decision = value.as_object().get_string("decision"sv).map([](auto const& string) { return persisted_decision_from_string(string); });
            if (!entry.has_value() || !decision.has_value() || !decision->has_value())
                return;
            ensure_entry(entry->name, entry->origin).decision = **decision;
        });
    }

    if (auto embargoes = json.value().get_array("embargoes"sv); embargoes.has_value()) {
        embargoes->for_each([&](JsonValue const& value) {
            if (!value.is_object())
                return;
            auto const& object = value.as_object();
            auto key = read_key(object);
            if (!key.has_value())
                return;
            auto& entry = ensure_entry(key->name, key->origin);
            entry.dismissals = object.get_integer<u8>("dismissals"sv).value_or(0);
            entry.ignores = object.get_integer<u8>("ignores"sv).value_or(0);
            if (auto until = object.get_integer<i64>("until"sv); until.has_value())
                entry.embargo_until = UnixDateTime::from_milliseconds_since_epoch(*until);
        });
    }

    remove_empty_entries();
}

void PermissionManager::persist() const
{
    if (!m_persistence_path.has_value())
        return;

    JsonArray decisions;
    JsonArray embargoes;

    for (auto const& entry : m_entries) {
        auto origin = entry.origin.serialize();
        auto name = MUST(String::from_utf8(Web::Permissions::to_string(entry.name)));

        if (entry.decision == PermissionDecision::Allow || entry.decision == PermissionDecision::Block) {
            JsonObject object;
            object.set("origin"sv, origin);
            object.set("permission"sv, name);
            object.set("decision"sv, MUST(String::from_utf8(decision_to_string(*entry.decision))));
            decisions.must_append(move(object));
        }

        if (entry.dismissals != 0 || entry.ignores != 0 || entry.embargo_until.has_value()) {
            JsonObject object;
            object.set("origin"sv, origin);
            object.set("permission"sv, name);
            object.set("dismissals"sv, entry.dismissals);
            object.set("ignores"sv, entry.ignores);
            if (entry.embargo_until.has_value())
                object.set("until"sv, entry.embargo_until->milliseconds_since_epoch());
            else
                object.set("until"sv, JsonValue {});
            embargoes.must_append(move(object));
        }
    }

    JsonObject json;
    json.set("version"sv, PERSISTENCE_VERSION);
    json.set("decisions"sv, move(decisions));
    json.set("embargoes"sv, move(embargoes));

    if (auto result = write_json_file(*m_persistence_path, json); result.is_error())
        warnln("Unable to persist site permissions: {}", result.error());
}

}

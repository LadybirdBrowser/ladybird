/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/Time.h>
#include <LibCore/Directory.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/PermissionManager.h>

using namespace WebView;
using Web::Permissions::EffectiveState;
using Web::Permissions::PermissionName;
using Web::Permissions::PromptOutcome;

static URL::Origin origin_of(StringView url)
{
    auto parsed_url = URL::Parser::basic_parse(url);
    VERIFY(parsed_url.has_value());
    return parsed_url->origin();
}

static ByteString persistence_path(StringView name)
{
    auto directory = LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("test-permission-manager-{}", Core::System::getpid())).string();
    MUST(Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes));
    auto path = LexicalPath::join(directory, ByteString::formatted("{}.json", name)).string();
    if (FileSystem::exists(path))
        MUST(FileSystem::remove(path, FileSystem::RecursionMode::Disallowed));
    return path;
}

struct FakeClock {
    UnixDateTime now { UnixDateTime::from_seconds_since_epoch(1'800'000'000) };

    void advance(AK::Duration duration) { now += duration; }
};

static NonnullOwnPtr<PermissionManager> create_manager(FakeClock& clock, StringView name)
{
    auto manager = PermissionManager::create_for_ordinary_session(persistence_path(name));
    manager->set_clock_for_testing([&clock] { return clock.now; });
    return manager;
}

static auto const maps = origin_of("https://maps.example.com/"sv);
static auto const talk = origin_of("https://talk.example.org/"sv);

static constexpr u64 first_view = 1;
static constexpr u64 second_view = 2;

TEST_CASE(allow_always_persists_across_reload)
{
    auto path = persistence_path("allow-always"sv);
    {
        auto manager = PermissionManager::create_for_ordinary_session(path);
        manager->record_prompt_outcome(PermissionName::Geolocation, maps, PromptOutcome::AllowAlways);
        EXPECT(manager->effective_state(PermissionName::Geolocation, maps) == EffectiveState::Granted);
    }
    auto reloaded = PermissionManager::create_for_ordinary_session(path);
    EXPECT(reloaded->effective_state(PermissionName::Geolocation, maps) == EffectiveState::Granted);
    EXPECT(reloaded->decision(PermissionName::Geolocation, maps) == PermissionDecision::Allow);
    EXPECT(reloaded->effective_state(PermissionName::Microphone, maps) == EffectiveState::Prompt);
}

TEST_CASE(allow_once_is_not_persisted)
{
    auto path = persistence_path("allow-once"sv);
    {
        auto manager = PermissionManager::create_for_ordinary_session(path);
        manager->set_view_presence(first_view, maps, true);
        manager->record_prompt_outcome(PermissionName::Geolocation, maps, PromptOutcome::AllowOnce);
        EXPECT(manager->effective_state(PermissionName::Geolocation, maps) == EffectiveState::Granted);
        EXPECT(manager->decision(PermissionName::Geolocation, maps) == PermissionDecision::AllowOnce);
    }
    auto reloaded = PermissionManager::create_for_ordinary_session(path);
    EXPECT(reloaded->effective_state(PermissionName::Geolocation, maps) == EffectiveState::Prompt);
}

TEST_CASE(block_reads_denied)
{
    FakeClock clock;
    auto manager = create_manager(clock, "block"sv);
    manager->record_prompt_outcome(PermissionName::Microphone, talk, PromptOutcome::Block);
    EXPECT(manager->effective_state(PermissionName::Microphone, talk) == EffectiveState::Denied);
    EXPECT(manager->decision(PermissionName::Microphone, talk) == PermissionDecision::Block);
}

TEST_CASE(reset_origin_returns_both_names_to_default)
{
    FakeClock clock;
    auto manager = create_manager(clock, "reset"sv);
    manager->set_decision(PermissionName::Geolocation, maps, PermissionDecision::Allow);
    manager->set_decision(PermissionName::Microphone, maps, PermissionDecision::Block);
    manager->set_decision(PermissionName::Microphone, talk, PermissionDecision::Block);
    manager->reset_origin(maps);
    EXPECT(manager->effective_state(PermissionName::Geolocation, maps) == EffectiveState::Prompt);
    EXPECT(manager->effective_state(PermissionName::Microphone, maps) == EffectiveState::Prompt);
    EXPECT(manager->effective_state(PermissionName::Microphone, talk) == EffectiveState::Denied);
}

TEST_CASE(three_dismissals_embargo_for_seven_days)
{
    FakeClock clock;
    auto manager = create_manager(clock, "dismissals"sv);
    manager->record_prompt_outcome(PermissionName::Geolocation, maps, PromptOutcome::Dismissed);
    manager->record_prompt_outcome(PermissionName::Geolocation, maps, PromptOutcome::Dismissed);
    EXPECT(manager->effective_state(PermissionName::Geolocation, maps) == EffectiveState::Prompt);
    manager->record_prompt_outcome(PermissionName::Geolocation, maps, PromptOutcome::Dismissed);
    EXPECT(manager->effective_state(PermissionName::Geolocation, maps) == EffectiveState::Denied);
    EXPECT(!manager->decision(PermissionName::Geolocation, maps).has_value());
    EXPECT(manager->effective_state(PermissionName::Microphone, maps) == EffectiveState::Prompt);

    clock.advance(AK::Duration::from_seconds(7 * 86400 - 1));
    manager->run_expiry();
    EXPECT(manager->effective_state(PermissionName::Geolocation, maps) == EffectiveState::Denied);
    clock.advance(AK::Duration::from_seconds(1));
    manager->run_expiry();
    EXPECT(manager->effective_state(PermissionName::Geolocation, maps) == EffectiveState::Prompt);
}

TEST_CASE(embargo_persists_across_reload)
{
    FakeClock clock;
    auto path = persistence_path("embargo-persists"sv);
    {
        auto manager = PermissionManager::create_for_ordinary_session(path);
        manager->set_clock_for_testing([&clock] { return clock.now; });
        for (int i = 0; i < 3; ++i)
            manager->record_prompt_outcome(PermissionName::Geolocation, maps, PromptOutcome::Dismissed);
    }
    auto reloaded = PermissionManager::create_for_ordinary_session(path);
    reloaded->set_clock_for_testing([&clock] { return clock.now; });
    EXPECT(reloaded->effective_state(PermissionName::Geolocation, maps) == EffectiveState::Denied);
}

TEST_CASE(four_ignores_embargo)
{
    FakeClock clock;
    auto manager = create_manager(clock, "ignores"sv);
    for (int i = 0; i < 3; ++i)
        manager->record_prompt_outcome(PermissionName::Microphone, talk, PromptOutcome::Ignored);
    EXPECT(manager->effective_state(PermissionName::Microphone, talk) == EffectiveState::Prompt);
    manager->record_prompt_outcome(PermissionName::Microphone, talk, PromptOutcome::Ignored);
    EXPECT(manager->effective_state(PermissionName::Microphone, talk) == EffectiveState::Denied);
}

TEST_CASE(explicit_answer_resets_dismissal_count)
{
    FakeClock clock;
    auto manager = create_manager(clock, "reset-count"sv);
    manager->record_prompt_outcome(PermissionName::Geolocation, maps, PromptOutcome::Dismissed);
    manager->record_prompt_outcome(PermissionName::Geolocation, maps, PromptOutcome::Dismissed);
    manager->record_prompt_outcome(PermissionName::Geolocation, maps, PromptOutcome::AllowAlways);
    manager->set_decision(PermissionName::Geolocation, maps, {});
    manager->record_prompt_outcome(PermissionName::Geolocation, maps, PromptOutcome::Dismissed);
    EXPECT(manager->effective_state(PermissionName::Geolocation, maps) == EffectiveState::Prompt);
}

TEST_CASE(cancelled_records_nothing)
{
    FakeClock clock;
    auto manager = create_manager(clock, "cancelled"sv);
    for (int i = 0; i < 5; ++i)
        manager->record_prompt_outcome(PermissionName::Geolocation, maps, PromptOutcome::Cancelled);
    EXPECT(manager->effective_state(PermissionName::Geolocation, maps) == EffectiveState::Prompt);
}

TEST_CASE(one_time_expires_when_origin_absent)
{
    FakeClock clock;
    auto manager = create_manager(clock, "one-time-absent"sv);
    manager->set_view_presence(first_view, maps, true);
    manager->set_view_presence(second_view, maps, false);
    manager->record_prompt_outcome(PermissionName::Geolocation, maps, PromptOutcome::AllowOnce);

    manager->set_view_presence(first_view, talk, true);
    EXPECT(manager->effective_state(PermissionName::Geolocation, maps) == EffectiveState::Granted);
    manager->remove_view(second_view);
    EXPECT(manager->effective_state(PermissionName::Geolocation, maps) == EffectiveState::Prompt);
}

TEST_CASE(one_time_expires_after_five_minutes_hidden)
{
    FakeClock clock;
    auto manager = create_manager(clock, "one-time-hidden"sv);
    manager->set_view_presence(first_view, maps, true);
    manager->record_prompt_outcome(PermissionName::Geolocation, maps, PromptOutcome::AllowOnce);

    manager->set_view_presence(first_view, maps, false);
    clock.advance(AK::Duration::from_seconds(4 * 60));
    manager->run_expiry();
    manager->set_view_presence(first_view, maps, true);
    manager->set_view_presence(first_view, maps, false);
    clock.advance(AK::Duration::from_seconds(4 * 60));
    manager->run_expiry();
    EXPECT(manager->effective_state(PermissionName::Geolocation, maps) == EffectiveState::Granted);

    clock.advance(AK::Duration::from_seconds(60));
    manager->run_expiry();
    EXPECT(manager->effective_state(PermissionName::Geolocation, maps) == EffectiveState::Prompt);
}

TEST_CASE(one_time_expires_after_sixteen_hours_even_if_visible)
{
    FakeClock clock;
    auto manager = create_manager(clock, "one-time-max"sv);
    manager->set_view_presence(first_view, maps, true);
    manager->record_prompt_outcome(PermissionName::Microphone, maps, PromptOutcome::AllowOnce);
    clock.advance(AK::Duration::from_seconds(16 * 3600 - 1));
    manager->run_expiry();
    EXPECT(manager->effective_state(PermissionName::Microphone, maps) == EffectiveState::Granted);
    clock.advance(AK::Duration::from_seconds(1));
    manager->run_expiry();
    EXPECT(manager->effective_state(PermissionName::Microphone, maps) == EffectiveState::Prompt);
}

TEST_CASE(next_deadline_reports_the_earliest_expiry)
{
    FakeClock clock;
    auto manager = create_manager(clock, "deadline"sv);
    EXPECT(!manager->next_deadline().has_value());
    manager->set_view_presence(first_view, maps, true);
    manager->record_prompt_outcome(PermissionName::Geolocation, maps, PromptOutcome::AllowOnce);
    EXPECT(manager->next_deadline() == clock.now + AK::Duration::from_seconds(16 * 3600));
    manager->set_view_presence(first_view, maps, false);
    EXPECT(manager->next_deadline() == clock.now + AK::Duration::from_seconds(5 * 60));
}

TEST_CASE(os_denial_turns_only_allowed_geolocation_denied)
{
    FakeClock clock;
    auto manager = create_manager(clock, "os-denial"sv);
    manager->set_decision(PermissionName::Geolocation, maps, PermissionDecision::Allow);
    manager->set_decision(PermissionName::Microphone, maps, PermissionDecision::Allow);
    manager->set_os_denies_geolocation(true);
    EXPECT(manager->effective_state(PermissionName::Geolocation, maps) == EffectiveState::Denied);
    EXPECT(manager->effective_state(PermissionName::Geolocation, talk) == EffectiveState::Prompt);
    EXPECT(manager->effective_state(PermissionName::Microphone, maps) == EffectiveState::Granted);
    EXPECT(manager->decision(PermissionName::Geolocation, maps) == PermissionDecision::Allow);
    manager->set_os_denies_geolocation(false);
    EXPECT(manager->effective_state(PermissionName::Geolocation, maps) == EffectiveState::Granted);
}

TEST_CASE(global_default_applies_without_entry)
{
    FakeClock clock;
    auto manager = create_manager(clock, "global-default"sv);
    manager->set_decision(PermissionName::Geolocation, maps, PermissionDecision::Block);
    manager->set_global_default(PermissionName::Geolocation, EffectiveState::Denied);
    EXPECT(manager->effective_state(PermissionName::Geolocation, talk) == EffectiveState::Denied);
    EXPECT(manager->effective_state(PermissionName::Microphone, talk) == EffectiveState::Prompt);
    auto snapshot = manager->snapshot();
    EXPECT(snapshot.geolocation_default == EffectiveState::Denied);
    EXPECT(snapshot.microphone_default == EffectiveState::Prompt);
}

TEST_CASE(snapshot_lists_effective_states)
{
    FakeClock clock;
    auto manager = create_manager(clock, "snapshot"sv);
    manager->set_decision(PermissionName::Geolocation, maps, PermissionDecision::Allow);
    for (int i = 0; i < 3; ++i)
        manager->record_prompt_outcome(PermissionName::Microphone, talk, PromptOutcome::Dismissed);
    auto snapshot = manager->snapshot();
    VERIFY(snapshot.entries.size() == 2);
    for (auto const& entry : snapshot.entries) {
        if (entry.name == PermissionName::Geolocation) {
            EXPECT(entry.top_level_origin.is_same_origin(maps));
            EXPECT(entry.state == EffectiveState::Granted);
        } else {
            EXPECT(entry.top_level_origin.is_same_origin(talk));
            EXPECT(entry.state == EffectiveState::Denied);
        }
    }
}

TEST_CASE(private_session_inherits_ordinary_block_live)
{
    FakeClock clock;
    auto ordinary = create_manager(clock, "private-block"sv);
    auto private_session = PermissionManager::create_for_private_session(*ordinary);
    EXPECT(private_session->effective_state(PermissionName::Geolocation, maps) == EffectiveState::Prompt);
    auto revision_before = private_session->snapshot().revision;
    ordinary->set_decision(PermissionName::Geolocation, maps, PermissionDecision::Block);
    EXPECT(private_session->effective_state(PermissionName::Geolocation, maps) == EffectiveState::Denied);
    EXPECT(private_session->snapshot().revision > revision_before);
    // The site settings panel shows the inherited block as the private session's own decision.
    EXPECT(private_session->decision(PermissionName::Geolocation, maps) == PermissionDecision::Block);

    VERIFY(private_session->snapshot().entries.size() == 1);

    // An answer a private prompt collected before the block is stale now, and stays so once the block is lifted.
    auto generation_before_reset = private_session->decision_generation(PermissionName::Geolocation, maps);
    EXPECT(generation_before_reset > 0);
    ordinary->set_decision(PermissionName::Geolocation, maps, {});
    EXPECT(private_session->decision_generation(PermissionName::Geolocation, maps) > generation_before_reset);
}

TEST_CASE(private_session_does_not_inherit_allow)
{
    FakeClock clock;
    auto ordinary = create_manager(clock, "private-allow"sv);
    ordinary->set_decision(PermissionName::Geolocation, maps, PermissionDecision::Allow);
    auto private_session = PermissionManager::create_for_private_session(*ordinary);
    EXPECT(private_session->effective_state(PermissionName::Geolocation, maps) == EffectiveState::Prompt);
}

TEST_CASE(private_decision_never_persisted)
{
    auto path = persistence_path("private-persist"sv);
    {
        auto ordinary = PermissionManager::create_for_ordinary_session(path);
        auto private_session = PermissionManager::create_for_private_session(*ordinary);
        private_session->record_prompt_outcome(PermissionName::Geolocation, maps, PromptOutcome::AllowAlways);
        EXPECT(private_session->effective_state(PermissionName::Geolocation, maps) == EffectiveState::Granted);
        EXPECT(ordinary->effective_state(PermissionName::Geolocation, maps) == EffectiveState::Prompt);
    }
    auto reloaded = PermissionManager::create_for_ordinary_session(path);
    EXPECT(reloaded->effective_state(PermissionName::Geolocation, maps) == EffectiveState::Prompt);
}

class CountingObserver final : public PermissionManagerObserver {
public:
    virtual void permissions_changed(PermissionManager const&) override { ++count; }
    int count { 0 };
};

TEST_CASE(revision_increases_and_observers_run_once_per_change)
{
    FakeClock clock;
    auto manager = create_manager(clock, "observers"sv);
    CountingObserver observer;
    manager->register_observer(observer);
    auto revision = manager->snapshot().revision;

    manager->set_decision(PermissionName::Geolocation, maps, PermissionDecision::Allow);
    EXPECT_EQ(observer.count, 1);
    EXPECT(manager->snapshot().revision > revision);
    revision = manager->snapshot().revision;

    manager->set_decision(PermissionName::Geolocation, maps, PermissionDecision::Allow);
    EXPECT_EQ(observer.count, 1);
    EXPECT_EQ(manager->snapshot().revision, revision);

    manager->reset_origin(maps);
    EXPECT_EQ(observer.count, 2);
    manager->unregister_observer(observer);
}

TEST_CASE(opaque_origin_is_never_stored)
{
    FakeClock clock;
    auto manager = create_manager(clock, "opaque"sv);
    auto opaque = URL::Origin::create_opaque();
    manager->record_prompt_outcome(PermissionName::Geolocation, opaque, PromptOutcome::AllowAlways);
    manager->set_decision(PermissionName::Geolocation, opaque, PermissionDecision::Block);
    EXPECT(manager->snapshot().entries.is_empty());
    EXPECT(manager->site_decisions(PermissionName::Geolocation).is_empty());
}

TEST_CASE(site_decisions_list_stored_decisions)
{
    FakeClock clock;
    auto manager = create_manager(clock, "site-decisions"sv);
    manager->set_decision(PermissionName::Geolocation, maps, PermissionDecision::Allow);
    manager->set_decision(PermissionName::Geolocation, talk, PermissionDecision::Block);
    manager->set_decision(PermissionName::Microphone, talk, PermissionDecision::Allow);
    auto decisions = manager->site_decisions(PermissionName::Geolocation);
    VERIFY(decisions.size() == 2);
    for (auto const& site : decisions)
        EXPECT(site.decision == (site.origin.is_same_origin(maps) ? PermissionDecision::Allow : PermissionDecision::Block));
}

TEST_CASE(aging_an_origin_leaves_others_alone)
{
    auto manager = PermissionManager::create_for_ordinary_session(persistence_path("aging"sv));
    for (auto const& origin : { maps, talk }) {
        for (int i = 0; i < 3; ++i)
            manager->record_prompt_outcome(PermissionName::Geolocation, origin, PromptOutcome::Dismissed);
    }
    manager->age_origin_for_testing(maps, AK::Duration::from_seconds(7 * 86400));
    EXPECT(manager->effective_state(PermissionName::Geolocation, maps) == EffectiveState::Prompt);
    EXPECT(manager->effective_state(PermissionName::Geolocation, talk) == EffectiveState::Denied);
}

TEST_CASE(global_block_overrides_stored_allow)
{
    FakeClock clock;
    auto manager = create_manager(clock, "global-block"sv);
    manager->set_decision(PermissionName::Geolocation, maps, PermissionDecision::Allow);
    manager->set_global_default(PermissionName::Geolocation, EffectiveState::Denied);
    EXPECT(manager->effective_state(PermissionName::Geolocation, maps) == EffectiveState::Denied);
    EXPECT(manager->decision(PermissionName::Geolocation, maps) == PermissionDecision::Allow);
    manager->set_global_default(PermissionName::Geolocation, EffectiveState::Prompt);
    EXPECT(manager->effective_state(PermissionName::Geolocation, maps) == EffectiveState::Granted);
}

TEST_CASE(hiding_a_one_time_site_reports_a_new_deadline)
{
    FakeClock clock;
    auto manager = create_manager(clock, "deadline-change"sv);
    int deadline_changes = 0;
    manager->on_deadlines_changed = [&] { ++deadline_changes; };
    manager->set_view_presence(first_view, maps, true);
    manager->record_prompt_outcome(PermissionName::Geolocation, maps, PromptOutcome::AllowOnce);
    auto before = deadline_changes;
    manager->set_view_presence(first_view, maps, false);
    EXPECT(deadline_changes > before);
}

TEST_CASE(every_explicit_change_moves_the_decision_generation)
{
    FakeClock clock;
    auto manager = create_manager(clock, "generation"sv);
    auto generation = manager->decision_generation(PermissionName::Microphone, talk);
    manager->set_decision(PermissionName::Microphone, talk, PermissionDecision::Block);
    manager->set_decision(PermissionName::Microphone, talk, {});
    EXPECT(manager->decision_generation(PermissionName::Microphone, talk) != generation);
    auto after_reset = manager->decision_generation(PermissionName::Microphone, talk);
    manager->set_decision(PermissionName::Microphone, talk, PermissionDecision::Allow);
    manager->reset_origin(talk);
    EXPECT(manager->decision_generation(PermissionName::Microphone, talk) != after_reset);
}

// The sessions are torn down in no set order, so a private session's manager may outlive the ordinary session's it
// reads from: it then stops reading from it, and dies without touching it.
TEST_CASE(private_session_manager_outlives_the_ordinary_sessions)
{
    FakeClock clock;
    OwnPtr<PermissionManager> ordinary = create_manager(clock, "private-outlives-ordinary"sv);
    ordinary->set_global_default(PermissionName::Geolocation, EffectiveState::Denied);
    ordinary->set_decision(PermissionName::Geolocation, maps, PermissionDecision::Block);
    OwnPtr<PermissionManager> private_manager = PermissionManager::create_for_private_session(*ordinary);
    EXPECT_EQ(private_manager->effective_state(PermissionName::Geolocation, maps), EffectiveState::Denied);

    ordinary = nullptr;
    EXPECT_EQ(private_manager->effective_state(PermissionName::Geolocation, maps), EffectiveState::Prompt);
    private_manager = nullptr;
}

TEST_CASE(snapshot_for_lists_only_the_given_origins)
{
    FakeClock clock;
    auto manager = create_manager(clock, "snapshot-for"sv);
    manager->set_decision(PermissionName::Geolocation, maps, PermissionDecision::Allow);
    manager->set_decision(PermissionName::Microphone, talk, PermissionDecision::Block);
    manager->set_global_default(PermissionName::Microphone, EffectiveState::Denied);

    auto only_maps = manager->snapshot_for({ maps });
    VERIFY(only_maps.entries.size() == 1);
    EXPECT(only_maps.entries[0].top_level_origin.is_same_origin(maps));
    VERIFY(only_maps.origins.size() == 1);
    EXPECT(only_maps.origins[0].is_same_origin(maps));
    EXPECT(only_maps.microphone_default == EffectiveState::Denied);

    auto none = manager->snapshot_for({});
    EXPECT(none.entries.is_empty());
    EXPECT(none.origins.is_empty());
    EXPECT(none.microphone_default == EffectiveState::Denied);
}

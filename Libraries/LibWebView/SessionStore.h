/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibCore/Timer.h>
#include <LibDatabase/Database.h>
#include <LibURL/URL.h>
#include <LibWebView/Export.h>
#include <LibWebView/SessionHistory.h>
#include <LibWebView/SessionHistorySnapshotStorage.h>

namespace WebView {

using SessionWindowId = i64;
using SessionTabId = i64;
using SessionCloseSequence = i64;

struct ClosedSessionTab {
    URL::URL active_url;
    Optional<SessionHistorySnapshot> history;
};

struct ClosedSessionUnit {
    enum class Origin : i64 {
        CloseAction = 0,
        StartupRecovery = 1,
    };

    Vector<ClosedSessionTab> tabs;
    i64 active_tab_index { 0 };
    bool was_window { false };
    Optional<SessionWindowId> source_window_id;
    Optional<i64> source_tab_index;
    Origin origin { Origin::CloseAction };
};

// Persists live windows and tabs plus a bounded recently-closed stack. State updates coalesce;
// lifecycle mutations write through. Startup recovers leftover open windows.
class WEBVIEW_API SessionStore {
    AK_MAKE_NONCOPYABLE(SessionStore);
    AK_MAKE_NONMOVABLE(SessionStore);

public:
    enum class IsActive {
        No,
        Yes,
    };

    struct TabOpened {
        Optional<SessionWindowId> window_id;
        URL::URL initial_url;
        Optional<i64> insertion_index;
        IsActive is_active { IsActive::No };
    };

    struct TabStateUpdate {
        SessionTabId tab_id { 0 };
        Optional<SessionHistorySnapshot> history;
        URL::URL url;
    };

    struct TabClosed {
        SessionTabId tab_id { 0 };
        UnixDateTime closed_at;
    };

    struct TabMoved {
        SessionTabId tab_id { 0 };
        SessionWindowId new_window_id { 0 };
        i64 ordinal { 0 };
    };

    struct TabOrderChanged {
        SessionWindowId window_id { 0 };
        Vector<SessionTabId> ordered_tabs;
    };

    struct WindowClosing {
        SessionWindowId window_id { 0 };
        i64 active_tab_index { 0 };
        UnixDateTime closed_at;
    };

    static ErrorOr<Database::MigrationOutcome> migrate_schema(Database::Database&, Database::MigrationMode = Database::MigrationMode::Apply);

    static ErrorOr<NonnullOwnPtr<SessionStore>> create(Database::Database&);
    static NonnullOwnPtr<SessionStore> create();

    ~SessionStore();

    ErrorOr<SessionWindowId> window_opened();
    ErrorOr<void> window_closing(WindowClosing);
    void window_detached(SessionWindowId);
    ErrorOr<SessionTabId> tab_opened(TabOpened);
    void update_tab_state(TabStateUpdate);
    ErrorOr<void> tab_closed(TabClosed);
    void tab_detached(SessionTabId);
    void tab_moved(TabMoved);
    void tab_order_changed(TabOrderChanged);
    void active_tab_changed(SessionTabId);
    void application_quitting();
    void application_quit_aborted();

    bool has_closed_units() const;
    ErrorOr<Optional<ClosedSessionUnit>> take_most_recently_closed();
    ErrorOr<void> remove_entries_accessed_since(UnixDateTime);

    void flush_dirty_state();

    Optional<TabStateUpdate> cached_tab_state_for_testing(SessionTabId) const;

    // Fired when a flush completes a previously failed close.
    Function<void()> on_closed_units_changed;

private:
    // Monotonic within one serialized writer.
    class IdAllocator {
    public:
        explicit IdAllocator(i64 last_used = 0)
            : m_last_used(last_used)
        {
        }

        // The statement must yield one row whose first column is the INTEGER maximum id in use, or
        // NULL when no rows exist.
        static ErrorOr<IdAllocator> create_from_maximum(Database::Database&, Database::StatementID);

        ErrorOr<i64> allocate();

    private:
        i64 m_last_used { 0 };
    };

    struct IdAllocators {
        IdAllocator session_ids;
        IdAllocator tab_ids;
        IdAllocator close_sequences;
    };

    struct Statements {
        Database::StatementID insert_session;
        Database::StatementID insert_tab;
        Database::StatementID insert_tab_if_missing;
        Database::StatementID update_tab_row;
        Database::StatementID update_tab_parent;
        Database::StatementID update_session_closed;
        Database::StatementID update_session_active_tab_index;
        Database::StatementID delete_session;
        Database::StatementID delete_tab;
        Database::StatementID prune_closed;
        Database::StatementID select_max_session_id;
        Database::StatementID select_max_tab_id;
        Database::StatementID select_max_close_sequence;
        Database::StatementID select_closed_sessions;
        Database::StatementID select_session_tabs;
        Database::StatementID select_open_sessions;
        Database::StatementID recover_open_session;
        Database::StatementID delete_empty_open_sessions;
        SessionHistorySnapshotStatements snapshot_statements;
    };

    enum class UnitKind : i64 {
        OpenWindow = 0,
        ClosedTab = 1,
        ClosedWindow = 2,
    };

    struct OpenTab {
        SessionWindowId window_id { 0 };
        URL::URL url;
        size_t current_used_step_index { 0 };
        // The pending flush payload in persisted mode, dropped once flushed; the storage itself in
        // transient mode.
        Optional<SessionHistorySnapshot> history;
        bool dirty { false };
    };

    struct OpenWindow {
        Vector<SessionTabId> tabs;
        SessionTabId active_tab { 0 };
        bool metadata_dirty { false };
    };

    struct ClosedTab {
        URL::URL active_url;
        i64 tab_id { 0 };
        size_t current_used_step_index { 0 };
        // Transient mode captures the snapshot at demotion; persisted mode loads it on reopen.
        Optional<SessionHistorySnapshot> history;
    };

    struct ClosedUnit {
        i64 session_id { 0 };
        UnitKind kind { UnitKind::ClosedTab };
        UnixDateTime closed_time;
        SessionCloseSequence close_sequence { 0 };
        i64 active_tab_index { 0 };
        Optional<SessionWindowId> source_window_id;
        Optional<i64> source_tab_index;
        ClosedSessionUnit::Origin origin { ClosedSessionUnit::Origin::CloseAction };
        Vector<ClosedTab> tabs;
    };

    enum class SnapshotDisposition {
        Preserve,
        Replace,
        Clear,
    };

    struct TabWrite {
        SessionTabId id { 0 };
        SessionWindowId window_id { 0 };
        i64 tab_ordinal { 0 };
        URL::URL url;
        i64 current_used_step_index { 0 };
        SnapshotDisposition snapshot_disposition { SnapshotDisposition::Preserve };
        SessionHistorySnapshot const* snapshot { nullptr };
    };

    // Always the complete live-plus-pending order, so persisted units stay contiguous while failed
    // closes await their retry.
    struct WindowMetadataWrite {
        SessionWindowId id { 0 };
        Vector<SessionTabId> tabs_in_order;
        i64 active_tab_index { 0 };
    };

    struct OpenTabWrite {
        TabWrite tab;
        WindowMetadataWrite window;
    };

    struct ClosedUnitWrite {
        SessionWindowId id { 0 };
        UnitKind kind { UnitKind::ClosedTab };
        UnixDateTime closed_at;
        SessionCloseSequence close_sequence { 0 };
        Optional<SessionWindowId> source_window_id;
        Optional<i64> source_tab_index;
        ClosedSessionUnit::Origin origin { ClosedSessionUnit::Origin::CloseAction };
        i64 active_tab_index { 0 };
    };

    struct ClosedTabWrite {
        ClosedUnitWrite unit;
        TabWrite tab;
        Optional<WindowMetadataWrite> remaining_window;
        u32 retention_limit { 0 };
    };

    struct ClosedWindowWrite {
        ClosedUnitWrite unit;
        Vector<TabWrite> members;
        u32 retention_limit { 0 };
    };

    struct DiscardTabWrite {
        SessionTabId id { 0 };
        Optional<WindowMetadataWrite> remaining_window;
    };

    struct ClearClosedUnitsWrite {
        Vector<SessionWindowId> closed_units;
        Vector<DiscardTabWrite> pending_tabs;
        Vector<SessionWindowId> pending_windows;
    };

    // Retain the exact targets until the clear transaction commits. Reapplying a timestamp cutoff
    // would also delete units closed after the user initiated the clear.
    struct PendingClearClosedUnits {
        Vector<SessionWindowId> closed_units;
        Vector<SessionTabId> pending_tabs;
        Vector<SessionWindowId> pending_windows;
    };

    struct RecoveryWrite {
        UnixDateTime recovered_at;
        u32 closed_tab_limit { 0 };
        u32 closed_window_limit { 0 };
    };

    // Applies mutations atomically while preserving persisted row invariants.
    struct PersistedStorage {
        static ErrorOr<Database::MigrationOutcome> migrate_schema(Database::Database&, Database::MigrationMode);
        static ErrorOr<PersistedStorage> create(Database::Database&);

        ErrorOr<void> insert_open_window(SessionWindowId);
        ErrorOr<void> open_tab(OpenTabWrite const&);
        ErrorOr<void> flush_tab(TabWrite const&);
        ErrorOr<void> write_windows_metadata(Vector<WindowMetadataWrite> const&);
        ErrorOr<void> close_tab(ClosedTabWrite const&);
        ErrorOr<void> close_window(ClosedWindowWrite const&);
        ErrorOr<void> discard_tab(DiscardTabWrite const&);
        ErrorOr<void> clear_closed_units(ClearClosedUnitsWrite const&);
        ErrorOr<void> recover_open_windows(RecoveryWrite const&, IdAllocator& close_sequence_allocator);
        ErrorOr<void> delete_session_rows(SessionWindowId);
        ErrorOr<void> prune_closed_units(UnitKind, u32 max_units);

        ErrorOr<IdAllocators> create_id_allocators();
        ErrorOr<SessionHistorySnapshot> load_tab_snapshot(SessionTabId, size_t current_used_step_index);
        ErrorOr<Vector<ClosedUnit>> load_closed_units();
        ErrorOr<Vector<ClosedSessionTab>> take_closed_unit(SessionWindowId);

        Database::Database& database;
        Statements statements;

    private:
        ErrorOr<void> apply_tab_row_write(TabWrite const&);
        ErrorOr<void> apply_window_metadata(WindowMetadataWrite const&);
        ErrorOr<void> insert_closed_unit_row(ClosedUnitWrite const&);
        ErrorOr<void> delete_tab_rows(SessionTabId);
    };

    // A close whose database write failed; the frontend tab or window is already gone, so the
    // demotion retries from these copies instead of holding the unit open.
    struct PendingTabClose {
        SessionTabId tab_id { 0 };
        i64 unit_id { 0 };
        OpenTab tab;
        UnixDateTime closed_at;
        SessionCloseSequence close_sequence { 0 };
        Optional<i64> source_tab_index;
        bool discard { false };
    };

    struct PendingWindowClose {
        SessionWindowId window_id { 0 };
        Vector<PendingTabClose> members;
        i64 active_tab_index { 0 };
        UnixDateTime closed_at;
        SessionCloseSequence close_sequence { 0 };
        bool discard { false };
    };

    // Owns in-memory membership, ordering, and dirty state.
    class TransientStorage {
    public:
        void open_window(SessionWindowId);
        Optional<size_t> open_tab(SessionWindowId, SessionTabId, OpenTab, Optional<size_t> insertion_index = {});
        OpenTab const* find_tab(SessionTabId) const;
        OpenWindow const* find_window(SessionWindowId) const;
        Optional<size_t> tab_index(SessionTabId) const;

        void set_tab_state(SessionTabId, Optional<SessionHistorySnapshot>, URL::URL, bool dirty);
        void mark_tab_flushed(SessionTabId, size_t current_used_step_index);
        Optional<SessionHistorySnapshot> take_tab_history(SessionTabId);
        void set_active_tab(SessionTabId);
        void set_tab_order(SessionWindowId, Vector<SessionTabId> const&);
        Optional<SessionWindowId> move_tab(SessionTabId, SessionWindowId, size_t ordinal);
        void remove_tab(SessionTabId);
        void remove_window(SessionWindowId);
        void mark_window_metadata_clean(SessionWindowId);

        template<typename Callback>
        void for_each_window(Callback callback) const
        {
            for (auto const& it : m_windows)
                callback(it.key, it.value);
        }

        template<typename Callback>
        void for_each_tab(Callback callback) const
        {
            for (auto const& it : m_tabs)
                callback(it.key, it.value);
        }

        bool has_closed_units() const { return !m_closed_units.is_empty(); }
        void set_closed_units(Vector<ClosedUnit>);
        void append_closed_unit(ClosedUnit);
        ClosedUnit const* last_closed_unit() const;
        ClosedUnit take_last_closed_unit();
        void prune_closed_units(UnitKind, u32 max_units);
        Vector<SessionWindowId> remove_closed_units_since(UnixDateTime);

        Optional<SessionWindowId> default_window;

    private:
        HashMap<SessionWindowId, OpenWindow> m_windows;
        HashMap<SessionTabId, OpenTab> m_tabs;

        // Oldest first by close sequence; reopen consumes from the back.
        Vector<ClosedUnit> m_closed_units;
    };

    explicit SessionStore(Optional<PersistedStorage>);

    ErrorOr<void> run_startup_recovery(UnixDateTime recovered_at);
    ErrorOr<void> flush_tab(SessionTabId, OpenTab const&);
    ErrorOr<void> drain_window_metadata();
    WindowMetadataWrite window_metadata_write(SessionWindowId, OpenWindow const&) const;
    Optional<WindowMetadataWrite> remaining_window_write(SessionWindowId, SessionTabId closing_tab) const;
    TabWrite tab_write_for(SessionTabId, OpenTab const&, i64 tab_ordinal) const;
    bool window_has_pending_tab_closes(SessionWindowId) const;
    ErrorOr<void> finish_tab_close(PendingTabClose const&);
    ErrorOr<void> finish_window_close(PendingWindowClose const&);
    ErrorOr<void> retry_pending_clear_closed_units();
    void append_closed_tab_unit(PendingTabClose const&);
    void append_closed_window_unit(PendingWindowClose const&);
    ErrorOr<SessionWindowId> default_window();
    static bool is_tab_worth_keeping(URL::URL const&);
    static u32 max_closed_units(UnitKind);

    Optional<PersistedStorage> m_persisted_storage;
    TransientStorage m_transient_storage;
    RefPtr<Core::Timer> m_flush_timer;
    Vector<PendingTabClose> m_pending_tab_closes;
    Vector<PendingWindowClose> m_pending_window_closes;
    Optional<PendingClearClosedUnits> m_pending_clear_closed_units;
    Vector<WindowMetadataWrite> m_retired_window_metadata;
    bool m_quitting { false };

    IdAllocator m_session_id_allocator;
    IdAllocator m_tab_id_allocator;
    IdAllocator m_close_sequence_allocator;
};

}

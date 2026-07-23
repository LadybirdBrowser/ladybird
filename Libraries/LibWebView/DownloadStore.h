/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibDatabase/Forward.h>
#include <LibWebView/Export.h>

namespace WebView {

struct DownloadSegmentRecord {
    u64 start_offset { 0 };
    u64 end_offset { 0 };
    u64 next_offset { 0 };
};

struct WEBVIEW_API DownloadRecord {
    i64 id { 0 };

    String url;
    String display_url;

    String destination;
    String temporary_destination;
    i64 total_size { 0 };
    Optional<String> etag;
    Optional<String> last_modified;
    Vector<DownloadSegmentRecord> segments;
    UnixDateTime created_time;

    bool can_restart_from_zero { false };
};

class WEBVIEW_API DownloadStore {
    AK_MAKE_NONCOPYABLE(DownloadStore);
    AK_MAKE_NONMOVABLE(DownloadStore);

public:
    static ErrorOr<Database::MigrationOutcome> migrate_schema(Database::Database&, Database::MigrationMode = Database::MigrationMode::Apply);

    static ErrorOr<NonnullOwnPtr<DownloadStore>> create(Database::Database&);

    static NonnullOwnPtr<DownloadStore> create_disabled();

    ~DownloadStore();

    void save_download(DownloadRecord const&, UnixDateTime updated_at = UnixDateTime::now());
    void remove_download(i64 id);
    Vector<DownloadRecord> resumable_downloads();

    i64 maximum_download_id();

private:
    struct Statements {
        Database::StatementID upsert_download { 0 };
        Database::StatementID delete_download { 0 };
        Database::StatementID list_downloads { 0 };
        Database::StatementID maximum_download_id { 0 };
    };

    DownloadStore() = default;
    DownloadStore(Database::Database&, Statements&&);

    Database::Database* m_database { nullptr };
    Statements m_statements;
};

}

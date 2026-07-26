/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Badge.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/LexicalPath.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <LibHTTP/Forward.h>
#include <LibRequests/CameFromCache.h>
#include <LibRequests/Forward.h>
#include <LibRequests/NetworkError.h>
#include <LibURL/URL.h>
#include <LibWebView/Forward.h>
#include <LibWebView/PrivateBrowsing.h>

namespace Core {

class File;

}

namespace WebView {

class FileDownloaderObserver;

class WEBVIEW_API FileDownloader {
public:
    enum class DownloadStatus : u8 {
        InProgress,
        Paused,
        Completed,
        Canceled,
        Failed,
    };

    static bool status_is_active(DownloadStatus status)
    {
        return status == DownloadStatus::InProgress || status == DownloadStatus::Paused;
    }

    struct SegmentProgress {
        u64 start_offset { 0 };
        u64 end_offset { 0 };
        u64 next_offset { 0 };
    };

    struct WEBVIEW_API Download {
        Optional<double> progress() const;

        u64 id { 0 };
        IsPrivate is_private { IsPrivate::No };
        URL::URL url;
        LexicalPath destination;
        DownloadStatus status { DownloadStatus::InProgress };
        u64 downloaded_size { 0 };
        Optional<u64> total_size;
        Optional<String> error;

        bool can_resume { false };
        u32 connection_count { 0 };
        Optional<u64> bytes_per_second;
        bool is_waiting_to_retry { false };
        UnixDateTime created_time;

        Optional<AK::Duration> estimated_time_remaining() const;
    };

    FileDownloader();
    ~FileDownloader();

    u64 download_file(IsPrivate, URL::URL const&, LexicalPath);
    u64 adopt_download(IsPrivate, URL::URL const&, LexicalPath, Optional<u64> total_size, int request_server_client_id, u64 request_server_request_id, ReadonlyBytes initial_data = {});
    u64 start_download(IsPrivate, URL::URL const&, LexicalPath, Optional<u64> total_size = {});
    bool has_active_downloads() const;
    bool has_unresumable_downloads() const;
    void set_cancel_callback(u64 id, Function<void()>);
    void append_download_data(u64 id, ReadonlyBytes);
    void finish_download(u64 id);
    void cancel_unresumable_downloads();
    void cancel_private_downloads();
    void cancel_download(u64 id);
    void pause_download(u64 id);
    void resume_download(u64 id);
    void pause_active_downloads();
    void fail_download(u64 id, String);
    Vector<u64> prune_inactive_downloads();
    Vector<u64> remove_inactive_downloads_created_since(UnixDateTime);

    ReadonlySpan<Download> downloads() const { return m_downloads.span(); }
    Optional<Download const&> download(u64 id) const;

    Vector<SegmentProgress> segment_progress(u64 id) const;

    void adopt_download_store(Badge<Application>, DownloadStore&);

    static void add_observer(Badge<FileDownloaderObserver>, FileDownloaderObserver&);
    static void remove_observer(Badge<FileDownloaderObserver>, FileDownloaderObserver&);

private:
    struct ActiveDownload;
    struct Segment;

    Download* mutable_download_or_null(u64 id);
    ActiveDownload* active_download(u64 id);
    void start_download_request(u64 id, URL::URL const&);
    void follow_download_redirect(u64 id, HTTP::HeaderList const&);
    void attach_request_to_download(u64 id, size_t segment_index, NonnullRefPtr<Requests::Request>);
    void maybe_redistribute_segments(u64 id);
    void start_stall_watchdog(u64 id);
    void check_for_stalled_segments(u64 id);
    void restart_stalled_segment(u64 id, size_t segment_index);
    void handle_segment_headers(u64 id, size_t segment_index, u64 request_generation, HTTP::HeaderList const&, Optional<u32> response_code, Optional<String> const& reason_phrase, Requests::CameFromCache);
    void handle_segment_finished(u64 id, size_t segment_index, u64 request_generation, u64 delivered_size, Optional<Requests::NetworkError> const&);
    bool validate_range_response(u64 id, size_t segment_index, HTTP::HeaderList const&, Optional<u32> response_code);
    void append_segment_data(u64 id, size_t segment_index, ReadonlyBytes, Optional<u64> request_generation = {});
    static ErrorOr<void> write_segment_data(ActiveDownload&, Segment&, ReadonlyBytes);
    static void refresh_download_progress(Download&, ActiveDownload const&);
    static void sample_download_rate(Download&, ActiveDownload&);
    static void stop_segment_request(ActiveDownload&, size_t segment_index);

    void fail_or_pause_download(u64 id, String);

    enum class PersistUrgency : u8 {
        Throttled,
        Immediate,
    };
    void persist_download_snapshot(u64 id, PersistUrgency);
    void forget_persisted_download(u64 id);
    void restore_persisted_downloads();
    bool restore_persisted_download(DownloadRecord&);
    static void remove_orphaned_temporary_files(HashTable<ByteString> const& directories, HashTable<ByteString> const& temporary_paths_in_use);

    void maybe_split_download(u64 id);
    void start_segment_request(u64 id, size_t segment_index);
    bool retry_segment_request(u64 id, size_t segment_index, AK::Duration delay = {});
    void handle_rate_limited_segment(u64 id, size_t segment_index, HTTP::HeaderList const&);
    void abandon_segmentation(u64 id);
    void restart_download_from_zero(u64 id, String reason_if_exhausted);
    void maybe_finish_download(u64 id);
    void discard_active_download(u64 id);

    void notify_download_added(Download const&);
    void notify_download_updated(Download const&);
    void notify_download_removed(u64 id);

    Vector<Download> m_downloads;
    HashMap<u64, NonnullOwnPtr<ActiveDownload>> m_active_downloads;
    Vector<FileDownloaderObserver&> m_observers;
    DownloadStore* m_download_store { nullptr };
    u64 m_next_download_id { 0 };
};

class WEBVIEW_API FileDownloaderObserver {
public:
    FileDownloaderObserver();
    virtual ~FileDownloaderObserver();

    virtual void download_added(FileDownloader::Download const&) { }
    virtual void download_updated(FileDownloader::Download const&) { }
    virtual void download_removed(u64) { }
};

}

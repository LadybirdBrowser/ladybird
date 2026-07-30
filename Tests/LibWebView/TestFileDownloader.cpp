/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibMain/Main.h>
#include <LibSync/Mutex.h>
#include <LibThreading/Thread.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/FileDownloader.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>

namespace {

class TestApplication : public WebView::Application {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : WebView::Application(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        web_content_options.is_test_mode = WebView::IsTestMode::Yes;
    }

    virtual bool should_coordinate_browser_process() const override { return false; }
};

enum class RangeSupport {
    Yes,
    No,
};

enum class RangeRequestBehavior {
    Serve,
    Reject429,
    Reject403,
};

enum class Validator {
    Yes,
    No,
    ChangesBetweenResponses,
};

enum class StallBehavior {
    None,
    StallFirstRangeRequest,
    TrickleFirstRangeRequest,
};

class TestHttpServer {
public:
    TestHttpServer(ByteBuffer body, RangeSupport range_support, RangeRequestBehavior range_request_behavior = RangeRequestBehavior::Serve, Validator validator = Validator::Yes, StallBehavior stall_behavior = StallBehavior::None)
        : m_body(move(body))
        , m_range_support(range_support)
        , m_range_request_behavior(range_request_behavior)
        , m_validator(validator)
        , m_stall_behavior(stall_behavior)
        , m_stall_pending(stall_behavior != StallBehavior::None)
    {
        m_socket = MUST(Core::System::socket(AF_INET, SOCK_STREAM, 0));

        int reuse_address = 1;
        MUST(Core::System::setsockopt(m_socket, SOL_SOCKET, SO_REUSEADDR, &reuse_address, sizeof(reuse_address)));

        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;

        MUST(Core::System::bind(m_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)));
        MUST(Core::System::listen(m_socket, 16));

        socklen_t address_size = sizeof(address);
        VERIFY(::getsockname(m_socket, reinterpret_cast<sockaddr*>(&address), &address_size) == 0);
        m_port = ntohs(address.sin_port);

        m_thread = Threading::Thread::construct("TestHttpServer"sv, [this] -> intptr_t {
            run();
            return 0;
        });
        m_thread->start();
    }

    ~TestHttpServer()
    {
        m_shutting_down.store(true);
        wake_server();
        (void)m_thread->join();
        for (auto& thread : m_stalled_connection_threads)
            (void)thread->join();
        (void)Core::System::close(m_socket);
    }

    u16 port() const { return m_port; }

    Vector<ByteString> range_requests()
    {
        Sync::MutexLocker locker { m_mutex };
        return m_range_requests;
    }

    size_t request_count()
    {
        Sync::MutexLocker locker { m_mutex };
        return m_request_count;
    }

    size_t refused_count()
    {
        Sync::MutexLocker locker { m_mutex };
        return m_refused_count;
    }

private:
    void run()
    {
        while (!m_shutting_down.load()) {
            auto client = ::accept(m_socket, nullptr, nullptr);
            if (client < 0)
                return;

            if (handle_connection(client))
                (void)Core::System::close(client);
        }
    }

    void wake_server()
    {
        auto socket = Core::System::socket(AF_INET, SOCK_STREAM, 0);
        if (socket.is_error())
            return;

        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(m_port);

        (void)::connect(socket.value(), reinterpret_cast<sockaddr*>(&address), sizeof(address));
        (void)Core::System::close(socket.value());
    }

    // Returns whether the caller still owns the client socket and should close it.
    bool handle_connection(int client)
    {
        StringBuilder request;
        Array<u8, 4096> buffer;

        while (!request.string_view().contains("\r\n\r\n"sv)) {
            auto read = Core::System::read(client, buffer);
            if (read.is_error() || read.value() == 0)
                return true;
            request.append(StringView { buffer.span().trim(read.value()) });
        }

        Optional<ByteString> range_header;
        for (auto line : request.string_view().split_view("\r\n"sv)) {
            if (line.starts_with("Range:"sv, CaseSensitivity::CaseInsensitive))
                range_header = line.substring_view(6).trim_whitespace();
        }

        {
            Sync::MutexLocker locker { m_mutex };
            ++m_request_count;
            if (range_header.has_value())
                m_range_requests.append(*range_header);
        }

        if (range_header.has_value() && m_range_request_behavior != RangeRequestBehavior::Serve) {
            {
                Sync::MutexLocker locker { m_mutex };
                ++m_refused_count;
            }

            StringBuilder rejection;
            if (m_range_request_behavior == RangeRequestBehavior::Reject429) {
                rejection.append("HTTP/1.1 429 Too Many Requests\r\n"sv);
                rejection.append("Retry-After: 1\r\n"sv);
            } else {
                rejection.append("HTTP/1.1 403 Forbidden\r\n"sv);
            }
            rejection.append("Content-Length: 0\r\n"sv);
            rejection.append("Connection: close\r\n\r\n"sv);

            (void)write_all(client, rejection.string_view().bytes());
            return true;
        }

        size_t start = 0;
        size_t end = m_body.size() - 1;
        auto is_partial = false;

        if (range_header.has_value() && m_range_support == RangeSupport::Yes) {
            auto value = range_header->view();
            if (value.starts_with("bytes="sv)) {
                auto parts = value.substring_view(6).split_view('-', SplitBehavior::KeepEmpty);
                if (parts.size() >= 1) {
                    if (auto parsed = parts[0].to_number<size_t>(); parsed.has_value())
                        start = *parsed;
                    if (parts.size() >= 2 && !parts[1].is_empty()) {
                        if (auto parsed = parts[1].to_number<size_t>(); parsed.has_value())
                            end = min(*parsed, m_body.size() - 1);
                    }
                    is_partial = start <= end;
                }
            }
        }

        if (!is_partial) {
            start = 0;
            end = m_body.size() - 1;
        }

        auto length = end - start + 1;

        StringBuilder response;
        response.appendff("HTTP/1.1 {}\r\n", is_partial ? "206 Partial Content"sv : "200 OK"sv);
        response.append("Content-Type: application/octet-stream\r\n"sv);
        response.appendff("Content-Length: {}\r\n", length);
        response.appendff("Accept-Ranges: {}\r\n", m_range_support == RangeSupport::Yes ? "bytes"sv : "none"sv);
        if (m_validator == Validator::Yes)
            response.append("ETag: \"test-etag\"\r\n"sv);
        else if (m_validator == Validator::ChangesBetweenResponses)
            response.appendff("ETag: \"{}\"\r\n", is_partial ? "changed-etag"sv : "test-etag"sv);
        if (is_partial)
            response.appendff("Content-Range: bytes {}-{}/{}\r\n", start, end, m_body.size());
        response.append("Connection: close\r\n\r\n"sv);

        // Keep this socket on another thread so the accept loop can serve replacement requests.
        if (is_partial && m_stall_pending.exchange(false)) {
            auto thread = Threading::Thread::construct("StalledConnection"sv, [this, client, response = response.to_byte_string(), start, length] -> intptr_t {
                (void)write_all(client, response.bytes());

                if (m_stall_behavior == StallBehavior::StallFirstRangeRequest) {
                    (void)write_all(client, m_body.bytes().slice(start, length / 2));

                    while (!m_shutting_down.load())
                        (void)Core::System::sleep_ms(50);
                } else {
                    static constexpr size_t trickle_chunk_size = 256 * KiB;

                    for (size_t offset = 0; offset < length && !m_shutting_down.load(); offset += trickle_chunk_size) {
                        if (!write_all(client, m_body.bytes().slice(start + offset, min(trickle_chunk_size, length - offset))))
                            break;
                        (void)Core::System::sleep_ms(50);
                    }
                }

                (void)Core::System::close(client);
                return 0;
            });
            thread->start();
            m_stalled_connection_threads.append(move(thread));
            return false;
        }

        if (!write_all(client, response.string_view().bytes()))
            return true;
        (void)write_all(client, m_body.bytes().slice(start, length));
        return true;
    }

    static bool write_all(int fd, ReadonlyBytes bytes)
    {
        while (!bytes.is_empty()) {
            auto written = Core::System::write(fd, bytes);
            if (written.is_error() || written.value() == 0)
                return false;
            bytes = bytes.slice(written.value());
        }
        return true;
    }

    ByteBuffer m_body;
    RangeSupport m_range_support;
    RangeRequestBehavior m_range_request_behavior { RangeRequestBehavior::Serve };
    Validator m_validator { Validator::Yes };
    int m_socket { -1 };
    u16 m_port { 0 };
    RefPtr<Threading::Thread> m_thread;
    Vector<NonnullRefPtr<Threading::Thread>> m_stalled_connection_threads;
    StallBehavior m_stall_behavior { StallBehavior::None };
    Atomic<bool> m_shutting_down { false };
    Atomic<bool> m_stall_pending { false };

    Sync::Mutex m_mutex;
    Vector<ByteString> m_range_requests;
    size_t m_request_count { 0 };
    size_t m_refused_count { 0 };
};

class DownloadWatcher final : public WebView::FileDownloaderObserver {
public:
    virtual void download_updated(WebView::FileDownloader::Download const& download) override
    {
        if (download.id != m_download_id)
            return;

        m_status = download.status;
        m_error = download.error;
        m_downloaded_size = download.downloaded_size;
        m_can_resume = download.can_resume;

        if (on_update)
            on_update(download);
    }

    void watch(u64 download_id) { m_download_id = download_id; }

    bool is_finished() const
    {
        return m_status != WebView::FileDownloader::DownloadStatus::InProgress
            && m_status != WebView::FileDownloader::DownloadStatus::Paused;
    }

    WebView::FileDownloader::DownloadStatus status() const { return m_status; }
    Optional<String> const& error() const { return m_error; }
    u64 downloaded_size() const { return m_downloaded_size; }
    bool can_resume() const { return m_can_resume; }

    Function<void(WebView::FileDownloader::Download const&)> on_update;

private:
    u64 m_download_id { NumericLimits<u64>::max() };
    WebView::FileDownloader::DownloadStatus m_status { WebView::FileDownloader::DownloadStatus::InProgress };
    Optional<String> m_error;
    u64 m_downloaded_size { 0 };
    bool m_can_resume { false };
};

ByteBuffer make_body(size_t size)
{
    auto body = MUST(ByteBuffer::create_uninitialized(size));
    for (size_t i = 0; i < size; ++i)
        body[i] = static_cast<u8>((i * 31 + (i >> 8) * 17) & 0xff);
    return body;
}

ByteString run_download(TestHttpServer& server, StringView destination_directory, StringView file_name, Function<void(u64, DownloadWatcher&)> during_download = {})
{
    auto& downloader = WebView::Application::the().file_downloader();

    DownloadWatcher watcher;
    auto destination = LexicalPath::join(destination_directory, file_name);
    auto url = URL::Parser::basic_parse(ByteString::formatted("http://127.0.0.1:{}/file", server.port()));
    VERIFY(url.has_value());

    auto download_id = downloader.download_file(WebView::IsPrivate::No, *url, destination);
    watcher.watch(download_id);

    if (during_download)
        during_download(download_id, watcher);

    Core::EventLoop::current().spin_until([&] { return watcher.is_finished(); });
    if (watcher.status() != WebView::FileDownloader::DownloadStatus::Completed)
        warnln("download did not complete: status {}, error {}", to_underlying(watcher.status()), watcher.error().value_or("(none)"_string));
    VERIFY(watcher.status() == WebView::FileDownloader::DownloadStatus::Completed);

    return destination.string();
}

void expect_file_matches(StringView path, ReadonlyBytes expected)
{
    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    auto contents = MUST(file->read_until_eof());

    VERIFY(contents.size() == expected.size());
    VERIFY(contents.bytes() == expected);
}

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto test_directory = ByteString::formatted("{}/Ladybird-TestFileDownloader-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
    TRY(Core::Directory::create(test_directory, Core::Directory::CreateDirectories::Yes));
    auto cleanup_test_directory = ScopeGuard([&] {
        MUST(FileSystem::remove(test_directory, FileSystem::RecursionMode::Allowed));
    });
    VERIFY(setenv("XDG_CONFIG_HOME", test_directory.characters(), 1) == 0);
    VERIFY(setenv("XDG_DOWNLOAD_DIR", test_directory.characters(), 1) == 0);

    signal(SIGPIPE, SIG_IGN);

#if defined(LADYBIRD_BINARY_PATH)
    auto app = TRY(TestApplication::create(arguments, LADYBIRD_BINARY_PATH));
#else
    auto app = TRY(TestApplication::create(arguments, OptionalNone {}));
#endif

    auto body = make_body(12 * MiB);

    {
        TestHttpServer server { body, RangeSupport::Yes };

        auto path = run_download(server, test_directory, "ranges.bin"sv);
        expect_file_matches(path, body.bytes());

        auto range_requests = server.range_requests();
        outln("multi-connection: {} requests, {} of them ranged", server.request_count(), range_requests.size());
        VERIFY(range_requests.size() >= 2);
    }

    {
        TestHttpServer server { body, RangeSupport::No };

        auto path = run_download(server, test_directory, "no-ranges.bin"sv);
        expect_file_matches(path, body.bytes());

        outln("single connection: {} requests, {} of them ranged", server.request_count(), server.range_requests().size());
        VERIFY(server.request_count() == 1);
        VERIFY(server.range_requests().is_empty());
    }

    {
        TestHttpServer server { body, RangeSupport::Yes };
        auto& downloader = WebView::Application::the().file_downloader();

        auto paused = false;
        auto path = run_download(server, test_directory, "paused.bin"sv, [&](u64 download_id, DownloadWatcher& watcher) {
            watcher.on_update = [&, download_id](auto const& download) {
                if (paused || !download.can_resume || download.downloaded_size < 1 * MiB)
                    return;

                paused = true;
                downloader.pause_download(download_id);

                Core::deferred_invoke([&downloader, download_id] {
                    downloader.resume_download(download_id);
                });
            };
        });

        VERIFY(paused);
        expect_file_matches(path, body.bytes());
        outln("pause and resume: {} requests, {} of them ranged", server.request_count(), server.range_requests().size());
    }

    {
        WebView::Application::settings().set_config_variable(WebView::ConfigVariableID::SplitDownloadsWithoutValidators, true);
        auto reset_setting = ScopeGuard([] {
            WebView::Application::settings().set_config_variable(WebView::ConfigVariableID::SplitDownloadsWithoutValidators, false);
        });

        TestHttpServer server { body, RangeSupport::Yes, RangeRequestBehavior::Serve, Validator::No };

        auto path = run_download(server, test_directory, "no-validator-opt-in.bin"sv);
        expect_file_matches(path, body.bytes());

        auto range_requests = server.range_requests();
        outln("no validator, split allowed: {} requests, {} of them ranged", server.request_count(), range_requests.size());
        VERIFY(range_requests.size() >= 2);
    }

    {
        WebView::Application::settings().set_config_variable(WebView::ConfigVariableID::MaximumConnectionsPerDownload, 2);
        auto reset_setting = ScopeGuard([] {
            WebView::Application::settings().set_config_variable(WebView::ConfigVariableID::MaximumConnectionsPerDownload, 4);
        });

        TestHttpServer server { body, RangeSupport::Yes };

        auto path = run_download(server, test_directory, "two-connections.bin"sv);
        expect_file_matches(path, body.bytes());

        outln("two connections: {} requests, {} of them ranged", server.request_count(), server.range_requests().size());
        // Three minimum, but redistribution may add more depending on how the serial server interleaves with response validation.
        VERIFY(server.request_count() >= 3);
        VERIFY(server.range_requests().size() == server.request_count() - 1);
    }

    {
        WebView::Application::settings().set_config_variable(WebView::ConfigVariableID::MaximumConnectionsPerDownload, 8);
        auto reset_setting = ScopeGuard([] {
            WebView::Application::settings().set_config_variable(WebView::ConfigVariableID::MaximumConnectionsPerDownload, 4);
        });

        TestHttpServer server { body, RangeSupport::Yes };

        // The 12 MiB body splits into 6 segments, enough to reallocate the segment list.
        auto path = run_download(server, test_directory, "eight-connections.bin"sv);
        expect_file_matches(path, body.bytes());

        outln("eight connections: {} requests, {} of them ranged", server.request_count(), server.range_requests().size());
        // Six minimum, but redistribution may add more depending on how the serial server interleaves with response validation.
        VERIFY(server.request_count() >= 6);
        VERIFY(server.range_requests().size() == server.request_count() - 1);
    }

    {
        TestHttpServer server { body, RangeSupport::Yes, RangeRequestBehavior::Serve, Validator::Yes, StallBehavior::StallFirstRangeRequest };

        auto path = run_download(server, test_directory, "stalled.bin"sv);
        expect_file_matches(path, body.bytes());

        outln("stalled connection: {} requests, {} of them ranged", server.request_count(), server.range_requests().size());
        VERIFY(server.request_count() >= 5);
        VERIFY(server.range_requests().size() >= 4);
    }

    {
        WebView::Application::settings().set_config_variable(WebView::ConfigVariableID::MaximumConnectionsPerDownload, 2);
        auto reset_setting = ScopeGuard([] {
            WebView::Application::settings().set_config_variable(WebView::ConfigVariableID::MaximumConnectionsPerDownload, 4);
        });

        TestHttpServer server { body, RangeSupport::Yes, RangeRequestBehavior::Serve, Validator::Yes, StallBehavior::TrickleFirstRangeRequest };

        auto path = run_download(server, test_directory, "redistributed.bin"sv);
        expect_file_matches(path, body.bytes());

        outln("redistributed: {} requests, {} of them ranged", server.request_count(), server.range_requests().size());
        VERIFY(server.request_count() >= 3);
        VERIFY(server.range_requests().size() >= 2);
    }

    {
        TestHttpServer server { body, RangeSupport::Yes, RangeRequestBehavior::Reject429 };

        auto path = run_download(server, test_directory, "rate-limited.bin"sv);
        expect_file_matches(path, body.bytes());

        outln("rate limited: {} requests, {} of them refused", server.request_count(), server.refused_count());
        VERIFY(server.refused_count() >= 1);

        VERIFY(server.request_count() <= 8);
    }

    {
        TestHttpServer server { body, RangeSupport::Yes, RangeRequestBehavior::Serve, Validator::No };

        auto path = run_download(server, test_directory, "no-validator.bin"sv);
        expect_file_matches(path, body.bytes());

        outln("no validator: {} requests, {} of them ranged", server.request_count(), server.range_requests().size());
        VERIFY(server.request_count() == 1);
        VERIFY(server.range_requests().is_empty());
    }

    {
        TestHttpServer server { body, RangeSupport::Yes, RangeRequestBehavior::Reject403 };

        auto path = run_download(server, test_directory, "refused.bin"sv);
        expect_file_matches(path, body.bytes());

        outln("refused ranges: {} requests, {} of them refused", server.request_count(), server.refused_count());
        VERIFY(server.refused_count() >= 1);

        VERIFY(server.refused_count() <= 4);
    }

    {
        TestHttpServer server { body, RangeSupport::Yes, RangeRequestBehavior::Serve, Validator::ChangesBetweenResponses };

        auto path = run_download(server, test_directory, "changed-validator.bin"sv);
        expect_file_matches(path, body.bytes());

        auto range_requests = server.range_requests();
        outln("changed validator: {} requests, {} of them ranged", server.request_count(), range_requests.size());
        VERIFY(range_requests.size() >= 1);
    }

    outln("PASS");
    return 0;
}

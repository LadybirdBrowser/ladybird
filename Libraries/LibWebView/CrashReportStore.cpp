/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/CharacterTypes.h>
#include <AK/LexicalPath.h>
#include <AK/NeverDestroyed.h>
#include <AK/QuickSort.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <LibCore/CrashHandler.h>
#include <LibCore/CrashReportData.h>
#include <LibCore/DirIterator.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/Process.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibWebView/CrashReport.h>
#include <LibWebView/CrashReportStore.h>
#include <LibWebView/ProcessManager.h>

#if !defined(AK_OS_WINDOWS)
#    include <fcntl.h>
#    include <stdlib.h>
#    include <sys/file.h>
#    include <time.h>
#    include <unistd.h>
#endif

namespace WebView {

using namespace Core::CrashReportData;

ByteString CrashReportStore::default_directory()
{
    return LexicalPath::join(Core::StandardPaths::user_data_directory(), "Ladybird"sv, "CrashReports"sv).string();
}

// The browser's own store lives under the user data directory.
CrashReportStore& CrashReportStore::the()
{
    static NeverDestroyed<CrashReportStore> store { default_directory() };
    return *store;
}

bool CrashReportStore::is_saved_report_name(StringView name)
{
    constexpr auto timestamp_pattern = "####-##-##T##-##-##Z-"sv;
    if (!name.ends_with(".txt"sv))
        return false;
    auto suffix = name;
    if (name.length() > timestamp_pattern.length()) {
        bool has_timestamp = true;
        for (size_t i = 0; i < timestamp_pattern.length(); ++i) {
            if (timestamp_pattern[i] == '#' ? !is_ascii_digit(name[i]) : name[i] != timestamp_pattern[i]) {
                has_timestamp = false;
                break;
            }
        }
        if (has_timestamp)
            suffix = name.substring_view(timestamp_pattern.length());
    }
    for (auto type : { ProcessType::Browser, ProcessType::WebContent, ProcessType::WebWorker,
             ProcessType::RequestServer, ProcessType::ImageDecoder, ProcessType::MediaServer, ProcessType::Compositor,
             ProcessType::WasmCompiler }) {
        auto prefix = ByteString::formatted("{}-", process_name_from_type(type));
        if (!suffix.starts_with(prefix) || suffix.length() != prefix.length() + 10)
            continue;
        // Only the characters the store names files with, so a name never reaches outside its directory.
        return all_of(suffix.substring_view(prefix.length(), 6), is_ascii_alphanumeric);
    }
    return false;
}

#if !defined(AK_OS_WINDOWS)

static constexpr auto pending_prefix = "Browser-"sv;
static constexpr auto pending_suffix = ".pending"sv;
static constexpr size_t retained_report_count = 20;

static NeverDestroyed<ByteString> s_browser_pending_path;
static NeverDestroyed<OwnPtr<CrashReport>> s_browser_crash_report;

static ErrorOr<Core::Directory> open_report_directory(ByteString const& path)
{
    auto directory = TRY(Core::Directory::create(path, Core::Directory::CreateDirectories::Yes, 0700));
    auto status = TRY(directory.stat());
    if (status.st_uid != getuid())
        return Error::from_string_literal("Crash report directory is not owned by the current user");
    TRY(Core::System::fchmod(directory.fd(), 0700));
    return directory;
}

static bool is_owned_regular_file(struct stat const& status)
{
    return S_ISREG(status.st_mode) && status.st_uid == getuid();
}

static bool is_owned_regular_file_at(Core::Directory const& directory, ByteString const& name, struct stat& status)
{
    auto result = Core::System::fstatat(directory.fd(), name, AT_SYMLINK_NOFOLLOW);
    if (result.is_error())
        return false;
    status = result.release_value();
    return is_owned_regular_file(status);
}

// Creates a file that did not exist yet, named with random characters between the prefix and suffix. It is created
// relative to the directory's descriptor, so it lands in the directory that was checked whatever happens to the path.
static ErrorOr<NonnullOwnPtr<Core::File>> create_unique_file(Core::Directory const& directory, StringView prefix,
    StringView suffix, ByteString& name)
{
    static constexpr auto characters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"sv;
    static constexpr size_t random_length = 6;
    static constexpr size_t attempts = 100;

    for (size_t attempt = 0; attempt < attempts; ++attempt) {
        StringBuilder builder;
        builder.append(prefix);
        for (size_t i = 0; i < random_length; ++i)
            builder.append(characters[get_random_uniform(characters.length())]);
        builder.append(suffix);
        auto candidate = builder.to_byte_string();

        auto file = directory.open(candidate,
            Core::File::OpenMode::ReadWrite | Core::File::OpenMode::MustBeNew | Core::File::OpenMode::NoFollow, 0600);
        if (file.is_error() && file.error().code() == EEXIST)
            continue;
        name = move(candidate);
        return file;
    }
    return Error::from_errno(EEXIST);
}

static timespec modified_time(struct stat const& status)
{
#    if defined(AK_OS_MACOS)
    return status.st_mtimespec;
#    else
    return status.st_mtim;
#    endif
}

// Bound disk use. Unrelated files and symlinks in this directory are ignored.
static void apply_retention(Core::Directory const& directory)
{
    struct RetainedReport {
        ByteString name;
        timespec modified;
    };

    Vector<RetainedReport> reports;
    Core::DirIterator iterator(directory.path().string(), Core::DirIterator::SkipDots);
    while (iterator.has_next()) {
        auto name = iterator.next_path();
        if (!CrashReportStore::is_saved_report_name(name))
            continue;
        struct stat status {};
        if (!is_owned_regular_file_at(directory, name, status))
            continue;
        reports.append({ move(name), modified_time(status) });
    }

    quick_sort(reports, [](auto const& a, auto const& b) {
        if (a.modified.tv_sec != b.modified.tv_sec)
            return a.modified.tv_sec < b.modified.tv_sec;
        return a.modified.tv_nsec < b.modified.tv_nsec;
    });
    for (size_t i = 0; i + retained_report_count < reports.size(); ++i)
        (void)unlinkat(directory.fd(), reports[i].name.characters(), 0);
}

// Writes report text under a name derived from the time of the crash, then drops the oldest reports
// beyond the retention limit.
ErrorOr<void> CrashReportStore::store_report(ProcessType process_type, StringView text, UnixDateTime crashed_at) const
{
    auto directory = TRY(open_report_directory(m_directory));

    auto seconds = static_cast<time_t>(crashed_at.truncated_seconds_since_epoch());
    tm utc_time {};
    if (!gmtime_r(&seconds, &utc_time))
        return Error::from_errno(errno);
    Array<char, 21> timestamp {};
    if (strftime(timestamp.data(), timestamp.size(), "%Y-%m-%dT%H-%M-%SZ", &utc_time) == 0)
        return Error::from_string_literal("Could not format crash report timestamp");

    auto prefix = ByteString::formatted("{}-{}-", timestamp.data(), process_name_from_type(process_type));
    ByteString name;
    auto report = TRY(create_unique_file(directory, prefix, ".txt"sv, name));

    ArmedScopeGuard remove_incomplete_report = [&] { (void)unlinkat(directory.fd(), name.characters(), 0); };
    TRY(report->write_until_depleted(text.bytes()));
    if (fsync(report->fd()) < 0)
        return Error::from_errno(errno);
    // The new name is only durable once the directory holding it is.
    if (fsync(directory.fd()) < 0)
        return Error::from_errno(errno);
    remove_incomplete_report.disarm();

    apply_retention(directory);
    return {};
}

// Format the signal-safe records left by browsers that are no longer running. A report keeps the
// time of the crash, which may be much earlier than the launch that recovers it.
ErrorOr<size_t> CrashReportStore::recover_pending_reports() const
{
    auto directory = TRY(open_report_directory(m_directory));
    size_t recovered_count = 0;

    Core::DirIterator iterator(directory.path().string(), Core::DirIterator::SkipDots);
    while (iterator.has_next()) {
        auto name = iterator.next_path();
        if (!name.starts_with(pending_prefix) || !name.ends_with(pending_suffix))
            continue;

        auto file = directory.open(name,
            Core::File::OpenMode::ReadWrite | Core::File::OpenMode::DontCreate | Core::File::OpenMode::NoFollow);
        if (file.is_error())
            continue;
        auto fd = file.value()->fd();

        // A running browser holds this lock for its lifetime, so taking it means the owner is gone.
        // Process IDs are recycled, which makes them an unreliable way to tell.
        if (flock(fd, LOCK_EX | LOCK_NB) != 0)
            continue;

        auto status = file.value()->stat();
        if (status.is_error() || !is_owned_regular_file(status.value()))
            continue;

        ReportHeader header {};
        if (pread(fd, &header, sizeof(header), 0) != sizeof(header) || header.magic != report_magic
            || !header.signal) {
            (void)unlinkat(directory.fd(), name.characters(), 0);
            continue;
        }

        // The file was last written as the browser died, so this is when the crash happened, which
        // can be much earlier than the launch that recovers it.
        auto crashed_at = UnixDateTime::from_unix_timespec(modified_time(status.value()));

        CrashReport recovered(file.release_value(), ProcessType::Browser);
        if (auto result = recovered.save(header.signal, m_directory, crashed_at); result.is_error()) {
            warnln("Could not recover Browser crash report: {}", result.error());
            continue;
        }
        (void)unlinkat(directory.fd(), name.characters(), 0);
        ++recovered_count;
    }
    return recovered_count;
}

static void remove_clean_browser_report()
{
    if (!s_browser_pending_path->is_empty())
        (void)unlink(s_browser_pending_path->characters());
}

ErrorOr<void> CrashReportStore::initialize_browser_crash_handler()
{
    auto directory = TRY(open_report_directory(m_directory));

    // A browser cannot format its own fatal signal, so earlier ones are recovered before this
    // process installs a handler of its own. One unreadable file must not prevent that.
    if (auto result = recover_pending_reports(); result.is_error())
        warnln("Could not recover browser crash reports: {}", result.error());

    ByteString pending_name;
    auto file = TRY(create_unique_file(directory, pending_prefix, pending_suffix, pending_name));
    *s_browser_pending_path = directory.path().append(pending_name).string();

    // Held until this process exits. Recovery takes it to tell a crashed browser from a live one.
    auto fd = file->fd();
    if (flock(fd, LOCK_EX | LOCK_NB) != 0)
        return Error::from_errno(errno);

    *s_browser_crash_report = make<CrashReport>(move(file), ProcessType::Browser);
    if (auto result = (*s_browser_crash_report)->record_build_description(CrashReport::describe_current_build(ProcessType::Browser)); result.is_error())
        warnln("Could not record the build for Browser crash reports: {}", result.error());
    TRY(Core::CrashHandler::initialize(fd));
    if (atexit(remove_clean_browser_report) != 0)
        return Error::from_string_literal("Could not register browser crash report cleanup");
    return {};
}

ErrorOr<void> CrashReportStore::show_directory() const
{
    TRY(open_report_directory(m_directory));
    Vector<ByteString> arguments { m_directory };
#    if defined(AK_OS_MACOS)
    TRY(Core::Process::spawn("/usr/bin/open"sv, arguments));
#    else
    TRY(Core::Process::spawn({
        .executable = "xdg-open"sv,
        .search_for_executable_in_path = true,
        .arguments = arguments,
    }));
#    endif
    return {};
}

#else

ErrorOr<void> CrashReportStore::store_report(ProcessType, StringView, UnixDateTime) const
{
    return Error::from_string_literal("Crash reports are not supported on this platform yet");
}

ErrorOr<size_t> CrashReportStore::recover_pending_reports() const { return 0; }

ErrorOr<void> CrashReportStore::initialize_browser_crash_handler()
{
    return Error::from_string_literal("Crash reports are not supported on this platform yet");
}

ErrorOr<void> CrashReportStore::show_directory() const
{
    return Error::from_string_literal("Crash reports are not supported on this platform yet");
}

#endif

}

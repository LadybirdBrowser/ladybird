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

static constexpr auto seen_directory_name = "Seen"sv;
static constexpr auto pending_prefix = "Browser-"sv;
static constexpr auto pending_suffix = ".pending"sv;
static constexpr off_t maximum_report_size = 1048576;
static constexpr size_t retained_report_count = 20;

static NeverDestroyed<ByteString> s_browser_pending_path;
static NeverDestroyed<OwnPtr<CrashReport>> s_browser_crash_report;

static ErrorOr<Core::Directory> open_report_directory(ByteString const& path,
    Core::Directory::CreateDirectories create_directories = Core::Directory::CreateDirectories::Yes)
{
    auto directory = TRY(Core::Directory::create(path, create_directories, 0700));
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

static ErrorOr<ByteString> read_owned_file(Core::Directory const& directory, ByteString const& name)
{
    auto file = TRY(directory.open(name, Core::File::OpenMode::Read | Core::File::OpenMode::NoFollow));
    auto status = TRY(file->stat());
    if (!is_owned_regular_file(status) || status.st_size > maximum_report_size)
        return Error::from_string_literal("Invalid crash report file");
    return ByteString::copy(TRY(file->read_until_eof()));
}

// The user has been asked about the reports in here, so every report left at the top level still awaits an answer.
static ErrorOr<Core::Directory> open_seen_directory(Core::Directory const& directory)
{
    return open_report_directory(directory.path().append(seen_directory_name).string());
}

// The seen reports get their directory once the first of them is marked, so until then there are none.
static ErrorOr<Optional<Core::Directory>> open_seen_directory_if_exists(Core::Directory const& directory)
{
    auto seen_directory = open_report_directory(directory.path().append(seen_directory_name).string(),
        Core::Directory::CreateDirectories::No);
    if (seen_directory.is_error() && seen_directory.error().code() == ENOENT)
        return OptionalNone {};
    return TRY(move(seen_directory));
}

// Moving is atomic, so a report is always in exactly one of the two directories. Moving it where it already is succeeds.
static ErrorOr<void> move_report(Core::Directory const& from, Core::Directory const& to, ByteString const& name)
{
    struct stat status {};
    if (is_owned_regular_file_at(from, name, status)) {
        TRY(Core::System::renameat(from.fd(), name, to.fd(), name));
        // The move is only durable once both directories are, or a report could be offered again.
        if (fsync(to.fd()) < 0 || fsync(from.fd()) < 0)
            return Error::from_errno(errno);
        return {};
    }
    if (is_owned_regular_file_at(to, name, status))
        return {};
    return Error::from_string_literal("Crash report no longer exists");
}

static timespec modified_time(struct stat const& status)
{
#    if defined(AK_OS_MACOS)
    return status.st_mtimespec;
#    else
    return status.st_mtim;
#    endif
}

// Bound disk use by the reports kept for reference. Unrelated files and symlinks are ignored.
static void apply_retention(Core::Directory const& seen_directory, ByteString const& offered_name)
{
    struct RetainedReport {
        ByteString name;
        timespec modified;
    };

    Vector<RetainedReport> reports;
    Core::DirIterator iterator(seen_directory.path().string(), Core::DirIterator::SkipDots);
    while (iterator.has_next()) {
        auto name = iterator.next_path();
        if (!CrashReportStore::is_saved_report_name(name))
            continue;
        struct stat status {};
        if (!is_owned_regular_file_at(seen_directory, name, status))
            continue;
        reports.append({ move(name), modified_time(status) });
    }

    quick_sort(reports, [](auto const& a, auto const& b) {
        if (a.modified.tv_sec != b.modified.tv_sec)
            return a.modified.tv_sec < b.modified.tv_sec;
        return a.modified.tv_nsec < b.modified.tv_nsec;
    });
    // The report just offered is kept however old it is, so it can still be read and sent. An older one goes instead.
    auto excess_count = reports.size() > retained_report_count ? reports.size() - retained_report_count : 0;
    for (auto const& report : reports) {
        if (excess_count == 0)
            break;
        if (report.name == offered_name)
            continue;
        (void)unlinkat(seen_directory.fd(), report.name.characters(), 0);
        --excess_count;
    }
}

ErrorOr<CrashReportStore::SavedReport> CrashReportStore::saved_report(ByteString const& name) const
{
    if (!is_saved_report_name(name))
        return Error::from_string_literal("Invalid crash report name");
    auto directory = TRY(open_report_directory(m_directory));
    auto text = read_owned_file(directory, name);
    if (text.is_error() && text.error().code() == ENOENT) {
        if (auto seen_directory = TRY(open_seen_directory_if_exists(directory)); seen_directory.has_value())
            text = read_owned_file(*seen_directory, name);
    }
    return SavedReport { name, TRY(move(text)) };
}

ErrorOr<Vector<ByteString>> CrashReportStore::pending_report_names() const
{
    // Runs on every launch, so this only lists names instead of reading every report.
    auto directory = TRY(open_report_directory(m_directory));

    Vector<ByteString> names;
    Core::DirIterator iterator(directory.path().string(), Core::DirIterator::SkipDots);
    while (iterator.has_next()) {
        auto name = iterator.next_path();
        if (is_saved_report_name(name))
            names.append(move(name));
    }
    // Names begin with the crash time, so this orders the most recent crash first.
    quick_sort(names, [](auto const& a, auto const& b) { return a > b; });
    return names;
}

bool CrashReportStore::has_pending_reports() const
{
    auto names = pending_report_names();
    return !names.is_error() && !names.value().is_empty();
}

ErrorOr<void> CrashReportStore::mark_seen(ByteString const& name) const
{
    if (!is_saved_report_name(name))
        return Error::from_string_literal("Invalid crash report name");
    auto directory = TRY(open_report_directory(m_directory));
    auto seen_directory = TRY(open_seen_directory(directory));
    TRY(move_report(directory, seen_directory, name));
    apply_retention(seen_directory, name);
    return {};
}

ErrorOr<void> CrashReportStore::remove_sent_report(ByteString const& name) const
{
    if (!is_saved_report_name(name))
        return Error::from_string_literal("Invalid crash report name");
    auto remove_from = [&name](Core::Directory const& from) -> ErrorOr<void> {
        if (unlinkat(from.fd(), name.characters(), 0) < 0) {
            if (errno == ENOENT)
                return {};
            return Error::from_errno(errno);
        }
        // The removal is only durable once the directory is, or a sent report could be offered and sent again.
        if (fsync(from.fd()) < 0)
            return Error::from_errno(errno);
        return {};
    };

    auto directory = TRY(open_report_directory(m_directory));
    TRY(remove_from(directory));
    if (auto seen_directory = TRY(open_seen_directory_if_exists(directory)); seen_directory.has_value())
        TRY(remove_from(*seen_directory));
    return {};
}

// Writes report text under a name derived from the time of the crash and returns that name.
ErrorOr<ByteString> CrashReportStore::store_report(ProcessType process_type, StringView text, UnixDateTime crashed_at) const
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
    return name;
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

// Opens a file or directory in whatever the user's system opens it with.
static ErrorOr<void> open_with_system_handler(ByteString const& path)
{
    Vector<ByteString> arguments { path };
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

ErrorOr<void> CrashReportStore::show_directory() const
{
    TRY(open_report_directory(m_directory));
    return open_with_system_handler(m_directory);
}

ErrorOr<void> CrashReportStore::show_report(ByteString const& name) const
{
    if (!is_saved_report_name(name))
        return Error::from_string_literal("Invalid crash report name");
    auto directory = TRY(open_report_directory(m_directory));
    struct stat status {};
    if (is_owned_regular_file_at(directory, name, status))
        return open_with_system_handler(directory.path().append(name).string());
    if (auto seen_directory = TRY(open_seen_directory_if_exists(directory)); seen_directory.has_value()) {
        if (is_owned_regular_file_at(*seen_directory, name, status))
            return open_with_system_handler(seen_directory->path().append(name).string());
    }
    return Error::from_string_literal("Crash report no longer exists");
}

#else

ErrorOr<CrashReportStore::SavedReport> CrashReportStore::saved_report(ByteString const&) const
{
    return Error::from_string_literal("Crash reports are not supported on this platform yet");
}

ErrorOr<Vector<ByteString>> CrashReportStore::pending_report_names() const { return Vector<ByteString> {}; }
bool CrashReportStore::has_pending_reports() const { return false; }
ErrorOr<void> CrashReportStore::mark_seen(ByteString const&) const { return {}; }
ErrorOr<void> CrashReportStore::remove_sent_report(ByteString const&) const { return {}; }

ErrorOr<ByteString> CrashReportStore::store_report(ProcessType, StringView, UnixDateTime) const
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

ErrorOr<void> CrashReportStore::show_report(ByteString const&) const
{
    return Error::from_string_literal("Crash reports are not supported on this platform yet");
}

#endif

}

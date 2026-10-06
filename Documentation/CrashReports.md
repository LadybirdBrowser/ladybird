# Crash reports

On macOS and Linux, Ladybird saves local text reports when the browser or a
helper process crashes: WebContent, WebWorker, RequestServer, ImageDecoder,
Compositor and WasmCompiler. Reports are stored in
`~/Library/Application Support/Ladybird/CrashReports/` on macOS and
`~/.local/share/Ladybird/CrashReports/` on Linux, or under
`$XDG_DATA_HOME/Ladybird/CrashReports/` if that variable is set.
The directory is private to the current user, and report files are read-only,
with mode `0400`.
Reports awaiting review are always kept; of the reports that have already been
offered, the newest 20 are kept. Filenames start with a UTC date and time,
for example
`2026-09-06T12-34-56Z-WebContent-a1B2c3.txt`, so they sort chronologically.
Hyphens in the time keep filenames compatible with Windows; the random suffix
avoids collisions. Retention includes reports saved with the older filenames.
Nothing is uploaded automatically.

Once the report of a WebContent crash is saved, the crash screen shows a review
of it, with sending it as the main action and **Reload page** next to it. After
the report is answered, reloading is what the screen offers. Any other crash,
such as one of the Compositor or RequestServer, shows a popover below the menu
button as soon as its report is saved. A newer crash takes the place of a
popover still on screen, and the popover closes when its tab goes out of view.
It offers to review the report on the same crash screen in a new blank tab,
which **Close tab** closes. A report on its way keeps being sent after the
screen that showed its review is closed or lost. A browser-process report is
recovered on the next launch, which shows the same popover for the newest report
still awaiting review. While no browser window is active, the popover waits
until one is. Browsers driven by WebDriver never ask. Ladybird automatically
offers each report at most once; leaving the crash screen or the popover without
answering keeps the report on the device without offering it again on a later
launch. A report of a crash from more than 14 days ago, or from before October
2, 2026 20:00 UTC, is never offered, but stays in the folder. Reports that have
been offered move into a `Seen/` subdirectory, where the newest 20 are kept for
reference. **Settings > Advanced > Crash reports > Open folder** remains
available even when nothing has crashed. Reload restores the failed page without
adding a crash-screen history entry; Back and Forward continue to use the
original session history. The crash screen is native browser UI, so it does not
depend on a web content process.

The review asks what the user was doing and lets them choose whether to send the
report. Report details lists its main fields, such as the failure, signal,
version and commit, and opens the full report, exactly as it would be attached,
in the system's text viewer. Submissions omit the website URL by default; when
the crashed page had one, the user can explicitly include it and edit it first.
Ladybird does not collect contact information. Network errors, timeouts, rate
limits and server errors are retried a few times, honoring the server's
`Retry-After`; a report the server rejects is not. A report that changed on disk
after it was reviewed is not sent. A successful submission removes the local
copy. A report that is declined or could not be sent stays on the device, but is
not offered again.

Reports and filenames identify the process type. Build information includes the
full Git commit, tracked-source modification state, C++ compiler identity and
version, macOS SDK version when applicable, CMake build options, and flags from
the helper's compilation command. Include/output paths, string-valued defines
and arbitrary compiler arguments are omitted. The metadata refreshes on
incremental builds and does not require Git at runtime. Source archives without
Git metadata report an unknown revision; local source modifications and
`-march=native` builds still require the corresponding source changes and
build-machine target to reproduce.

Reports also contain the time of the crash, the browser version, platform,
architecture, numeric kernel release, build configuration, process uptime when
available, termination signal or exit code, signal code when available, and a
bounded native stack. Stack frames identify their binaries by Mach-O UUID on
macOS or ELF build ID on Linux and contain object addresses with the load
relocation removed. When a binary is also loaded in the surviving browser, its
nearest available native symbol and the offset from that symbol are included.
Binary IDs identify builds, not users or devices.

Saved crash diagnostics do not collect page URLs, titles, content, JavaScript
stacks, cookies, network requests, console output, stderr, command lines,
environment variables, usernames, hostnames, installation paths, absolute
source paths, general register values, or memory dumps. Native symbol names
containing paths or non-printable characters are omitted. This also applies to
crashes in private windows.

Fatal `VERIFY` and `ASSERT` failures include their compile-time expression and
source location. Locations inside the checkout are repository-relative; external
locations include only the filename and line. Assertion text is bounded and does
not include evaluated operands or runtime page data. It is saved before terminal
formatting and backtrace generation, and remains available if those fail.

## Architecture

After a WebContent crash, the browser displays a native crash screen and retains
the failed URL, title and committed history entry. The replacement WebContent
process remains dormant until the user chooses a recovery action. The crash
screen provides reload and report review actions directly in the browser
process. LibWebView's `CrashReportReview` prepares a report for display and
validates the user's choices, and `CrashReportSubmission` sends it to the report
server; the Qt UI only presents them.

The browser creates an unlinked temporary file before spawning each helper and
passes a descriptor to the child. The child cannot access the report directory.
It snapshots native executable ranges and binary IDs before sandboxing. The
POSIX signal handler writes fixed-size records to the descriptor, starting with
the termination reason. It walks a bounded frame-pointer chain using Mach reads
on macOS and a preopened `/proc/self/mem` descriptor on Linux. Unreadable memory
ends the walk. Linux also has a bounded stack-scan fallback for code without
frame pointers. The handler does not allocate, format strings, acquire
application locks, or symbolize the stack. Signal termination is preserved so
the operating system can still handle the crash normally.

After process exit, the browser reads a bounded number of records and formats
the report. It never copies arbitrary child-process text into the report. Clean
exits, SIGTERM and SIGKILL do not produce reports. Other abnormal exits still
produce a minimal report when capture was unavailable. Helpers launched by a
test-mode browser do not produce automatic reports.

The browser process has no parent process to finish its report, so Ladybird
recovers its signal-safe crash data on the next launch. Clean exits and exits
without a captured fatal signal do not produce a browser report. The record also
keeps a description of the browser's build, so a report recovered after an
update describes the build that crashed. A record without one says so instead of
describing the launch that recovers it.

The capture implementation and bounded record format live in LibCore, so all
helpers can install the handler before sandboxing without linking browser UI
code. Browser-owned report formatting and storage remain in LibWebView. Windows
capture is not implemented yet; it will need native binary IDs, stack capture,
storage and exit-status handling.

## Limitations and symbolication

Only the crashing thread is captured. Stacks can be partial due to corruption,
missing frame pointers, JIT code, or modules loaded after handler
initialization. An alternate signal stack protects main-thread stack overflow;
stack overflow on other threads may only produce a minimal report. Early startup
crashes and other exits that bypass the handler also produce minimal
reports for helpers. Helper reports require the browser to survive long enough
to format them. Browser crashes before the handler is installed, or exits that
bypass its signal handler, may not produce a report. Disk errors can prevent
saving; they are reported to stderr.

Keep the binaries and debug symbols for distributed builds. A binary ID and object
address remain useful even when symbols were stripped from the user's install.
On macOS, use `dwarfdump --uuid <binary>` to match the report's build ID
(ignoring hyphens and case), then `atos -arch arm64 -o <binary>
<object-address>` to resolve a frame with the matching binary and dSYM. Use
`x86_64` for Intel reports. Report addresses are already unslid, so do not add
the user's ASLR slide. The report itself contains no local binary paths.

On Linux, use `readelf -n <binary>` to match the ELF build ID, then
`addr2line -f -C -e <binary> <object-address>` with matching debug symbols.

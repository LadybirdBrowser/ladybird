# Crash reports

On macOS and Linux, the browser automatically saves local text reports when a
helper process crashes: WebContent, WebWorker, RequestServer, ImageDecoder,
Compositor and WasmCompiler. Reports are stored in
`~/Library/Application Support/Ladybird/CrashReports/` on macOS and
`~/.local/share/Ladybird/CrashReports/` on Linux, or under
`$XDG_DATA_HOME/Ladybird/CrashReports/` if that variable is set.
The directory is private to the current user, and report files have mode `0600`.
The newest 20 reports across all helper types are kept. Filenames start with a
UTC date and time, for example
`2026-09-06T12-34-56Z-WebContent-a1B2c3.txt`, so they sort chronologically.
Hyphens in the time keep filenames compatible with Windows; the random suffix
avoids collisions. Retention includes reports saved with the older filenames.
Nothing is uploaded automatically.

The crash screen provides **Reload Page** and **View crash reports** actions.
**Settings > Advanced > Crash reports > Open folder** is available even when no
tab has crashed or no reports have been saved yet. Reload restores the failed
page without adding a crash-screen history entry; Back and Forward continue to
use the original session history. The crash overlay is native browser UI, so
displaying it does not require the replacement renderer to load a crash document.

Reports and filenames identify the helper type. Build information includes the
full Git commit, tracked-source modification state, C++ compiler identity and
version, macOS SDK version when applicable, CMake build options, and flags from
the helper's compilation command. Include/output paths, string-valued defines
and arbitrary compiler arguments are omitted. The metadata refreshes on
incremental builds and does not require Git at runtime. Source archives without
Git metadata report an unknown revision; local source modifications and
`-march=native` builds still require the corresponding source changes and
build-machine target to reproduce.

Reports also contain the browser version, platform, architecture, numeric kernel
release, build configuration, process uptime, termination signal or exit code,
signal code when available, and a bounded native stack. Stack frames identify
their binaries by Mach-O UUID on macOS or ELF build ID on Linux and contain
object addresses with the load relocation removed. When a binary is also loaded
in the surviving browser, its nearest available native symbol and the offset
from that symbol are included. Binary IDs identify builds, not users or devices.

Reports do not collect page URLs, titles, content, JavaScript stacks, cookies,
network requests, console output, stderr, command lines, environment variables,
usernames, hostnames, installation paths, absolute source paths, general
register values, or memory dumps. Native symbol names containing paths or
non-printable characters are omitted. This also applies to crashes in private
windows.

Fatal `VERIFY` and `ASSERT` failures include their compile-time expression and
source location. Locations inside the checkout are repository-relative; external
locations include only the filename and line. Assertion text is bounded and does
not include evaluated operands or runtime page data. It is saved before terminal
formatting and backtrace generation, and remains available if those fail.

## Architecture

After a crash, the browser displays a native AppKit or Qt overlay and retains
the failed URL, title and committed history entry. The replacement WebContent
process remains dormant until the user chooses a recovery action. The overlay
provides reload and report-folder actions directly in the browser process.

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
exits and SIGTERM do not produce reports. Other abnormal exits still produce a
minimal report when capture was unavailable. Helpers launched by a test-mode
browser do not produce automatic reports.

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
crashes, SIGKILL, and other exits that bypass the handler also produce minimal
reports. A crash of the browser itself is not covered, and the browser must
survive to save the report. Disk errors can prevent saving; they are reported to
stderr.

Keep the binaries and debug symbols for distributed builds. A binary ID and object
address remain useful even when symbols were stripped from the user's install.
On macOS, use `dwarfdump --uuid <binary>` to match the report's build ID
(ignoring hyphens and case), then `atos -arch arm64 -o <binary>
<object-address>` to resolve a frame with the matching binary and dSYM. Use
`x86_64` for Intel reports. Report addresses are already unslid, so do not add
the user's ASLR slide. The report itself contains no local binary paths.

On Linux, use `readelf -n <binary>` to match the ELF build ID, then
`addr2line -f -C -e <binary> <object-address>` with matching debug symbols.

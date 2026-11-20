# Security Policy

## Reporting a vulnerability

Please report security issues in Ladybird privately, through GitHub’s private vulnerability reporting: On the
repository’s Security tab, choose “Report a vulnerability”, as the
[GitHub documentation](https://docs.github.com/en/code-security/security-advisories/guidance-on-reporting-and-writing-information-about-vulnerabilities/privately-reporting-a-security-vulnerability)
describes. Please don’t report security issues as public GitHub issues or pull requests, or in public channels such as
Discord: Ladybird now has public releases, so a vulnerability disclosed before it’s fixed can put people using Ladybird at
risk.

You may also report privately a security issue you’ve found in another web browser that you believe affects Ladybird.

A useful report names the Ladybird version or commit you tested, and your platform, and includes a minimal test case that
reproduces the issue. If you found the issue with a fuzzer, please include the input that triggers it, and the build you
ran it against. Please make sure the issue reproduces on the latest release, or on a current build of the master branch.

## Supported versions

Each Ladybird release has its own release branch. Security fixes go into the master branch, and are backported
to the latest release’s branch.

## What happens after you report

The maintainers triage each report. They may include regular contributors in the triage and the fix, as their expertise
requires. A report they accept gets a security advisory and a CVE identifier. Once the issue is fixed, the advisory is
published, unless other browser vendors or projects received the same report. In that case, it’s published when the
longest agreed disclosure timeline ends. Researchers who report security issues privately are credited in the advisory.

The maintainers reserve the right to reject reports that aren’t security issues, or that are outside the scope of
Ladybird. A report determined not to be a security issue should be filed as a
[GitHub issue](https://github.com/LadybirdBrowser/ladybird/issues/new?template=bug_report.yml) instead. If you don’t
re-report it, the maintainers may do so themselves.

Ladybird doesn’t offer bug bounties for security issues at this time.

## Scope

Examples of issues in scope:

- Memory-safety bugs (use-after-free, out-of-bounds reads and writes, type confusion) that web content can trigger, in
  any Ladybird process
- Escapes from the sandbox of the WebContent process or of another helper process
- Bypasses of the same-origin policy, site isolation, Content Security Policy, or iframe sandboxing
- Spoofing of the address bar or of other browser UI
- Flaws in validating IPC messages that let a compromised helper process act on the browser UI or on another process

Some security features of the web platform aren’t implemented in Ladybird yet. Reports about a missing feature (rather
than a flaw in an implemented one) may be redirected to regular issues. Vulnerabilities in websites themselves, such as
cross-site scripting or cross-site request forgery on a particular site, aren’t Ladybird issues.

A controlled crash of the sandboxed WebContent process that corrupts no memory (a failed assertion, a null-pointer
dereference, or a stack overflow that hits the guard page) isn’t a security issue. Please report it as a
[GitHub issue](https://github.com/LadybirdBrowser/ladybird/issues/new?template=bug_report.yml). A crash in any other
process, or one that involves memory corruption, is in scope.

The maintainers reserve the right to modify this scope as the project matures and as security issues are reported.

Significant portions of the browser depend on third-party libraries. Examples include image decoding, video decoding,
internationalization, 2D graphics, networking, and TLS. Security issues in these libraries should be reported to the
maintainers of the respective libraries. The maintainers of Ladybird will work with the maintainers of these libraries to
resolve the issue. If a security issue relates more to the integration of the library into Ladybird, report it the same
way as other security issues.

## Known limitations

Ladybird’s process isolation is still being hardened: The browser process and the other helper processes don’t yet
treat every request from the WebContent process as untrusted. While that work is underway, a flaw that can only be
exploited from an already-compromised WebContent process (one that needs a separate memory-corruption bug in WebContent
first) is treated as lower severity. Please still report it privately, the same way as other security issues.

## Responsible disclosure

The maintainers of Ladybird will work with security researchers to resolve security issues in a timely manner. A default
30-day disclosure timeline is in place for all security issues, but it may be extended if the maintainers and the reporter
agree that more time is needed to resolve the issue. The maintainers will keep the reporter informed of progress and
resolution steps throughout the process.

When a security issue is also reported to other browser vendors or open-source projects, the maintainers will work with
the longest disclosure timeline, so that all parties have enough time to resolve the issue.

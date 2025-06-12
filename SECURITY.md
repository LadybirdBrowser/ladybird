# Security Policy

Ladybird is unreleased software still in early development, and so bugs and vulnerabilities in its code can be safely
disclosed publicly. The preference is to report security issues as [GitHub issues](https://github.com/LadybirdBrowser/ladybird/issues/new?template=bug_report.yml).

However, private vulnerability reporting is also enabled on the repository. If you find a security issue in Ladybird,
or in another web browser that you believe affects Ladybird, you may report it privately to the maintainers
using the [process outlined in GitHub documentation](https://docs.github.com/en/code-security/security-advisories/working-with-repository-security-advisories/creating-a-repository-security-advisory).

Issues reported and accepted through the private reporting process will be disclosed publicly once they are resolved,
and given a security advisory identifier. The maintainers may include regular contributors in the disposition and resolution
process as their expertise requires. Researchers who report security issues privately will be credited in the advisory.

The maintainers reserve the right to reject reports that are not security issues, or that are not in the scope of Ladybird.
For issues that are determined to not be security issues, please report them as a [GitHub issue](https://github.com/LadybirdBrowser/ladybird/issues/new?template=bug_report.yml)
instead. If you choose not to re-report the issue as a generic issue, the maintainers may do so themselves.

Ladybird does not offer bug bounties for security issues at this time.

If your issue was found using a fuzzer, please check [oss-fuzz](https://bugs.chromium.org/p/oss-fuzz/issues/list?q=label:Proj-serenity) first to see if it has already been recorded.

## Scope of Security Issues

Many security features of the web platform are not yet implemented in Ladybird. Security reports regarding
incomplete features may be redirected to regular issues. The following are examples of issues that are not in scope
at this time:

- Cross-site request forgery
- Cross-site scripting
- Content Security Policy violations
- Cross-origin iframe sandboxing

The maintainers reserve the right to modify this list as the project matures and as security issues are reported.

Significant portions of the browser depend on third party libraries. Examples include image decoding, video decoding,
internationalization, and 2D graphics. Security issues in these libraries should be reported to the maintainers of the
respective libraries. The maintainers of Ladybird will work with the maintainers of these libraries to resolve the issue.
If a security issue relates more to the integration of the library into Ladybird, it should be reported via the same
methods as other security issues.

## Responsible Disclosure

The maintainers of Ladybird will work with security researchers to resolve security issues in a timely manner. A default
30-day disclosure timeline is in place for all security issues, but this may be extended if the maintainers and the reporter
agree that more time is needed to resolve the issue. The maintainers will keep the reporter informed of progress and
resolution steps throughout the process.

In the case that a security issue is also reported to other browser vendors or OSS projects, the maintainers will work
with the longest disclosure timeline to ensure that all parties have sufficient time to resolve the issue.

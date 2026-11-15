Fixed [#14618](https://github.com/LadybirdBrowser/ladybird/issues/14618), uncommitted.

Worktree: /Users/mike/workspace/worktrees/ladybird/gradient-border-image-source
Branch: gradient-border-image-source
Base: fb3487adfd5fb8413d453663b464e68085e1d5fd, fetched from origin/master before creating the branch.

The exact issue reduction reproduced the black gradient border on the base; its URL-image control painted orange. Chrome 157.0.8103.78 painted the gradient, as does the fixed build. No live-site load was needed.

The paint-facts gate and border-image painter accepted only URL image sources. The fix accepts generated images, including the selected image-set() option, resolves gradients against the border-image area, and uses the nine-piece geometry to stretch and repeat each source slice. Each tile records the whole gradient under a source-to-tile transform, preserving radial and conic geometry. Numeric gradient slices scale with effective zoom. Generated tiles have bounded raster dimensions and area.

Three WPTs were imported before implementation: border-image-image-type-003.htm, border-image-outset-003.html, and border-image-021.html. A fourth reference test covers all six gradient forms, transparent currentColor stops, zoomed length stops and slices, stretched borders, center fill, translation, and image-set(). Chrome matched the custom reference exactly and passed the imported references. All four failed on the base; the other 33 border-image tests passed. After the fix, all 37 passed. Intermediate tile rasterization introduces one-level color rounding in 2,542 pixels of the custom matrix; its tolerance permits a maximum difference of 1 in 4,000 pixels.

Verification:

- Full LibWeb: 19,262 passed, 114 skipped, no failures, crashes, or timeouts.
- CTest: All 94 entries passed. LibWeb was run separately; the standard wrapper excluded the macOS accessibility suite.
- All four new tests: 100 runs each at -j1 and -j16; no flaky results, failures, or timeouts.
- cargo fmt, Meta/lint-ci.sh, Clippy, and the custom Rust GC lints passed.
- The GC clang-plugin checker exited successfully; no C++ files changed, so it had no translation units to inspect.
- No Python or PNG files changed. The PNG-size lint passed.
- The patch passed a reverse-application check, and git diff --check passed.

Drafts and evidence:

- [Commit message](/tmp/codex/issue-14618-85wsmp38/commit-message.txt)
- [Implementation note](/tmp/codex/issue-14618-85wsmp38/implementation-note.txt)
- [Patch](/tmp/codex/issue-14618-85wsmp38/fix.patch)
- [Baseline results](/tmp/codex/issue-14618-85wsmp38/baseline-results/index.html)
- [Full LibWeb results](/tmp/codex/issue-14618-85wsmp38/full-libweb-results/index.html)
- [CTest log](/tmp/codex/issue-14618-85wsmp38/ctest.log)
- [Flakiness log](/tmp/codex/issue-14618-85wsmp38/flakiness.log)
- [Lint log](/tmp/codex/issue-14618-85wsmp38/lint.log)

No commits, pushes, or GitHub comments.

The Build directory was deleted after preserving the reports.

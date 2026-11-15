Review of [PR #14663](https://github.com/LadybirdBrowser/ladybird/pull/14663), [review 5904839841](https://github.com/LadybirdBrowser/ladybird/pull/14663#pullrequestreview-5904839841).

Worktree: /Users/mike/workspace/worktrees/ladybird/gradient-border-image-source
Branch: gradient-border-image-source
HEAD: fe7fb16fa93650080e6f49c64de0350ada2796e3

Status: Complete. The overlap fix and regression tests are uncommitted on the requested head. Nothing was committed, pushed, or posted to GitHub.

1. Gradient auto width: Reject for interoperability. Chrome 157.0.8103.78 produced identical screenshots for auto and explicit 30px with a 30px slice and 10px border. Explicit 10px differed. The CSS Backgrounds 3 auto definition supports the review literally, but all three inspected engine implementations use the slice. Draft reply: /tmp/codex/pr-14663-gawuvjud/auto-width-rejection.txt.

2. Overlapping source slices: Confirmed by Chrome and source inspection. Chrome matches the six-case reference exactly: 100% slices, 150% slices with fill, 75% slices with fill, horizontal-only overlap, vertical-only overlap, and oversized destination widths. The branch proportionally shrinks opposing source slices before deriving corner rectangles. The change clamps each source slice to its corresponding source dimension, removes source normalization, and retains destination-width shrinking. The per-piece painters already reject empty source rectangles. The fix clamps values in resolve_slice_px() and removes source-slice shrinking from resolve_border_image_geometry(); destination-width shrinking is unchanged.

Tests added:

- Tests/LibWeb/Ref/input/border-image-overlapping-gradient-slices.html and its reference. Chrome comparison: zero differing pixels.
- WPT css/css-backgrounds/border-image-slice-006.htm, reference, and green_color.png. Imported from local WPT checkout 68d964a5f292bc4b2c2eba2f21e2b916190b6ad2 after remote URLs returned 404. The uniform-color WPT controls empty edge regions but cannot detect cropped gradient corners, so the custom test is necessary. PNG optimized with optipng -strip all (91 bytes).

Engine evidence, behavior only:

- Blink: third_party/blink/renderer/core/paint/nine_piece_image_grid.cc, ComputeEdgeWidth(), ComputeEdgeSlice(), and NinePieceImageGrid constructor. Auto uses the image slice; each source slice is individually clamped, with only destination widths reduced.
- WebKit: Source/WebCore/rendering/NinePieceImagePainter.cpp, computeSlice(), computeSlices(), and paintNinePieceImage(). Auto uses the slice. Source slices are independently clamped; scaleSlicesIfNeeded() applies to destination slices.
- Gecko: layout/painting/nsCSSRenderingBorders.cpp, nsCSSBorderImageRenderer constructor. Each mSlice is clamped to imgDimension; Auto assigns mSlice to mWidths. Proportional overlap reduction applies to mWidths only.

Spec: https://drafts.csswg.org/css-backgrounds-3/#border-image-slice and #border-image-width.

Artifacts: /tmp/codex/pr-14663-gawuvjud contains standalone reductions, Chrome screenshots, comparison harness, and the rejection draft. No commits, pushes, or GitHub comments.

Verification:

- Both the unchanged baseline and fixed full builds passed.
- Failing first: The six-case overlap reference failed before the source change; the other 38 border-image tests passed. After the change, all 39 passed.
- Full LibWeb: 19,263 passed, 114 skipped, no assertion/snapshot failures, and one timeout in Text/input/css/aspect-ratio-min-max-transfer-matrix.html. That unrelated layout matrix passed all three serial reruns. Logs: full-tests.log and timeout-serial.log.
- CTest: 93/94 passed initially; RustWorkspace hit the default 1500-second timeout while seeded style-pipeline tests were still running. The complete RustWorkspace rerun passed in 1133 seconds with a 3600-second limit. Logs: ctest.log and ctest-rust-recheck.log. LibWeb ran separately; the shared wrapper excluded host macOS accessibility.
- Flakiness: All 18 discovered candidates passed 100 repetitions at both -j1 and -j18, with zero flaky, always-failing, or timed-out cases. Log: flakiness.log.
- cargo fmt --all --check, Meta/lint-ci.sh, and git diff --check passed.
- The GC-plugin checker exited successfully; there are no changed C++ translation units. No Python files changed.
- The imported PNG was optimized with optipng -strip all (91 bytes). check_png_sizes.sh exited successfully and excludes WPT imports by design.

All logs and screenshots are under /tmp/codex/pr-14663-gawuvjud. The Build directory has been deleted, as requested; the shared dependency cache is preserved.

Draft message: [commit-message.txt](/tmp/codex/pr-14663-gawuvjud/commit-message.txt)
Standalone implementation note: [implementation-note.txt](/tmp/codex/pr-14663-gawuvjud/implementation-note.txt)

CodeRabbit withdrew the auto-width finding in [its reply](https://github.com/LadybirdBrowser/ladybird/pull/14663#issuecomment-6885044306). No additional action is needed for that finding.

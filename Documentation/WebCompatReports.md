# Web compatibility reports

A user who finds a website that Ladybird renders or runs incorrectly can report
it with **Report Website Issue...** in the Help menu or in the page's context
menu. The action is available for http and https pages that have not crashed,
in private windows too.

When the report starts, the browser freezes a frame of the visible page and dims
it. The user drags out the area that shows the problem, presses Enter or **Use
Visible Page** to take the whole frame, or picks **No Screenshot** to go without
one; Escape or **Cancel** abandons the report. The report then opens in a new
**Report Website Issue** tab next to the page. It asks what is wrong with the
website, which is required, and previews the selected area, which the user can
remove or retake from the same frame. Clicking the preview shows the screenshot
over the whole tab: in its actual size when the tab has room for it, and
otherwise fitted to the tab, where a click shows it in its actual size to drag
around in.

Reports always include the page URL, the user agent, the viewport size in CSS
pixels, the device pixel ratio and zoom level, and the Ladybird version,
platform, architecture, git commit and build configuration. The page's layout
tree, stacking context tree and display list are dumped when the report starts.
They are only included when the user checks **Include layout and painting
information**, because they contain text from the page and can contain personal
information; dumps larger than 8 MiB are cut off and say so. These dumps only
cover the top-level document, not its iframes. **View full report** shows the
diagnostics text in the tab, exactly as it would be sent.

The report is sent to the same report server as [crash reports](CrashReports.md),
as a `web_compat` report with a `webcompat-diagnostics.txt` attachment and, when
included, a `screenshot.png`. Screenshots are shrunk until they fit within 8
million pixels and 8 MiB. Sending retries transient failures, and **Try sending
again** after a failure resends the same submission. Until the report is sent,
**Cancel** stops sending it and returns to the review. Nothing about a web
compatibility report is written to disk: closing its tab discards an unsent
report, and a report on its way keeps being sent after the tab closes.

## Architecture

`WebCompatReport` in LibWebView describes the page from its view, requests its
dumps in one page info request, builds the diagnostics text and the manifest,
and sends them through `ReportSubmission`, which crash reports share.
The Qt UI takes the visible frame from the view's backing store, or from its
framebuffer when the backing store is GPU-shared, and presents
`ScreenshotSelectionOverlay` over the page, then `WebCompatReportReview` in a
blank tab, the way crash reports with no crash screen of their own are reviewed.
`ScreenshotViewer` shows the screenshot in full over that tab.

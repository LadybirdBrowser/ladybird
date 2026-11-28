# Accessibility

We want Ladybird to work for everyone, including people who use a screen reader, a magnifier, or the keyboard alone. This page tells you what works today, what doesn’t yet, and how to report a barrier you run into.

## Screen readers

- **macOS:** Ladybird works with VoiceOver.

- **Linux:** Ladybird works with Orca. Each time it launches, Ladybird makes sure Orca has a script that improves how Orca reads web pages, by keeping a marked block in `orca-customizations.py` in Orca’s data directory (`~/.local/share/orca/` unless `XDG_DATA_HOME` is set); the rest of that file is left alone. Orca picks up the script the next time it starts, so restart Orca if it was running when Ladybird first launched. The first “Say All” after a page loads can take a moment to start.

- **Windows:** Ladybird doesn’t yet expose web content to screen readers on Windows.

How Ladybird works with each screen reader, and the limitations we know about, are described in [Screen-reader support](Documentation/ScreenReaderSupport.md).

## Keyboard

- <kbd>Tab</kbd> and <kbd>Shift</kbd>+<kbd>Tab</kbd> move focus through the links, buttons, and form fields in a page, and the focused element shows a focus ring.

- <kbd>F6</kbd> moves focus between the address bar and the page.

- The usual browser shortcuts work, with <kbd>Ctrl</kbd> on Linux and <kbd>Command</kbd> on macOS:
  - <kbd>Ctrl</kbd>+<kbd>L</kbd> goes to the address bar.
  - <kbd>Ctrl</kbd>+<kbd>T</kbd> and <kbd>Ctrl</kbd>+<kbd>W</kbd> open and close a tab (<kbd>Ctrl</kbd>+<kbd>W</kbd> doesn’t close a pinned tab).
  - <kbd>Ctrl</kbd>+<kbd>Page Down</kbd> and <kbd>Ctrl</kbd>+<kbd>Page Up</kbd> switch tabs.
  - <kbd>Ctrl</kbd>+<kbd>F</kbd> finds text in the page.
  - <kbd>Ctrl</kbd>+<kbd>Plus</kbd>, <kbd>Ctrl</kbd>+<kbd>Minus</kbd>, and <kbd>Ctrl</kbd>+<kbd>0</kbd> zoom in, zoom out, and reset the zoom.

These don’t work yet:

- The <kbd>Menu</kbd> key and <kbd>Shift</kbd>+<kbd>F10</kbd> don’t open a context menu for the focused element, so opening a link in a new window, for example, takes a mouse ([#16230](https://github.com/LadybirdBrowser/ladybird/issues/16230)).

- <kbd>Tab</kbd> moves focus only through the page, never into the browser’s own toolbar and tabs ([#16232](https://github.com/LadybirdBrowser/ladybird/issues/16232)).

- There’s no caret browsing (moving a text cursor through a page with the arrow keys).

## Display and motion

The **View** menu has settings for web content: **Zoom In** and **Zoom Out**, **Color Scheme** (light or dark), **Contrast**, and **Motion**. On macOS and Linux, **Motion** follows your operating system’s setting for reduced motion unless you choose otherwise; Ladybird doesn’t read that setting on Windows yet, so choose **Reduce** there if you want less motion. Pages can respond to these settings through the `prefers-color-scheme`, `prefers-contrast`, and `prefers-reduced-motion` media queries.

## Reporting an accessibility problem

If something in Ladybird stands in your way, please report it with the [bug report form](https://github.com/LadybirdBrowser/ladybird/issues/new?template=bug_report.yml). Accessibility problems are bugs like any other, and we track them with the `accessibility` label.

A useful report says:

- What you did, what you expected, and what happened instead.
- The page or the part of Ladybird where it happened.
- Your operating system, and the Ladybird version or commit.
- Which assistive technology you use, and its version.

Please don’t report security issues that way; see [SECURITY.md](SECURITY.md) instead.

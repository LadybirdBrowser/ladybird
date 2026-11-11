# Ladybird "Quiet" UI Design Guidelines

This document describes how Ladybird's browser UI looks, moves and reads, so that new UI
work stays consistent with what already exists. It covers the browser chrome (tab strip,
toolbar, sidebar), its menus and panels, and Ladybird's own pages (`about:` pages).

The reference design lives on the "Quiet" design canvas. When this document and the canvas
disagree, fix whichever one is wrong in the same change, so they never drift apart.

Contents:

1. [Principles](#1-principles)
2. [Platforms](#2-platforms)
3. [Layout and metrics](#3-layout-and-metrics)
4. [Typography](#4-typography)
5. [Color](#5-color)
6. [Shape and elevation](#6-shape-and-elevation)
7. [Icons and the logomark](#7-icons-and-the-logomark)
8. [Components](#8-components)
9. [Surfaces](#9-surfaces)
10. [Motion](#10-motion)
11. [Interaction and keyboard](#11-interaction-and-keyboard)
12. [Writing](#12-writing)
13. [Accessibility](#13-accessibility)

---

## 1. Principles

**Quiet chrome, loud content.** The page is what people came for. The chrome is neutral
grey and white, and color appears only where it means something.

**Respect attention.** Don't put text on screen that people will learn to ignore. No
explanatory captions on everyday surfaces, no "tips", no labels that repeat what an icon or
a heading already says. Text that appears once (the welcome page, a crash restore prompt)
can explain more; text that appears every day must earn its place.

**Accent means "you are here".** The system accent color is used for focus, selection and
progress, and nothing else. A bookmarked star is not accent-colored; a filled star is enough.

**One way to do a thing.** Information lives in one place. Content blocking lives in site
settings, not in a separate shield. Downloads have one button. If a new feature duplicates
an existing entry point, merge them instead.

**Nothing moves on its own.** Animation answers something the person did or a real state
change. Nothing animates on hover, nothing loops for decoration, and everything respects
reduced motion.

**Stable layout.** Switching tabs, loading a page or opening a menu never shifts other
elements. Favicons stay put when a tab becomes active; buttons swap icons in place.

**Never rely on color alone.** Every color signal has a second carrier: a shape, a glyph,
text or position.

---

## 2. Platforms

Ladybird follows each platform where the platform has a strong convention, and keeps its
own identity everywhere else.

| | macOS | Windows | Linux |
|---|---|---|---|
| Window controls | Traffic lights, top left | Minimize, maximize, close on the right, 46 px wide each | Follow the desktop: right by default, left if configured |
| Ladybird button | After the traffic lights | First item, 13 px from the left edge | First item on the side without window controls |
| UI font | SF Pro (`system-ui`) | Segoe UI Variable | The desktop's UI font (Adwaita Sans, Noto Sans, …) |
| Accent | System accent color | Windows accent color | Desktop accent where offered, otherwise blue |
| More contrast | Accessibility › Increase contrast | Contrast themes | The desktop's high-contrast theme |
| Materials | Opaque chrome, no vibrancy behind tabs | Opaque chrome | Opaque chrome |
| App menu | Menu bar; the Ladybird button opens the same items | Ladybird button (Alt+F) | Ladybird button |

Keyboard shortcuts use ⌘ on macOS and Ctrl elsewhere (see [section 11](#11-interaction-and-keyboard)).

---

## 3. Layout and metrics

All sizes are in logical pixels.

### Window

| Element | Size | Notes |
|---|---|---|
| Traffic lights inset | 12 px from the top, 12 px from the left (centered 18 px down; if the system draws larger lights, keep the center) | The same on both sides. Never change one without the other. |
| Tab strip | 35 px | Window-level items (traffic lights, Ladybird button, Linux close button) sit on one line 18 px from the top. The Ladybird button and the all-tabs button run from 5 to 31 px: their tops are level with the active tab's top, with 4 px below. Tab contents and the new tab button are centered on the tab row, 20 px from the top, the exact middle of the active tab. |
| Toolbar | 40 px | Directly below the tab strip, in the toolbar color. |
| Drag space | 44 px | Always kept free after the + button (before the window controls on Windows), so there is somewhere to grab and move the window even with many tabs open. |
| Bookmarks bar | 30 px | Optional, below the toolbar. Set to show on hover, it slides down over the page while the pointer is over the toolbar, the sidebar or the bar itself. |
| Window corner radius | 10 px (macOS) | Follows the platform elsewhere. |

### Tabs

| Element | Size | Notes |
|---|---|---|
| Inactive tab | No fill at rest | Content laid out in the same 30 px row as the active tab |
| Hovered tab | 30 px tall | The active tab's exact outline (full slot width, 8 px top radius, flush with the toolbar) in the `hover` fill, without the feet. Hover previews the shape the tab takes when selected; nothing inside it moves. Drawn under the active tab, so a neighboring foot overlaps it cleanly. |
| Active tab | 30 px tall | 8 px top radius plus an 8 px inverted "foot" on each side where it joins the toolbar. 5 px of strip shows above it. Its contents are vertically centered in it, which puts them on the shared center line. |
| End insets | Favicon 7 px from the left edge; × glyph 11 px from the right edge | Rule: each end item is as far from the tab's side as it is from the tab's top (the 16 px favicon sits 7 px down, the 8 px × glyph 11 px down). Padding: active 0 6 px 0 7 px; inactive 3 px inset + 0 3 px 0 4 px, so favicons and × never move when a tab becomes active. |
| Favicon to title | 8 px | |
| Close button | 18 px hover square, 7 px × (14 px glyph box); click area extends to the full tab height | The × is always visible on every tab except pinned tabs, in `muted`; its hover square appears on hover. Indicators like the speaker and the microphone sit to its left. |
| Tab width | 20 to 232 px | Tabs shrink until only the favicon is left, giving up their padding below 32 px, then the strip scrolls. A tab too narrow for its favicon beside its × shows only the favicon; the active one shows its × in the favicon's place. |
| Pinned tab | 44 px wide | Favicon only, always first. While the tab plays audio or uses the microphone, those indicators take the favicon's place. |
| New tab (+) | 26 px button, 14 px glyph | Follows the last tab directly, 8 px after it (just clear of the active tab's foot), centered on the tab row; with few tabs it does not move to the end of the strip. Click area at least 32 px. |
| Separators | 1 × 16 px | Between inactive tabs only, hidden next to hovered or active tabs |

### Toolbar

| Element | Size | Notes |
|---|---|---|
| Toolbar buttons | 32 × 28 px | 6 px radius, 18 px icons, 2 px apart |
| Address field | 28 px tall | 7 px radius, no border, 8 px after the reload button |
| Icons inside the field | 16 px | Site settings (left), zoom chip and star (right) |
| Downloads button | 32 × 28 px | Appears at the right end of the toolbar, 6 px after the field, only while there are downloads |

### Ladybird button

| Property | Value |
|---|---|
| Size | 46 × 26 px, 7 px radius |
| Content | 20 px logomark + 3 px gap + 10 px chevron, padding 0 6 px 0 7 px. Keep the chevron: it is the only thing that says this is the main menu (a bare logo reads as branding), and it tells the Ladybird button apart from the All tabs button next to it. The outline keeps it from reading as a pinned tab. |
| Rest | No fill, 1 px inside hairline: black 14% in light, white 16% in dark and private, 45% in increased contrast. Never a gray fill at rest (it reads as stuck hover) and never a colored ring (it reads as focus). |
| Hover | Hover fill (black 9% / white 12%) plus the hairline |
| Menu open | Pressed fill (black 12% / white 16%) plus the hairline |
| Clear space | 24 px from the traffic lights to the button. Then 6 px to the all-tabs button, and 24 px from that to the first tab's straight left edge (the active tab's foot reaches 8 px closer). On Windows and Linux the button comes first, with the same 6 and 24 px after it. |
| All-tabs button | 28 × 26 px, 16 px card-stack glyph (a wide rounded card with two shorter lines stacked above it, like a pile of pages; not overlapping squares, which read as Copy, and not a chevron, which would read as the Ladybird button's old dropdown) in the `icon` color, no outline. Not a magnifier: it sits next to the address field, which already means search; hover and pressed fills like other buttons. Always shown, in every tab layout, right after the Ladybird button. Opens the all-tabs panel (list with a filter field) directly below it, left-aligned to it (also ⇧⌘A). There is no separate overflow button at the end of the strip. |
| Purpose | Opens the Ladybird menu. Belongs to the window, not to a tab, so it lives in the tab strip (or the sidebar header). It sits in the same place, right after the traffic lights, in horizontal tabs and in both sidebar modes, so it never jumps when the layout changes. |

### Sidebar (vertical tabs)

| Element | Size |
|---|---|
| Sidebar width | 236 px |
| Tab rows | 30 px, 6 px radius. Selected: toolbar color with a soft shadow in light; white 15% fill only in dark and private, with no ring: on the dark sidebar the fill is enough, and stacked outlines (Ladybird button, focused field, row) make the column busy. The × on each row is lighter at rest (`#9C9CA3` light, `#7A7A81` dark) and takes the `muted` color on the hovered and selected rows, so a full list does not draw a dark stripe down the sidebar. It is always there; only its tone changes. |
| Collapsed rail | 52 px. Tiles are 36 × 32 with the favicon centered. Badges sit on the favicon's corners, 18 px round on a toolbar-colored backing with a hairline ring: × top left (on hover only), speaker bottom right (while playing or muted), microphone bottom left (while in use). The × and the speaker are buttons, and opposite corners keep them apart. The microphone is not a button; it sits 2 px from the speaker and touches the × while that shows on hover. |
| Header | Traffic lights, Ladybird button, all tabs, then empty space, then the hide-sidebar toggle at the sidebar's right edge. With the sidebar collapsed, the toolbar starts the same way: traffic lights, Ladybird button, all tabs, show-sidebar toggle. |
| Background | `sidebar` token: the strip color in light, the toolbar color in dark and private. The page must never sit directly against near-black chrome. |
| Sidebar on the right | Only the sidebar moves: tab list, rail and the hide/show toggle, which sits at the sidebar's inner edge (facing the page). The traffic lights, Ladybird button and All tabs button stay top left, at the start of the toolbar row, exactly as in collapsed mode. Rail badges keep their corners (× top left, speaker bottom right). Dragging a tab out of the sidebar goes sideways toward the page. On Windows, the sidebar header holds the minimize, maximize and close buttons, with the toggle to their left. Settings → Appearance → Layout shows "Sidebar on: Left / Right" below the layout cards, only while Tabs on the side is selected. |
| Everything in the sidebar | A third tab layout (Arc/Zen style): no toolbar row; the page runs to the top of the window. The sidebar holds, top to bottom: the top row (traffic lights, Ladybird button, then back, forward and reload at the right end, 26 × 28 each so they fit), the full-width address field directly below it, 4 px down (domain only while idle; the full URL when focused), pinned tabs, the tab list and new tab, and a bottom bar with the sidebar toggle and All tabs on the left and Feedback and Downloads on the right. This is the one layout where All tabs and the toggle leave the top row: keeping the top row to one quiet line, like Arc and Zen, is worth more here than matching the other layouts. Same `sidebar` color and opaque chrome as the normal sidebar: no inset page card, no gradients, no translucency. The page meets the sidebar at the `divider` line. Focus alone never grows the field: focused and empty (a new tab, startup, ⌘L on a blank tab) it stays sidebar width with its focus ring. It grows into the suggestions surface (see Suggestions), 600 px wide over the page, only once there are rows to show. If the text is cleared and no rows are left, it shrinks back. Esc, Return or a click outside also shrink it back to the sidebar field. The panel is never shown empty. Collapsing the sidebar falls back to the normal collapsed layout (toolbar row plus rail), except that the sidebar toggle stays at the bottom left of the rail, in the same place as in the open sidebar. In Settings → Appearance → Layout it is the third card, "Everything in the sidebar". On Windows and Linux, nothing holds the minimize, maximize and close buttons here, so they stay out of sight: when the pointer comes within 10 px of the window's top edge above where they go, they slide down over the page's top right corner in the `sidebar` color, with the `divider` line along their two edges on the page. The corner stays square, as they are a native surface that can't show the page through a rounded one. They slide away 300 ms after the pointer leaves them, with 16 px of leeway. A drag that ends up in the corner doesn't reveal them. |

### Popovers and pages

| Element | Value |
|---|---|
| Menus | 10 px radius, 6 px padding, 13 px text |
| Panels (site settings, downloads, zoom, …) | 12 px radius, 14 px padding |
| Popover offset | 4 px below the control it belongs to, aligned to its edge |
| Settings sidebar | 232 px, then content in a fixed 600 px column |
| Page content column | Centered; 560 to 640 px for forms, 860 px for lists |

---

## 4. Typography

Use the platform UI font. Never ship a custom UI font.

| Size | Use |
|---|---|
| 11 px | Keycaps, tiny badges |
| 12 px | Tab titles, secondary text, captions, small links |
| 13 px | Default UI text: menus, address field, sidebar rows, buttons, body text on pages |
| 15 px | Section headings inside panels and pages |
| 22 px | Page titles (Settings section names, History, Bookmarks, error pages, welcome) |
| 40 px | The "Ladybird" wordmark on About only |

Weights: 400 for almost everything, including active tab titles (the active tab is shown by
its background, not by bold text). 500 for primary button labels. 600 for headings and the
one emphasized element of a panel. Never use weight to show selection.

Numbers that change in place (zoom, counts, sizes, times) use tabular figures.

---

## 5. Color

### Chrome tokens

| Token | Use | Light | Dark | Increased contrast (light) |
|---|---|---|---|---|
| `strip` | Tab strip | `#DCDCE0` | `#141416` | `#CFCFD4` |
| `toolbar` | Toolbar, active tab | `#FFFFFF` | `#2E2E31` | `#FFFFFF` |
| `sidebar` | Vertical tabs sidebar and rail | `#DCDCE0` | `#2E2E31` | `#CFCFD4` |
| `field` | Address field fill | Black 8% | White 8% | White + 1 px `#6E6E73` border |
| `text` | Titles, URL domain | `#1D1D1F` | `#F2F2F5` | `#000000` |
| `tab-title` | Inactive tab titles | `#3A3A40` | `#ACACB4` | `#3A3A40` |
| `muted` | URL path, secondary text | `#5D5D64` | `#ACACB4` | `#3A3A40` |
| `icon` | Toolbar icons | `#3A3A40` | `#DCDCE1` | `#1D1D1F` |
| `disabled` | Disabled controls | `#B3B3B9` | `#5B5B61` | `#8E8E93` |
| `hover` | Hover fills | Black 6% | White 6% | Black 10% |
| `separator` | Between inactive tabs | `#BCBCC2` | `#3D3D42` | `#8E8E93` |
| `divider` | Chrome to page edge | `#D2D2D6` | `#0A0A0C` | `#6E6E73` |
| `warning` | Crashed tab icon | `#B25D00` | `#F2A33A` | `#8A4700` |
| `warning-fill` | Not secure, lookalike pills | `#FDE0B8` | `#4F3416` | `#FBD29C` |
| `warning-ink` | Text and icon on `warning-fill` | `#6E3300` | `#FFC46E` | `#5A2900` |

### Popover and page tokens

| Token | Use | Light | Dark |
|---|---|---|---|
| `pop` | Menus, panels | `#FBFBFC` | `#333336` |
| `page` | Ladybird pages, blank tab | `#FFFFFF` | `#1C1C1E` |
| `body` | Body text in panels | `#3A3A40` | `#DCDCE1` |
| `sep` | Dividers in panels | `#DCDCE0` | `#3D3D42` |
| `sep-soft` | Row dividers, list groups | `#ECECEF` | `#333337` |
| `ctl` | Secondary button and control fill | Black 6% | White 10% |
| `press` | Pressed or open state | Black 9% | White 14% |
| `edge` | Hairline ring on floating surfaces | Black 10% | White 12% |
| `link` | Text links and link buttons | `#0A66D0` | `#6AA8FF` |
| `track-off` | Switch track, off | `#BCBCC2` | `#48484E` |

### Accent

The accent follows the operating system: blue, purple, pink, red, orange, yellow, green
or graphite. Each has a light and a dark variant that clears 3:1 on the toolbar. Accent is
used for:

- the focus ring
- selected list rows (suggestions, menus while navigating with keys)
- selected cards (outline plus a light tint)
- progress (loading ring, download ring and bar)
- switches that are on, checked checkboxes, primary buttons

Nothing else is accent-colored. Specifically not: bookmarked stars, active tabs, links in
chrome, icons. Actions written as text in the chrome and on Ladybird pages ("Clear finished",
"Reset permissions", "Add filter list…") are in the `text` color at weight 500, underlined on
hover. Blue `link` text is only for links inside a sentence ("See everything", "Show
report"), so the eye never mistakes an action for focus or progress.

Selection values for the default blue accent:

| | Light | Dark |
|---|---|---|
| Selected row | `#0A66D0`, white text | `#2463B8`, white text |
| Selected card | Tint `rgba(10,102,208,0.08)` + inset 1.5 px `#0A66D0` | Tint `rgba(61,139,255,0.16)` + inset 1.5 px `#3D8BFF` |
| Focus ring | `0 0 0 1px #0A7AFF, 0 0 0 3px rgba(10,122,255,0.16)` | `0 0 0 1px rgba(64,140,255,0.75), 0 0 0 3px rgba(64,140,255,0.16)` |

### Profiles

Each profile has a color. It is the only brand color in the chrome, and it appears only on
the Ladybird button.

| Profile | Mark (light) | Mark (dark) |
|---|---|---|
| Personal (default) | `#4B22AD` | `#A684FF` |
| Work (example) | `#0B5E5B` | `#3FB8B0` |
| Private window | `#D8C8FF` (mask glyph) | `#D8C8FF` |

The marks are deliberately deep in light mode and toned down in dark mode. Thin lines need
at least ~5:1 against their fill to look sharp, and very bright thin lines on dark
backgrounds bloom for people with astigmatism. Profile colors added later must meet the
same bar.

### Private windows

Private windows have their own violet-dark chrome in both light and dark appearance, so a
private window never looks like a normal one. Pages inside keep following the system
appearance. The Lavender color theme stays clear of it: its base color (`#7563D4`) is a
bluer, lighter lavender, and its dark surfaces carry less tint than other themes'.

| Token | Private |
|---|---|
| `strip` | `#110A20` |
| `toolbar` | `#2F2347` |
| `text` | `#F3F0FA` |
| `muted`, `tab-title` | `#B3AAC4` |
| `icon` | `#E2DCEF` |
| `disabled` | `#5E566E` |
| `field` | White 9% |
| `hover` | White 7% (toolbar and sidebar) |
| `strip-hover` | White 6% (background tabs, the + button and the All tabs button on the strip) |
| `separator` | `#4A3D63` |
| `divider` | `#0F0A19` |

Hover strength follows the surface a control sits on, not the kind of control. On the
toolbar's 7%, a hovered tab on the strip would land almost on the active tab, so the strip
is darker and its hover is 6%. That makes strip, hovered tab and active tab about
17 / 31 / 47 in red: steps of 14 and 16, slightly wider than normal dark (20 / 34 / 46),
because violet tones are harder to tell apart than neutral grays. The Ladybird button keeps
its own hover and press fills everywhere.

The Ladybird button shows a filled domino mask (20 px, eye holes cut out) instead of the logomark. The window is titled
"Ladybird (Private)" in the Window menu, app switcher and taskbar. There is no "Private"
text badge in the toolbar.

### Status colors

| State | Light | Dark |
|---|---|---|
| No matches (find bar field) | Fill `#FDE8E8`, text `#B3261E` | Fill `#3A2225`, text `#FF9A8F` |
| Update ready dot | `#208A3A` with a 2 px ring in the strip color | same |
| Success icon | `#208A3A` | `#30B14F` |

### Contrast rules

- Body text: at least 4.5:1. Chrome text should aim for 7:1.
- Icons and thin marks: at least 3:1, and around 5:1 for anything with fine detail.
- Every pair in the token tables is checked in light, dark, private and increased contrast.
  New colors must be checked in all four.

---

## 6. Shape and elevation

### Radii

| Radius | Use |
|---|---|
| 3 px | Favicon placeholders |
| 4 to 5 px | Keycaps, small chips, buttons inside the field |
| 6 px | Toolbar buttons, inactive tabs, list rows, menu items |
| 7 px | Push buttons, address field, Ladybird button, text fields |
| 8 px | Active tab top corners, large buttons, cards |
| 10 px | Menus, selection cards, list groups |
| 12 px | Panels and popovers |

### Elevation

Every floating surface has a 1 px hairline ring (`edge`). Shadows are added by level:

| Level | Use | Light | Dark |
|---|---|---|---|
| Edge | Inline bars that sit on the page | Ring only | Ring only |
| Lift | Something being dragged (tab, bookmark row) | `0 1px 2px rgba(0,0,0,.18), 0 6px 16px rgba(0,0,0,.12)` | `0 1px 2px rgba(0,0,0,.36), 0 6px 16px rgba(0,0,0,.30)` |
| Popover | Menus, panels, suggestions, find bar, banners | `0 14px 36px rgba(0,0,0,.16)` | `0 14px 36px rgba(0,0,0,.45)` |
| Dialog | Anything that blocks the page | `0 18px 40px rgba(0,0,0,.22)` | `0 18px 40px rgba(0,0,0,.55)` |

Don't invent new shadows. Pick a level.

---

## 7. Icons and the logomark

### UI icons

- Drawn on a 16 px grid with a 1.5 px stroke, round caps and round joins. Outline only.
  The only filled glyphs are state fills (a bookmarked star).
- Rendered at 16 px inside the address field, menus and panels.
- Rendered at 18 px in toolbar buttons, with a 1.4 stroke on the 16 px grid, so they read
  as slightly bolder next to the 28 px field.
- Icon color is the `icon` token in the toolbar and `body` in panels. Never accent-colored.
- Icon colors are always opaque. Never draw an icon with a translucent stroke: where its lines overlap, the color adds up and the crossings show as brighter spots. To dim an icon (disabled, inactive window), apply opacity to the whole icon at once.
- Render icons from the vector at the screen's device pixel ratio, positioned on whole pixels. Never scale a pre-rendered bitmap.
- On 1× screens, small chrome icons (close, new tab, speaker, muted, sidebar, downloads) use a pixel-snapped drawing: 1 px lines on half-pixel coordinates so straight edges are crisp, and a 1.25 line for diagonal-only glyphs like ×. At 2× and above, use the regular drawing. See the "Icons at 1×" board.
- The current set: back, forward, reload, stop (✕), site settings (two sliders), content
  blocking (shield, used inside site settings only), bookmark, bookmarked, new tab, close
  tab, all tabs (card stack), sidebar, downloads, audio, muted, pin, warning, loading, search, private
  (domino mask, the only filled glyph besides the bookmarked star). Add to this set rather than mixing in icons from another family.

### The logomark

- The mark is a 2:3 Lissajous figure (x = sin 2t, y = sin 3t), scaled 179 : 145, drawn as
  one continuous line on a 22 × 18.4 grid.
- It is always one flat color. No gradients, no two-tone versions, no shadows inside the
  window.
- Line weight is 1.55 on the 22-unit grid wherever the mark is shown at 20 px or larger:
  the Ladybird button, the app icon at 64 px and up, the Dock and taskbar. Don't let the
  weights drift apart; they should look like the same drawing.
- Shown large (the About page, about 170 px wide), the mark uses a lighter 0.95 line. At
  1.55 it would look much heavier than the page's bold title.
- Very small renderings (app icon at 32 and 16 px) use a heavier line (2.1 and 2.4) so the
  crossings don't break up. They keep the same breathing room as the big icon.
- The mark appears in the window only on the Ladybird button. Ladybird pages may show it
  large (About, welcome).

### App icon

- A soft lavender-grey tile (`#F5F2FB` to `#E9E5F2`, top to bottom) with a hairline edge,
  and the mark in Ladybird purple `#6A39DB` at about 60% of the tile width.
- No shine, no shadow on the mark.
- macOS: Apple's rounded-square grid, built in Icon Composer so the system's Dark, Clear
  and Tinted icon styles apply automatically.
- Windows: no tile. Just the mark in `#8A5CF0`, which reads on light and dark taskbars.
- Linux: the tile with a thin, slightly darker bottom edge (GNOME style).
- Private windows: a violet-dark tile (`#2F2347` to `#1D1430`) with the mark in `#C9B5FF`.

---

## 8. Components

### Buttons

| Kind | Look | Use |
|---|---|---|
| Primary | Accent fill, white text, weight 500 | The one main action in a panel, dialog or page |
| Secondary | `ctl` fill, text color | Every other action |
| Text button | No fill, `text` color, weight 500, underline on hover | Low-weight actions in panel headers and footers ("Clear finished", "All site settings") |
| Toolbar button | No fill at rest; `hover` fill on hover; `press` while pressed | Icon-only controls in the chrome |

- Push buttons are 28 px tall with a 7 px radius and 13 px text. Page-level actions (error
  pages, welcome) may use 32 px with an 8 px radius.
- At most one primary button per surface. Primary goes last (rightmost).
- Button labels are verbs in sentence case: "Restore tabs", "Start browsing", "Send report".

### Menus

- 13 px items, 6 px item radius, keyboard shortcut right-aligned in `muted`.
- Menu items use Title Case ("New Private Window", "Open Link in New Tab"), following the
  platform menu convention.
- The highlighted item uses the selected-row colors. Checked items show a ✓.
- Separators between groups, never above the first or below the last item.

### Panels

- Anchored to the control that opened it, 4 px below, aligned to the control's edge.
- Header: 15 px or 13 px weight 600 title, optional link button on the right.
- Footer: link buttons, left and right.
- Drill-in views (site settings → "Blocked on this page") replace the panel content in
  place with a back chevron in the header. They don't open a second panel.

### Selection controls

| Control | Spec |
|---|---|
| Switch | 32 × 18 px, 14 px knob. On: accent. Off: `track-off`. |
| Checkbox | 16 px, 4 px radius. Checked: accent fill with a white ✓. Unchecked: 1.5 px inset ring in `#6E6E75` (dark `#9A9AA2`). |
| Segmented control | `ctl` track, 2 px padding, 24 px segments with a 5 px radius. Selected segment: white fill (dark `#48484E`) with a small shadow. |
| Selection cards | Used for choices with a picture (tab layout, search engine on the welcome page). Unselected: 1 px `sep` ring. Selected: accent outline and tint (see [Color](#5-color)). Don't add bold text or extra checkmarks on top. |
| Lists with a choice | Grouped list (10 px radius, `sep` ring, `sep-soft` row dividers). The selected row gets the light accent tint and a trailing accent ✓. |

### Text fields

- 28 px tall, 7 px radius, `ctl` fill (or `page` fill inside a panel) with no border.
- Focused: the focus ring.
- Placeholders say what to type, not instructions ("Search history", not "Type here to
  search your history").

### Pills and chips

- 22 px tall, 5 px radius, 12 px weight 500 text.
- Warning pills ("Not secure", "Not wikipedia.org") use `warning-fill` with `warning-ink`
  text and icon.
- The zoom chip is a neutral chip shown only while the zoom isn't 100%.
- The location and microphone chips are neutral chips with a 16 px icon before their words.

### Focus

- One focus ring everywhere (see [Color](#5-color)), drawn outside the control.
- Only for keyboard focus. Never shown after a mouse click.

---

## 9. Surfaces

### Tabs

| State | Look |
|---|---|
| Default | Favicon, title and × |
| Hover | The active tab's shape in `hover` fill, no feet |
| Active | Joins the toolbar; title in full `text` color, weight 400 |
| Dragging | Stays in the strip with a Lift shadow, flat (no tilt), slides sideways only |
| Loading | Accent ring (20 px, 1.5 px line) around the favicon, reaching only 2 px past the favicon slot so the title keeps about 6 px of room and nothing moves. The favicon shrinks to 10 px inside the ring so square icons never touch it. |
| Playing audio / muted | 20 px speaker button left of the close slot, shown while the tab was heard in the last 3.5 s; click to mute. Muted keeps the struck speaker. In sidebar rows (13 px titles) the speaker is a 16 px glyph in a 22 px button, in the `icon` color, so it matches the × in weight. In the collapsed sidebar rail, the speaker is an 18 px round badge with a 13 px glyph, overlapping the favicon's bottom-right corner by about 7 px, on an opaque toolbar-colored backing with a hairline ring; it is clickable. |
| Using the microphone | A 14 px microphone glyph left of the speaker, or in the speaker's slot without it, in the speaker's color, shown while the page holds a capture stream and for 3 s after, whether or not the stream carries sound. It is not a button: a click on it goes to the tab, and its tooltip says "Using your microphone". In sidebar rows it is a 16 px glyph like the speaker. A tab too narrow for both shows only the microphone, so it never hides which tabs use the microphone. |
| Needs attention | A 7 px accent dot on the favicon's corner; clears on visit |
| Pinned | Favicon only, 44 px wide. The microphone and the speaker take the favicon's place, side by side and centered (14 + 20 px). |
| Crashed | Warning glyph replaces the favicon |

### Address field

| State | Look |
|---|---|
| Idle | Registrable domain in `text`, the rest of the URL in `muted` |
| Hover | Fill deepens to black 11% |
| Focused | White fill, focus ring, full URL selected |
| Typing | Inline completion only from addresses the person typed before |
| Not secure | "Not secure" warning pill replaces the site settings icon |
| Lookalike domain | "Not wikipedia.org" warning pill, and the odd character highlighted in `warning-fill` |
| Bookmarked | Filled star in the `icon` color |
| Permission requested | The permission's icon replaces the site settings icon, on the accent tint |
| Zoomed | Zoom chip before the star |
| Using location / microphone | "Using your location" chip before the zoom chip, and "Using your microphone" chip before that. Each says its words for 4 s after it appears, on its own timer, then shrinks to its 30 px icon. While the field has no room for a chip's words, it shows only its icon, keeping its tooltip and accessible name; resizing never restarts the timers. The field asks for room for the icons only, so a chip's words never widen the window. A click opens site settings. |
| Blank tab | Empty and focused, placeholder "Search or enter address", no site settings icon or star |

There is no content-blocker shield in the field. Content blocking is controlled from site
settings.

### Suggestions

- The field and its suggestions are one surface, in every tab layout. There is no separate
  popover: the field grows downward, and the text row stays exactly where it was, at the
  same left edge and with the same corner radius (7 px), so the text and caret never move.
  With the field in the toolbar, the surface keeps the field's width; in the sidebar it
  grows to 600 px over the page.
- While open, the surface has the field's focused fill and Popover elevation, and the focus
  ring runs around the whole surface, in place of its hairline edge. The ring never fades
  or jumps when the surface opens or closes: focus hasn't moved, so its indicator doesn't
  change either.
- A `sep-soft` divider, inset 8 px, separates the text row from the suggestions. Rows are
  34 px, in a 6 px padded list.
- The surface only opens when there are rows to show, and shrinks back to the field when
  there are none. It is never shown empty.
- The top row is selected (selected-row colors), so Return acts on it.
- Search suggestions first, then history and bookmark matches, separated by a divider.
- "Switch to tab" is the only trailing hint; no "Return" keycaps, no "Search the web" labels.
- With no search engine set, the top row reads "Choose a search engine…" (with the typed
  text as context) and there are no search suggestions.

### Site settings

- Opened from the icon at the left of the field.
- Header: favicon, domain, "Certificate" link.
- Permissions (location, camera, microphone, notifications) with Ask / Allow / Block menus.
- "Content blocking" switch, then "Blocked on this page" with a count, which drills in to
  the list of blocked requests grouped by filter list. "Report a broken site" and "Filter
  lists" sit at the bottom of the drill-in.
- "Cookies and site data" drill-in.
- Footer: "Reset permissions", "All site settings".

### Downloads

- A Downloads button appears at the right end of the toolbar while there are downloads,
  with an accent ring for progress. It stays until the list is cleared or the window
  closes. A failed download puts a warning dot on it.
- Clicking it opens the downloads panel directly below it.
- Rows: file type tile, name, progress bar while active, then status ("12.4 of 19.3 MB · 8
  seconds left", "4.2 MB", "Failed · The connection was interrupted"). Actions are icon
  buttons: pause, cancel, show in Finder/Explorer/Files, retry.
- Downloads are also in the Ladybird menu. The Ladybird button itself never shows download
  state.

### Feedback (alpha and nightly builds)

- A Feedback button at the right end of the toolbar, after the star and Downloads: 32 × 28,
  speech bubble with an exclamation mark (a plain bubble reads as comments), tooltip "Send feedback". Shown in alpha and nightly builds only,
  can be hidden in Settings → Feedback, and removed for release.
- A small amber dot (`#D9800F` light, `#F2A33A` dark, 7 px with a 2 px toolbar-colored ring)
  appears on it while crash reports are waiting. Not the icon color (it fuses with the glyph),
  not the accent, not green (green means an update is ready), no count.
- Clicking it opens a panel directly below it, right-aligned to it:
  1. Report a problem with this site (the site report flow, prefilled with the address):
     only on a website
  2. Report a bug: opens GitHub's new-issue page in a new tab, prefilled with version, OS
     and build where possible
  3. Crash reports, "2 to review": only while reports are waiting; opens the review page.
     The same amber dot as on the button sits just before "2 to review", so the label
     stays aligned with the rows above it

### Crash reports

- Nothing is ever sent without the person choosing to. Settings → Feedback → Crash reports:
  - Ask me (default): reports are kept on this computer until reviewed.
  - Never send: nothing is kept at all; no dot, no list, no files on disk.
- Crash reports live only in Settings → Feedback, not also under Privacy. Below the choice,
  while reports are waiting: "17 reports waiting" with a Review reports button.
- If the Feedback button is hidden and the mode is Ask me, reports are still kept, and
  Settings → Feedback is the only place they surface. No other reminder appears.
- After the alpha, the Feedback button and its switch go away; the Feedback section stays,
  with the crash reports setting, so it doesn't move between versions.
- A helper process that crashes and recovers on its own (GPU, network) shows no UI. The
  report is saved locally and waits behind the Feedback button.
- A crash the person saw (a tab's content) gets "Send crash report" next to Reload on the
  crashed-tab page. Left unanswered, its report waits in the list like any other.
- A tab crash's website is saved on this computer next to its report, never inside it, and
  never from a private window. Credentials and the fragment are dropped, and clearing history
  deletes it. It is only sent if the person ticks "Include website address". A group with more
  than one lists them under "Include website addresses", each with its own checkbox, off by
  default.
- Never a popup, and never anything at startup about earlier sessions.
- If repeated crashes make Ladybird change behavior (for example, falling back to software
  rendering), say so once, in the Feedback panel.
- Unsent reports delete themselves after 30 days. At most 100 wait at once; the oldest go first.
- The review page (about:crashes) is a list, not one report at a time:
  - Title "Crash reports" with a muted "17 not sent"; Delete all and Send all at the top right.
  - Each report is its own card (12 px radius, `tile` fill, 8 px apart), never rows in one
    shared box: they are separate things to send or delete, not parts of a whole.
  - One row per crash, named in plain words ("Graphics process", "Page on example.com", or
    "Page on 2 websites" when a group spans sites) with when it happened. Identical crashes
    (same process, signature and stack) collapse into one row; a report with no stack keeps a
    row of its own:
    "Graphics process · 14 times · last 2 hours ago".
  - Rows have Delete and Send. Clicking a row expands it in place: optional "What were you
    doing?" note, "Include website address" (off by default) when one was saved, a one-line summary of what's included with Show report, Delete and Send.
  - Show report on a group of identical crashes shows the latest, under "Showing the latest of 14.
    The others are the same crash."
  - With a single report, the list has one row, already expanded.
  - No button has default focus; sending is always a deliberate click.
  - Sent or deleted rows disappear. An empty list says "No crash reports waiting".
  - A "Crash report settings" link at the bottom goes to Settings → Feedback.

### Sandbox off

When Ladybird runs with `--disable-sandbox`, every window shows a bar directly below the
toolbar (or at the top of the page area in the sidebar layouts):

- 32 px plus a `divider` line below, in the `toolbar` color, so it reads as part of the
  chrome, not as a page banner. The page starts below it.
- The `warning` icon (16 px) and one sentence in `text`: "The sandbox is off. Websites are
  less isolated from your computer."
- No close button, no action button, no amber fill. It can't be dismissed, because the state
  can't be changed from inside the browser, and it should be in every screenshot that ends
  up in a bug report.
- Bug reports and crash reports include "sandbox: off" in their prefilled details.
- Nothing else changes: no badge on the Ladybird button, no tint, nothing in the address
  field (which describes the site, not the browser).

### Ladybird menu

- 280 px wide, 6 px padding, 26 px rows. Groups, in order:
  1. Profile row (current profile, opens the profile submenu)
  2. New Tab, New Window, New Private Window, Reopen Closed Tab
  3. History ▸, Bookmarks ▸, Downloads
  4. Zoom (− 100% +), Find in Page…
  5. Settings, Inspect ▸, Help ▸ (Report Website Issue…, About Ladybird), then Quit on
     Linux and Windows only. macOS has Quit in the app menu, so the Ladybird menu leaves it
     out.
- One right edge: shortcuts are right-aligned, submenu chevrons and the zoom control end at
  the same 10 px inset as the shortcuts.
- Zoom: `ctl`-filled − and + buttons, 28×22; the percentage between them is a borderless
  button that resets to 100%, in tabular figures.
- The Debug menu appears only when Settings → Advanced → "Show Debug menu" is on.
- Never put a group of one item in its own section.

### Find in page

- A bar anchored to the top right of the page, 40 px, Popover elevation.
- Field with the match count ("3 of 12"), previous, next, match case, close.
- No matches: the field gets the "no matches" fill, the count reads "No matches" in the
  status color, and previous/next are disabled. No shake.

### New tabs

New tabs are blank (`about:newtab` styled in the `page` color, so dark mode never flashes
white). The tab is titled "New Tab", the address field is focused and empty, and the field
never shows "about:newtab".

### Ladybird pages

- Page titles are 22 px weight 600.
- Content sits in a centered column. Settings uses a 232 px sidebar and a 600 px content
  column, centered in the space to the right of the sidebar, so wide windows don't leave
  the page leaning left.
- About: the logomark, "Ladybird" in 40 px with the release badge, one tagline, and a card
  with version, build and platform plus a Copy button. Nothing else.
- Welcome (`about:welcome`): the logomark at 56 px (0.85 line, so it doesn't get heavy), the title, then one centered column, in this order: search engine (optional,
  nothing preselected, engines in alphabetical order; shown by name only in a five-column
  grid, never with logos or letter tiles, which
  would mean shipping other companies' trademarks or contacting their servers before anyone
  has chosen), Layout (all three layouts), Color, content blocking switch, "Start browsing" (enabled as soon as the
  current settings are loaded; it never waits for a choice).
- Error pages: icon, title, one line of detail, buttons, centered like every other
  single-message page ("Report sent"). Security errors may use two lines, because the
  second line protects people.

### Settings

- Sections, in this order: Appearance, Search, Tabs and startup, Privacy and security,
  Permissions, Content blocking, Languages, Feedback, Advanced. Settings opens on
  Appearance. The order is by how often people come looking, not by how the code is split.
- No "General" section. Everything has a home with a name that says what it is; a section
  named "General" or "Other" collects settings nobody could find.
- Appearance, top to bottom: Theme, Color theme, Website appearance, Force dark mode,
  Layout, Show bookmarks bar, Default page zoom.
- Color theme is a row of round swatches. The first is System (a multicolor wheel, as in
  macOS), selected by default: the browser follows the OS accent until someone picks a
  color. Names live in tooltips and accessible names, never under the swatches. The
  welcome page shows the same row under "Color", right after Layout, and picking a swatch
  there recolors the real window around the page at once; that live change is the point of
  having it on the welcome page.
- Layout is three picture cards: Tabs on top, Tabs on the side, Everything in the sidebar.
  It is called Layout, not Tabs, because the third choice moves the toolbar too. Each
  label names what is different about it, and each picture draws the address field where
  it really is (in the toolbar row, or at the top of the sidebar). With Tabs on the side, a
  "Sidebar on: Left / Right" control appears below the cards. Everything in the sidebar
  is always on the left, where the traffic lights are. The welcome page
  offers the same three cards, with the same heading, labels and pictures; in its
  three-column grid they line up with the search engine buttons above.
- Settings that change how websites look (Website appearance, Force dark mode, zoom) live
  in Appearance, next to the browser's own look, not in a separate section.
- Permissions: one row per permission (camera, microphone, location, notifications, …),
  each with its default and an "Exceptions…" button. Never one long list of sites.
- Advanced holds network, cache and developer settings: Disk cache size, DNS, proxy, and
  "Show Debug menu". Nothing a typical person needs lives here.
- The sidebar shows the selected section with the `hover` fill. The focus ring appears
  only for keyboard focus, never after a click.
- Within a section, groups get a 13 px weight 600 subheading above them ("Layout",
  "Crash reports"), the same in every section and on the welcome page. At regular weight
  it would read as a setting with a missing control. Single rows don't get one.
- Setting names use sentence case and say what they do: "Show Debug menu", not "Advanced
  Debug Menu" or "Enable debug menu".

---

## 10. Motion

### Durations

| Name | Duration | Use |
|---|---|---|
| instant | 0 ms | Hover out, anything that must feel immediate |
| fast | 100 ms | Hover in, focus, menus, active tab |
| base | 160 ms | Tabs opening, closing, reordering |
| slow | 220 ms | Sidebar, anything that moves far |
| spin | 900 ms | One turn of the loading ring |

### Easing

| Name | Value | Use |
|---|---|---|
| enter | `cubic-bezier(0.2, 0, 0, 1)` | Things appearing or responding |
| exit | `cubic-bezier(0.4, 0, 1, 1)` | Things leaving |
| move | `cubic-bezier(0.3, 0, 0.2, 1)` | Things changing position |
| linear | `linear` | Spinners and progress only |

### Rules

- Motion answers a click, a key or a real state change. Never animate on hover beyond a
  fill fading in.
- Nothing loops except progress indicators.
- Menus open with a 0.97 → 1 scale and fade from their anchor; they close with a fade
  only (80 ms).
- With reduced motion, everything becomes an instant swap or a plain fade.

### Toolbar icon motion

| Control | Motion |
|---|---|
| Back / forward | The chevron nudges 2 px in its direction and springs back, 180 ms |
| Reload → stop | The arrow turns half a turn and, over the second half, cross-fades into the ✕, 340 ms. When the load ends, the ✕ simply fades back. |
| Site settings icon | The two slider knobs slide past each other while the panel is open, 240 ms |
| Star | The fill pops in (100 → 112 → 100%) when bookmarking; removing just fades the fill |
| Downloads | The arrow drops in from 4 px above when the button appears; bobs 2 px once when a download finishes |
| Feedback | The waiting-reports dot scales in (0 → 100%, base, enter) when the first report is saved. Later reports while the dot is showing change nothing, and the icon itself never moves, so a crash loop can't animate the toolbar. |
| New tab (+) | No motion. The new tab appearing is the feedback. |
| Ladybird button | No motion. Only its fill changes on hover, press and while the menu is open. |

### Sidebar collapse with everything in the sidebar

Collapsed, this layout is the normal collapsed layout (toolbar row plus rail), so expanding
and collapsing move the toolbar's controls into the sidebar and back. Nothing flies from
one place to the other: controls that change place fade out and in, and only edges move.

- Expanding, all over `slow` (220 ms) with `move`:
  1. The toolbar row's contents (back, forward, reload, field, buttons) fade out in the
     first 80 ms.
  2. The sidebar widens from 52 to 236 px while the toolbar row's height shrinks to 0. The
     page slides up with the toolbar row's bottom edge and is covered by the widening
     sidebar; it is moved, not laid out again, on every frame.
  3. The sidebar's back, forward and reload (in the top row), field and bottom bar fade in
     over the last 120 ms. Rail tiles
     become rows as in the normal sidebar: the titles fade in, and the favicons stay put.
  4. The page resizes once, at the end, to its new width and height.
- Collapsing is the same in reverse: sidebar contents fade out first, the width and the
  toolbar row animate together, and the toolbar contents fade in last.
- The traffic lights, Ladybird button and sidebar toggle (at the bottom left) are in the
  same place in both layouts and never move. Everything else that changes place (back,
  forward, reload, the field and All tabs) fades out and in like the rest.
- If the address field is focused, focus, text, selection and caret move to the other
  field. The field itself does not animate; it fades like the rest.
- With reduced motion, the two layouts swap instantly.

The full motion catalog (tabs, sidebar, suggestions, swipe navigation, find, bookmarks,
zoom chip, drag and drop) is on the canvas's motion spec board.

---

## 11. Interaction and keyboard

### Pointer

| Situation | Behavior |
|---|---|
| Tooltips | After 500 ms of hover. Once one has shown, neighbouring tooltips appear at once for the next 1.5 s. |
| Tab close | Every tab except pinned tabs always shows its ×. A tab too narrow for both its favicon and its × shows only its favicon, and once selected, its × in the favicon's place. Nothing appears or moves on hover. Middle-click anywhere on a tab closes it. In the collapsed sidebar rail the × stays a hover badge, so favicons aren't covered. |
| Width lock | After closing a tab by clicking, tab widths stay fixed until the pointer leaves the strip, plus 300 ms, so the next × lands under the cursor. |
| Dragging tabs | Starts after 4 px. The tab stays in the strip and slides along it until pulled 40 px away (up or down; sideways out of the sidebar in vertical mode), then comes loose as a window preview. Esc cancels. |
| Dropping | A link, URL or file dropped on the strip opens a new tab at the marker; on the bookmarks bar it becomes a bookmark; on the page, the page decides. |
| Swiping | Two-finger horizontal swipe goes back or forward. Commits past 30% of the width or on a fast flick. |
| Empty strip | Double-click does what the platform does on a title bar. It never opens a tab. |
| Scrolling the strip | Scrolls overflowing tabs. Never switches tabs. |
| Address field | First click selects all, a second click places the caret. Esc restores the address; a second Esc leaves the field. |
| Menus | Open on press; releasing over an item chooses it. Submenus open after 150 ms and stay open while moving diagonally toward them. |
| Bookmarks bar folders | Behave like a menu bar: a click opens a folder; while one is open, moving over another folder switches to it. A folder closes only on a click outside, Esc or choosing an item, never because the pointer left it. |
| Hit areas | Visible controls at least 28 px; hit areas at least 32 px. |

### Focus order

Ladybird button → tabs (one stop; arrow keys move between tabs) → back, forward, reload →
address field → page. F6 moves between the chrome and the page.

### Shortcuts

| Action | macOS | Windows and Linux |
|---|---|---|
| New tab | ⌘T | Ctrl+T |
| Close tab | ⌘W | Ctrl+W |
| Reopen closed tab | ⇧⌘T | Ctrl+Shift+T |
| New window | ⌘N | Ctrl+N |
| New private window | ⇧⌘N | Ctrl+Shift+N |
| Next / previous tab | ⌃Tab, ⌥⌘→ / ⌃⇧Tab, ⌥⌘← | Ctrl+Tab / Ctrl+Shift+Tab |
| Go to tab 1–8 / last tab | ⌘1–8 / ⌘9 | Ctrl+1–8 / Ctrl+9 |
| Search all tabs | ⇧⌘A | Ctrl+Shift+A |
| Focus address field | ⌘L | Ctrl+L, Alt+D |
| Back / forward | ⌘[ / ⌘] | Alt+← / Alt+→ |
| Reload / hard reload | ⌘R / ⇧⌘R | Ctrl+R, F5 / Ctrl+Shift+R |
| Find / next match | ⌘F / ⌘G | Ctrl+F / F3 |
| Zoom in / out / reset | ⌘+ / ⌘− / ⌘0 | Ctrl++ / Ctrl+− / Ctrl+0 |
| Bookmark page | ⌘D | Ctrl+D |
| Bookmarks | ⌥⌘B | Ctrl+Shift+O |
| Show bookmarks bar | ⇧⌘B | Ctrl+Shift+B |
| History | ⌘Y | Ctrl+H |
| Downloads | ⌥⌘L | Ctrl+J |
| Show or hide sidebar | ⌥⌘S | Ctrl+Shift+S |
| Ladybird menu | (menu bar) | Alt+F |
| Settings | ⌘, | Ctrl+, |
| Move between chrome and page | F6 | F6 |

Don't print shortcuts inside panels or footers. They belong in menus and tooltips.

---

## 12. Writing

**Say less.** Every string on an everyday surface should be one the person needs right now.
If an icon, heading or button already says it, drop the text.

**Case.**

- Buttons, headings, labels, settings and captions use sentence case: "Show bookmarks bar",
  "Report a broken site".
- Menu items use Title Case: "Open Link in New Tab".

**Voice.** Plain, calm, specific. Say what happened and what the person can do. No
exclamation marks, no apologies, no jokes in errors.

**Names.** Use one name per concept, everywhere:

| Concept | Name | Not |
|---|---|---|
| The menu button | Ladybird button / Ladybird menu | app menu, profile button, hamburger |
| Blocking ads, trackers and more | Content blocking | tracker blocking, ad blocker, shield |
| The panel from the left of the field | Site settings | site info, page info |
| Windows that keep nothing | Private window | incognito, InPrivate |
| `about:welcome` | Welcome | onboarding, first run |

**Numbers and units.** "12.4 of 19.3 MB", "8 seconds left", "125%", "3 of 12". Use a thin
middle dot (·) to join short facts on one line.

**Ellipsis.** Use "…" (one character) on menu items and buttons that need more input
before they act ("Choose a search engine…", "Clear history…").

**Security and privacy text is the exception to "say less".** Certificate errors, the
full-screen notice ("example.org is now full screen · Press Esc to exit") and what a report
sends must stay, even though they add words.

---

## 13. Accessibility

- Every control has an accessible name. Icon-only buttons get an `aria-label`, and a
  tooltip with the same text plus its shortcut.
- State is exposed, not only drawn: `aria-pressed` on toggles (star), `aria-checked` on
  switches and checkboxes, `aria-selected` on suggestion rows, `aria-current` on the active
  tab and sidebar section, `aria-disabled` on unavailable actions.
- Color is never the only signal. The private window has its own glyph, find's "no matches"
  has text, warnings have an icon and words, and profiles differ by color and name.
- Support the platform's increased-contrast mode (see the token tables), reduced motion, and
  the system text size where the platform offers it.
- Check contrast in light, dark, private and increased contrast for every new color.
- Thin marks (logomark, icons) are checked for legibility, not only contrast. Avoid very
  light, very thin lines on dark backgrounds.

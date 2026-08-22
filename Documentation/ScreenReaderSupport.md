# Screen-reader support

Ladybird exposes its internal accessibility tree to the Orca and VoiceOver screen readers — and to other platform assistive technologies (AT) — by serializing tree data from the WebContent process and presenting it through platform-specific wrapper objects in the UI process.

The AppKit port uses `LadybirdAccessibilityElement`, an Objective-C wrapper that implements the `NSAccessibility` protocol.

The Qt port on macOS essentially bypasses Qt’s `QMacAccessibilityElement` Cocoa bridge almost entirely — by using the same `LadybirdAccessibilityElement` wrapper as the AppKit port, but also using an NSView overlay along with three very small, targeted swizzles on `QMacAccessibilityElement` to redirect Qt’s bridge to the NSView overlay.

The Qt port on Linux uses Qt’s built-in AT-SPI2 bridge (`QSpiAccessibleBridge`) — by way of `AccessibilityInterface` (a `QAccessibleInterface` wrapper), along with a custom Orca script.

> [!NOTE]
> Qt’s built-in AT-SPI2 bridge implements direct communication over the Linux AT-SPI2 D-Bus accessibility protocol — rather than, say, making calls using the ATK C API, which is what Firefox and Chromium use for AT-SPI2 D-Bus communication.

The core underlying “infrastructure” code for IPC serialization, tree management, and WebContent-side tree building is shared across all platforms and ports/UIs.

## Key technologies

This section explains key technologies referenced throughout the rest of the document.

### AT-SPI2

AT-SPI2 is the Linux desktop-wide accessibility-bus protocol: a D-Bus protocol through which applications publish an object tree. Platform assistive technologies (AT) — screen readers, screen magnifiers, accessibility-tree inspectors — connect to the same bus and query that object tree.

The protocol defines interfaces on each accessible object: `Accessible`, `Text`, `Component`, `Action`, `Hypertext`, `Hyperlink`, `Document`, `Table`, `Value`, `Selection`, and a few others. Each interface is a D-Bus interface with methods like `GetText(start, end)` or `GetCaretOffset()`.

There is no AT-SPI2 spec; instead, the protocol is defined by its reference implementation, [at-spi2-core](https://gitlab.gnome.org/GNOME/at-spi2-core). The canonical surface for the AT-SPI2 protocol is that `at-spi2-core` reference implementation’s set of D-Bus interface XML files: `Accessible.xml`, `Text.xml`, `Component.xml`, `Hypertext.xml`, `Hyperlink.xml`, `Document.xml`, `Cache.xml`, and so on).

The client-side C library for AT-SPI2 is **`libatspi`**, which wraps the raw D-Bus calls in a GObject API; `pyatspi` is its Python binding, and Orca uses both. [A libatspi API reference](https://docs.gtk.org/atspi2/) documents the same interfaces from the client side, and the [ATK API reference](https://docs.gtk.org/atk/) complements that — because AT-SPI2 interfaces mirror their ATK counterparts almost one-to-one, so the ATK docs effectively describe the semantics of the matching AT-SPI2 methods.

### ATK

[ATK](https://docs.gtk.org/atk/) is a C/GObject API that actually predates AT-SPI2 and that’s historically been the shape of how GTK apps expose or “serve” their accessibility trees to clients. An ATK object is a GObject subclass which implements interfaces like `AtkText`, `AtkComponent`, etc. There’s a separate `libatk-bridge-2.0` library that takes an ATK tree at runtime and republishes it over the AT-SPI2 D-Bus to clients. Chromium, Firefox, and GTK3 apps (though not GTK4 apps) all implement ATK objects to serve/expose their accessibility trees, and rely on `libatk-bridge-2.0` to handle the D-Bus-side client communication.

### Qt AT-SPI2 bridge

Qt’s AT-SPI2 bridge (`QSpiAccessibleBridge`, compiled into `QtGui.so`) plays the same role that `libatk-bridge-2.0` plays for ATK-based apps: It takes a tree of `QAccessibleInterface` objects (Qt’s platform-agnostic accessibility API) and talks AT-SPI2 D-Bus directly — _without_ going through ATK or `libatk-bridge-2.0`.

The Qt AT-SPI2 bridge activates automatically when Qt detects a screen reader on the session bus, and there’s no supported way to disable it — and that constraint drives many of the design decisions recorded in other parts of this document.

### Orca and VoiceOver

The GNOME project’s **Orca** utility is the standard screen reader on Linux. It’s an AT-SPI2 client written in Python, uses `pyatspi`/`libatspi` to handle the AT-SPI2 communication with applications, and is what ultimately announces the accessibility tree to users. Orca has an extension system: Per-application Python modules/scripts in `~/.local/share/orca/orca-scripts/` that subclass `orca.scripts.default.Script` (or `web.Script` for browsers) to customize behavior for a specific application.

Apple’s **VoiceOver** utility is the standard screen reader on macOS. It talks to applications via the NSAccessibility protocol (the macOS equivalent to AT-SPI2).

The view of web content that Orca and VoiceOver “see” is a _snapshot_ of the accessibility tree — roles, names, bounds, states, and text — exposed by the browser engine to the browser’s UI process. The browser UI process transforms the accessibility tree into platform-specific AT protocol traffic, and Orca and VoiceOver receive that traffic and stitch the tree back together from it, and then speak the contents of the tree to users.

On Linux, the Qt AT-SPI2 bridge is a component in our Qt-based browser UI process which turns the Qt-side accessibility tree (`QAccessibleInterface` wrappers that get built around accessibility data from the browser engine) into AT-SPI2 D-Bus traffic.

On macOS, there is no separate bridge component: `LadybirdAccessibilityElement` (NSObject wrappers in the UI process, built around accessibility data from the browser engine) implements the NSAccessibility protocol directly — and VoiceOver uses the NSAccessibility protocol to query those wrappers across process boundaries.

## Architecture

```
        WebContent process                               UI process
-------------------------------------+---------------------------------------------
DOM + ARIA tree                      |
    |                                |
build_accessibility_tree()           |
    |                                |
serialize_tree_as_node_data()        |
    |                                |
    +--- IPC: did_get_accessibility ----->       AccessibilityTreeManager
         _tree(page_id, nodes)       |         (C++, in LibWebView, shared)
                                     |      HashMap<i64, AccessibilityNodeData>
-------------------------------------+                     /
                                                          /
                                          +--------------+
                                         /
                  +---------------------+-------------+
                 /                                     \
             AppKit                                    Qt
          (macOS-only)                                   \
             /                                +--(macOS)--+--(Linux)--+
            /                                /                         \
LadybirdAccessibilityElement  LadybirdAccessibilityElement   AccessibilityInterface
 (Obj-C, NSObject subclass)    (via NSView overlay, plus     (QAccessibleInterface)
            \                   three method swizzles on               /
             \                  QMacAccessibilityElement)             / 
              \                        /                      Qt AT-SPI2 bridge 
               \                      /                             /
              (NSAccessibility protocol)                    (AT-SPI2 protocol)
                          |                                       /
                    +-----+-----+                  +-------------+------------+
                    | VoiceOver |                  |           Orca           |
                    +-----------+                  +--------------------------+
                                                   | + loads our custom Orca  |
                                                   |   scripts, which runtime |
                                                   |   monkey-patch Orca’s    |                                                                          
                                                   |   own Python internals   |
                                                   +--------------------------+
```

That mostly follows the Chromium/Firefox model rather than the WebKit model:

- No dependency on private Apple APIs (WebKit uses the private `NSAccessibilityRemoteUIElement` API)
- The accessibility wrapper objects live in the UI process
- The platform-agnostic parts (`AccessibilityNodeData` IPC data format, IPC endpoints, WebContent-side serialization, `AccessibilityTreeManager` UI-process tree manager) are shared between both UIs

On Linux specifically, the Qt AT-SPI2 bridge and the custom Orca script work as a pair: the bridge publishes our tree over AT-SPI2 D-Bus in whatever shape we can give it through `QAccessibleInterface`, and the Orca script closes the gaps that remain on the Orca side (through Python subclass overrides and a handful of runtime monkey patches).

## IPC data format: AccessibilityNodeData

`AccessibilityNodeData` (in `Libraries/LibWebView/`) is the serialization format. Each node carries:

- `id` / `parent_id` / `child_ids` — tree structure via flat ID references
- `role` — ARIA role string (`button`, `heading`, `banner`, etc.)
- `name` / `description` / `value` — accessible name, description, value
- `bounds` — bounding rect in CSS pixels (viewport-relative)
- `is_focused` / `is_disabled` / `is_editable` / `is_multi_line` / `is_read_only` / `is_required` / `is_invalid` / `is_multi_selectable` / `is_pressed` / `is_visited` — individual state bits
- `heading_level` — for `h1`—`h6` and explicit `aria-level`
- `live` — `aria-live` value (`assertive`, `polite`, or empty)
- Table geometry: `column_span`, `row_span`, `cell_row_index`, `cell_column_index`, `table_row_count`, `table_column_count`, `column_header_ids`, `row_header_ids`, `table_caption_id`, `table_summary`
- `keybinding`, `checked_state`, `expanded_state`
- Text formatting: `font_family`, `font_size`, `font_weight`, `font_style`, `color`, `background_color`, `text_decoration`
- Caret/selection: `caret_offset`, `selection_start`, `selection_end`
- Text layout: `character_offsets` (per-character viewport positions), `line_break_character_offsets`, `line_heights` — used by macOS text-marker navigation

The IPC payload is a flat `Vector<AccessibilityNodeData>` with parent/child ID references, rather than a nested tree. That simplifies IPC encoding and lets the `AccessibilityTreeManager` build its own lookup tables.

## IPC endpoints

| Endpoint | Direction | Purpose |
| -------- | --------- | ------- |
| `request_accessibility_tree(page_id)` | UI —> WebContent | Request full tree snapshot |
| `perform_accessibility_action(page_id, node_id, action)` | UI —> WebContent | AT-triggered action: `press`, `click`, `focus`, `scroll_into_view` |
| `inspect_accessibility_tree(page_id)` | UI —> WebContent | Debug JSON dump for DevTools |
| `did_get_accessibility_tree(page_id, nodes)` | WebContent —> UI | Full tree response (also used for live updates) |
| `did_accessibility_focus_change(page_id, node_id)` | WebContent —> UI | Focus changed |
| `did_inspect_accessibility_tree(page_id, json)` | WebContent —> UI | Debug JSON response |

See the _Qt on Linux_ section below for what `perform_accessibility_action`’s `scroll_into_view` value does — and why it’s distinct from `focus`.

## WebContent-side serialization

`AccessibilityTreeNode::serialize_tree_as_node_data()` walks the existing GC-managed accessibility tree (the same tree used for DevTools JSON inspection) and produces the flat vector. For each node:

- Role from `element—>role_or_default()` via `ARIA::role_name()`
- Name/description from the existing ACCNAME 1.2 implementation
- Bounds from `element—>get_bounding_client_rect()`
- Heading level from `aria_level()` (works for both `h1`—`h6` and `role="heading" aria-level="N"`)
- Live value from explicit `aria-live`, or implicit from role (alert —> assertive, status/log/marquee/timer —> polite)
- Table geometry from the grid layout
- Text formatting from computed style

`update_layout()` must be called before building the tree — because `exclude_from_accessibility_tree()` checks `layout_node()`, which asserts layout is up-to-date.

Text children of `head` (e.g., `title` text) are filtered out during serialization, since they’re not visible content. Text leaf nodes inherit their parent element’s bounding rect — since text DOM nodes don’t have their own layout box.

## UI-process tree manager: AccessibilityTreeManager

`AccessibilityTreeManager` (in `Libraries/LibWebView/`) is a platform-agnostic C++ class that caches the tree in the UI process. It provides:

- `node(id)` — lookup by node ID
- `root()` — document root node
- `hit_test(point)` — recursive hit testing (reverse child order, deepest node wins)
- `set_focused_node(node_id)` — updates focus tracking
- `text_leaves_in_order()` — flat DFS-ordered list of text leaf node IDs for text-marker navigation
- `update_tree(nodes)` — replaces the tree, with `aria-live` change detection: compares old and new node names, fires `on_live_region_changed` callback when content changes inside a live region

## AppKit: NSAccessibility wrapper

`LadybirdAccessibilityElement` (in `UI/AppKit/Interface/`) is an `NSObject` subclass implementing the `NSAccessibility` “informal” protocol. Each instance wraps one node by holding a node ID and a pointer to the `AccessibilityTreeManager`. Elements are cached in a dictionary keyed by node ID on the `LadybirdWebView`.

### Why NSObject, not NSAccessibilityElement?

Chromium uses `NSAccessibilityElement` as its base class. We tried that but encountered two problems:

1. `NSAccessibilityElement`’s synthesized read-write properties (internal ivar storage) competed with our getter overrides — the accessibility system read the stored nil values rather than calling our methods.
2. `accessibilityParameterizedAttributeNames` overrides were never called — `NSAccessibilityElement` swallowed them.

`NSObject` with the “informal” protocol gives us full control. Tradeoff: we have to override certain “modern” property API methods that `NSAccessibilityElement` would provide automatically. In particular, we found that missing overrides for `accessibilityWindow` and `accessibilityTopLevelUIElement` caused VoiceOver to treat nested elements as orphaned (not part of any window) — breaking VoiceOver forward navigation into nested groups.

### Dual API support: “informal” and “modern”

The macOS accessibility system queries objects via two code paths:

1. **Older “informal” protocol** — `accessibilityAttributeValue:`, `accessibilityAttributeNames`, `accessibilityIsIgnored`
2. **“Modern” property API** — `accessibilityRole`, `accessibilityTitle`, `accessibilityChildren`, etc.

Both paths must return consistent data. The “informal” protocol is the source of truth, with “modern” property overrides that delegate to `accessibilityAttributeValue:`. All relevant modern overrides must be present; on `NSObject`, the defaults return nil, and VoiceOver may treat nil-returning elements as invalid.

### Role mapping

ARIA roles map to `NSAccessibility` roles via `aria_role_to_ns_role()`. Landmark roles (banner, navigation, main, etc.) map to `NSAccessibilityGroupRole`, with `AXLandmark*` subrole strings. `NSAccessibilityRoleDescription()` doesn’t recognize the `AXLandmark*` subrole strings (it returns `group` for all of them) — so custom role description strings are provided, matching what Safari, Chrome, and Firefox all do.

The `document` role maps to `@"AXWebArea"` with role description `@“HTML content”`. That was an undocumented private WebKit string adopted by all browser engines until Apple made it a public constant (`NSAccessibilityWebAreaRole`) in macOS 26. Without `AXWebArea`, VoiceOver treats the entire page as a generic group and summarizes it — rather than offering web-specific navigation (rotor, search predicates, text markers).

### Tree exclusion

Per the [ARIA spec’s tree-exclusion rules](https://www.w3.org/TR/wai-aria-1.2/#tree_exclusion), elements in the following cases are excluded from the accessibility tree during `build_accessibility_tree()`, along with all their descendants:

- Elements with no layout node (`display:none`, HTML `hidden` attribute)
- Elements with `visibility:hidden` or `visibility:collapse` (those have layout nodes but aren’t visually perceptible)
- Elements with `aria-hidden=true`, including descendants (checked via `Element::is_aria_hidden()`, which walks up the ancestor chain)

Elements with `role=none` or `role=presentation` are also excluded, but their children are promoted to the parent (per the spec: _“their descendants and text content are generally included”_). If such an element has global ARIA attributes (like `aria-label`), the presentational role is overridden and the element isn’t excluded.

### Ignored-element handling

Elements with certain roles are “ignored” — transparent to VoiceOver. Their children are promoted to the nearest non-ignored ancestor. The `is_ignored_role()` static function centralizes this check:

- `generic` role with no name (HTML `div`, `span`, `body`)
- `paragraph` role with no name

That check is used in four places (all calling the same function):

- `accessibilityIsIgnored`
- parent walking in `accessibilityParent`
- `collectUnignoredChildren`
- the search-predicate DFS

Parent-child consistency is maintained: if element C appears in parent P’s `accessibilityChildren`, then C’s `accessibilityParent` returns P.

### VoiceOver navigation via search predicates

VoiceOver navigates web content (VO+Right/Left) via the `AXUIElementsForSearchPredicate` parameterized attribute, not via basic tree traversal. Without search predicates, it only does sibling navigation within groups.

`AXUIElementsForSearchPredicate` must be advertised by `accessibilityParameterizedAttributeNames` on every element. VoiceOver checks that list on the current element before calling it. If a nested group element doesn’t advertise search predicates, VoiceOver falls back to sibling-only navigation within that group.

The search-predicate implementation does a pre-order depth-first traversal. Two important behaviors:

1. **Leaf-like roles are navigation terminals.** Links, buttons, headings, menu items, tabs, radio buttons, checkboxes, and images don’t have their children included in the navigation order. Their accessible name already carries the text content. Without that, for e.g., a link with the text _“Planning”_, VoiceOver would visit both, _“link Planning”_ and _“text leaf Planning”_ — as separate stops.

2. **Container descendants are skipped.** When navigating forward past a container element (e.g., VoiceOver exits a `nav` and the parent’s search predicate receives the `nav` as the start element), all descendants of the start element are skipped. In pre-order DFS, descendants form a contiguous block after their ancestor — so if we _don’t_ skip that block, VoiceOver reads it out twice.

`shouldGroupAccessibilityChildren` returns `NO` on all elements — to prevent VoiceOver from grouping children by screen position rather than document order.

### Actions

`accessibilityPerformAction:` handles `NSAccessibilityPressAction` by sending a `perform_accessibility_action` IPC with action `press`. On the WebContent side, `HTMLElement::click()` is called on the target node, looked up by `Node::from_unique_id(UniqueNodeID)`. `accessibilitySetValue:forAttribute:` handles the focused attribute by sending action `focus`, which calls `HTML::run_focusing_steps()` on the target element.

### Table navigation

Table-specific attributes are returned conditionally based on role:

- **Table elements:** `AXRowCount`, `AXColumnCount`, `AXRows`, `AXVisibleRows`, `AXColumns`, `AXVisibleColumns`, `AXHeader` (computed from the tree structure)
- **Row elements:** `AXIndex` (zero-based row index spanning rowgroups)
- **Cell elements:** `AXRowIndexRange`, `AXColumnIndexRange` (position and span)

### Coordinate conversion

Element bounds arrive as CSS pixel rectangles relative to the viewport. On macOS, CSS pixels equal points (the `NSView` coordinate unit), so no device-pixel-ratio scaling is needed:

```objc
NSRect window_rect = [self convertRect:viewRect toView:nil];
return [self.window convertRectToScreen:window_rect];
```

### Text markers

Text markers enable cursor-level text navigation. Each marker encodes a node ID (i64) and a character offset (i32), in a 12-byte `AXTextMarkerRef`. Text-marker attributes are exposed only on the document root element (`AXWebArea` role) — not on structural containers. We found that exposing them on every element caused VoiceOver to treat containers as text content.

On the `AXWebArea` itself, three text-marker-related attributes work together to give VoiceOver a starting cursor position: `AXStartTextMarker` (the document beginning), `AXEndTextMarker` (the document end), and `AXSelectedTextMarkerRange` — a zero-width range at the first text leaf. Without `AXSelectedTextMarkerRange`, on page load, VoiceOver fires its load-complete handler but has no anchor for its cursor — which makes it fail to move into the web-content area.

### `AXWebArea` attributes for VoiceOver focus-on-load

Beyond text markers, the document `AXWebArea` element exposes a small set of attributes that VoiceOver uses to recognize it as the active reading target after page load:

- `AXFocused`: returns `YES` when the containing view is window first responder. VoiceOver uses this to identify which web area is currently the active reading target.
- `AXURL`: the page URL. VoiceOver compares this against its own page-identity tracking — to decide whether the load is a new page worth re-reading.
- `AXVisibleCharacterRange`: a non-empty range — so that VoiceOver doesn’t treat the `AXWebArea` as skippable empty content.

Together with `AXSelectedTextMarkerRange` and the `accessibilityFocusedUIElement` behavior described below, those attributes match what Safari and Chrome expose on their respective `AXWebArea` elements after page load. Without them, VoiceOver receives `AXLoadComplete` — but never moves its cursor into the web content.

### Live-region announcements

When `AccessibilityTreeManager::update_tree()` detects that a node’s `name` changed, and that node is inside a live region (has a non-empty, non-“off” `live` value in its ancestor chain), it fires the `on_live_region_changed` callback. `LadybirdWebView` posts `NSAccessibilityAnnouncementRequestedNotification` with `NSAccessibilityPriorityHigh` for “assertive” or `NSAccessibilityPriorityMedium` for “polite”.

### Live DOM-mutation updates

After an accessibility action is performed (press, focus), `schedule_accessibility_tree_update()` is called with a 200ms debounce timer to rebuild the tree and push it via `did_get_accessibility_tree`. That handles cases where the action changes the page content.

### LadybirdWebView integration

`LadybirdWebView` (`NSView` subclass) acts as the scroll-area container, with role `NSAccessibilityScrollAreaRole` and a single child: the document root `LadybirdAccessibilityElement`. It owns the `AccessibilityTreeManager` and element cache. Callbacks:

- `on_load_finish` → `request_accessibility_tree()` (direct)
- `on_load_start`, `on_url_change`, `on_title_change` → `scheduleAccessibilityTreeRequest` (debounced, 500ms). Multiple callbacks may fire in quick succession during navigation; `performSelector:afterDelay:` with `cancelPreviousPerformRequests` ensures only one tree request fires after they settle. That covers pages where `on_load_finish` doesn’t fire.
- `on_accessibility_tree_received` → update manager, clear cache, post `NSAccessibilityLayoutChangedNotification` on the view; transfer window first-responder to the view (so AppKit resolves `accessibilityFocusedUIElement` through it); locate the `AXWebArea` element and post `NSAccessibilityFocusedUIElementChangedNotification` and `@"AXLoadComplete"` on it (matching what Chromium and Firefox do); call `NSAccessibilityHandleFocusChanged()` — so AppKit refreshes its cached focused-UI-element.
- `on_accessibility_focus_changed` → update manager, post `NSAccessibilityFocusedUIElementChangedNotification`
- `on_live_region_changed` → post `NSAccessibilityAnnouncementRequestedNotification`

`accessibilityFocusedUIElement` returns the focused DOM element when one is focused (e.g., an `<input>` the user clicked into) — and otherwise returns the `AXWebArea` (document root). That matches what Safari and Chrome also do. Returning a leaf (heading, link, etc.) instead causes VoiceOver to interpret the focus as “user already navigated to this element” — which makes it unexpectedly skip its read-from-document-beginning behavior.

## Qt on macOS: shared LadybirdAccessibilityElement

On macOS, the Qt port uses the same `LadybirdAccessibilityElement` wrapper as the AppKit port. A `WebContentAccessibilityView` (NSView overlay) conforms to `LadybirdAccessibilityViewProtocol` — the same protocol the AppKit `LadybirdWebView` conforms to — and provides the scroll-area container, element cache, coordinate conversion, and search predicate delegation.

The overlay is added as a subview of the QWidget’s underlying NSView (obtained via `QWidget::winId()`). It overrides `isFlipped` to return `YES` so that Cocoa’s coordinate conversion matches CSS’s top-left origin.

### Three swizzles on QMacAccessibilityElement

VoiceOver in a Qt app navigates through `QMacAccessibilityElement` objects (created by Qt’s cocoa bridge) — not through NSViews. Our overlay is an NSView, so VoiceOver cannot discover it through the view hierarchy alone. Three very small, targeted swizzles on `QMacAccessibilityElement` connect Qt’s bridge to the overlay:

1. **`accessibilityRole`**: Returns `@"AXWebArea"` for elements that back a `WebContentView`.
2. **`accessibilityChildren`**: Returns the overlay (which contains the root `LadybirdAccessibilityElement`) as the sole child.
3. **`accessibilityFocusedUIElement`**: Delegates to the overlay’s `accessibilityFocusedUIElement`.

All other accessibility semantics — role mapping, landmark subroles, role descriptions, search predicates, ignored-element handling, hit testing, actions, text markers — are handled by `LadybirdAccessibilityElement`, shared with the AppKit port.

A minimal `WebContentViewAccessible` (`QAccessibleWidget` with `Grouping` role, no children) is registered via `QAccessible::installFactory()` so Qt’s bridge creates a `QMacAccessibilityElement` for the swizzles to act on.

### Why swizzles are needed (and alternatives that were tried)

Several swizzle-free approaches were tested:

- **Overlay only** (no swizzles, no `makeFirstResponder`): VoiceOver never discovers the overlay. It stays in the browser UI.
- **`makeFirstResponder` on the overlay**: VoiceOver discovers the overlay and announces “web content”, but enters an infinite re-entry loop (repeating “web content” endlessly). The loop appears to be caused by VoiceOver’s web-content-mode entry logic conflicting with the overlay being the first responder.
- **`makeFirstResponder` without `NSAccessibilityHandleFocusChanged`**: VoiceOver never discovers the overlay.
- **`object_setClass` per-instance subclass injection on the QWidget’s NSView**: Crashes in `QCocoaWindow::safeAreaMargins()` because the class change breaks Qt’s internal NSView state.
- **Swizzling QNSView** (the QWidget’s NSView class) instead of `QMacAccessibilityElement`: Breaks accessibility for the entire application because QNSView is shared by all widgets.

The three swizzles on `QMacAccessibilityElement` are the minimum that works. They are targeted (only affect elements backing a `WebContentView`), stable (operate within Qt’s existing bridge mechanism), and very small and simple (each is ~5 lines that delegate to the overlay).

### Advantages of the overlay approach

An earlier implementation used a `QAccessibleInterface` subclass (770 lines) that reimplemented all the role mapping, ignored-element handling, text content, actions, and tree navigation that `LadybirdAccessibilityElement` already provided for the AppKit port. 7 runtime swizzles on `QMacAccessibilityElement` patched Qt’s bridge to add `AXWebArea`, landmark subroles, role descriptions, search predicates, focused-element handling, and `ListItem` ignore behavior.

The overlay approach replaced all of that:

| | Earlier approach | Current overlay approach |
|---|---|---|
| macOS Qt a11y code | `AccessibilityInterface` (770 lines) + 7 swizzles (464 lines) = **1,234 lines** | Overlay + three swizzles (**480 lines**) |
| Code shared with AppKit | `AccessibilityTreeManager` only | `AccessibilityTreeManager` + `LadybirdAccessibilityElement` (all semantics) |
| Swizzles on `QMacAccessibilityElement` | 7 (role, subrole, roleDescription, parameterizedAttributeNames, attributeValue:forParameter:, focusedUIElement, isIgnored) | 3 (role, children, focusedUIElement) |
| What swizzles do | Reimplement web-content semantics inside the swizzle functions | Delegate to the overlay (which delegates to `LadybirdAccessibilityElement`) |
| Qt-specific role mapping | Full reimplementation in `AccessibilityInterface::map_role()` | None |
| Qt-specific search predicates | DFS traversal reimplemented in swizzled `accessibilityAttributeValue:forParameter:` | None |
| Element cache | `QHash<i64, AccessibilityInterface*>` with manual pruning | `NSMutableDictionary` on the overlay (same pattern as AppKit) |
| Adding a new toolkit | Would require another full reimplementation | Reuse overlay pattern with new toolkit’s bridge |

The key insight: the earlier approach treated the Qt port as a separate accessibility implementation that happened to share `AccessibilityTreeManager`. The overlay approach treats `LadybirdAccessibilityElement` as the single source of truth for macOS accessibility semantics, with the overlay acting as a thin adapter between Qt’s bridge and the shared elements.

### Differences from the AppKit implementation

| Aspect | AppKit | Qt |
| ------ | ------ | -- |
| NSAccessibility wrapper | `LadybirdAccessibilityElement` (direct) | Same `LadybirdAccessibilityElement` (via overlay) |
| Container | `LadybirdWebView` (NSView, conforms to `LadybirdAccessibilityViewProtocol`) | `WebContentAccessibilityView` (NSView overlay, conforms to same protocol) |
| Bridge to platform | None needed (native NSView) | three swizzles on `QMacAccessibilityElement` + minimal `QAccessibleWidget` factory |
| Coordinate system | `isFlipped` inherited from parent | `isFlipped` overridden to `YES` on overlay |
| Element cache lifecycle | Cleared on every tree update | Same (cleared on every tree update) |
| Coordinate conversion | `convertRect:toView:nil` + `convertRectToScreen:` | Same (on the overlay NSView) |

## Qt on Linux: QAccessibleInterface with custom Orca scripts

On Linux, the Qt port uses `QAccessibleInterface` (`UI/Qt/AccessibilityInterface.cpp`) to expose the accessibility tree through Qt’s built-in AT-SPI2 bridge (`QSpiAccessibleBridge`). A custom Orca screen-reader script at `~/.local/share/orca/orca-scripts/Ladybird/` extends Orca’s `web.Script` to close gaps in Qt’s bridge and tune browse-mode behavior for web content.

The implementation is split between C++ (Qt/LibWeb) and Python (Orca):

- **C++ side** decides what the AT-SPI2 tree looks like (which nodes are exposed, what their text is, what children they have, which states and interfaces they advertise).
- **Python side** (the Orca script) sits inside Orca’s process and adjusts Orca’s own behavior where the default behavior is wrong for our tree shape or Qt’s bridge’s quirks.

What works:

- Say All (auto-reading through an entire document)
- Flat review (manually reading word-by-word/line-by-line)
- Object navigation through the accessibility tree
- Structural navigation (`H` key for headings, `K` for links, etc.)
- Table-cell navigation
- Focus ring shown during various forms of navigation
- Actions (press, focus, scroll-into-view) on all interactive elements
- Live-region announcements

### Tree-exposure rules (C++ side, at a glance)

The shape of the AT-SPI2 tree that Orca sees is determined by four overlapping decisions in `UI/Qt/AccessibilityInterface.cpp`:

1. **`is_ignored_role(role, name)`**. Returns true only for `generic` elements with no accessible name: `div`, `span`, unnamed `body`, etc. Ignored elements are not exposed; their children “bubble up” via `collect_unignored_children()` to the nearest non-ignored ancestor. Paragraphs are _not_ ignored — even when they have no name: Orca’s sentence-extension logic during Say All relies on seeing `ROLE_PARAGRAPH` in the tree to stop walking across paragraph boundaries. Without that role present, Orca would merge content across paragraphs up to the nearest heading.

2. **`is_leaf_like_container_role(role)`**. Returns true for roles that Orca reads as whole units: `link`, `button`, `heading`, `img`, `image`, `menuitem`, `tab`, `checkbox`, `radio`, `listitem`. For those, `build_hypertext()` flattens the full descendant text into a single string — so Orca gets readable content in one query, rather than a `See [U+FFFC] for details`-style string that, to expand, would require the AT-SPI2 Hypertext interface (but which the Qt AT-SPI2 bridge doesn’t support).

3. **`interface_cast(QAccessible::TextInterface)`**. Decides which roles advertise a Text interface over AT-SPI2. Text leaves always do. Leaf-like containers do (their flattened text is their content). `paragraph` is explicitly included — Orca’s `is_text_block_element()` heuristic requires `AXObject.supports_text(obj)` to return true for sentence-boundary logic to fire. `list` is explicitly _excluded_ — a list’s text over AT-SPI2 would be `[U+FFFC][U+FFFC][U+FFFC]…` (one marker per `listitem`), and without a Hypertext interface, Orca can’t expand those markers, and skips the entire list during Say All. Leaving the list without a Text interface makes Orca descend into listitem children naturally instead.

4. **`state().focusable`**. Set to true _only_ for roles that are genuinely keyboard-focusable (`button`, `link`, form controls, menu items, tabs, tree items, switches), plus any node that is actually editable or currently focused. Reporting `focusable=true` for every element (some simpler behavior we tried) was found to break Orca’s `is_text_block_element()` heuristic: that heuristic returns false for any focusable object, so paragraphs/headings/list items that claimed to be focusable were excluded from text-block treatment — collapsing sentence boundaries and leading to an unexpected “resume at the top of the document every time” Say-All bug.

Two helpers serve different callers:

- `collect_unignored_children(node)` – returns all non-ignored children, recursing through ignored intermediates. Used by `build_hypertext()` and `find_unignored_parent()` (that is, the internal tree view).
- `collect_exposed_children(node)` – returns what `child()` and `childCount()` advertise to AT-SPI2, and matches similar handling in Firefox.

### Text content: hypertext with flattening

`build_hypertext()` in `AccessibilityInterface.cpp` produces the text that `QAccessibleTextInterface::text(start, end)` returns for a node:

- **Text leaves**: just the leaf’s own text.
- **Leaf-like containers** (listitem, heading, link, button, image, menuitem, tab, checkbox, radio): `flatten_descendant_text()` recursively concatenates every descendant’s text. The node presents one continuous string, with no U+FFFC markers.
- **General containers** (paragraph, section, table cell, etc.): Returns what Orca expects for a paragraph like `<p>Alpha <a>aaa</a> more.</p>`. What Orca sees for that is `Alpha [U+FFFC] more.` — and Orca’s `find_next_caret_in_order` is supposed to expand the [U+FFFC] marker querying the AT-SPI2 Hypertext interface. But because Qt’s bridge doesn’t implement that, if we returned the default expansion None, that’d cause Say All to skip the link entirely and continue with the text after the U+FFFC.

### Focus, and the “accessibility focus target” pseudo-state

Two kinds of “focus” exist in the Linux flow:

- **DOM focus** – the element the keyboard user has focused (via `Tab`, `click`, or JS). Painted as `:focus-visible` by the existing CSS machinery. Orca browse-mode navigation must _not_ move this; the user expects `Enter` to activate whatever _they_ focused with the keyboard, regardless of where Orca is reading.
- **Accessibility focus target** – the element Orca is currently speaking/reviewing. The Linux flow paints a focus ring on this target so the screen reader’s current position is observable visually. DOM focus is unchanged.

The “accessibility focus target” behavior is implemented by a new `Document::set_accessibility_focus_target(Element*)` member on `Libraries/LibWeb/DOM/Document.h`, and a matching pseudo-class match in `Libraries/LibWeb/CSS/SelectorEngine.cpp`: `:focus-visible` now also matches when `document.accessibility_focus_target() == &target.element()`. The existing focus-ring UA stylesheet rule is reused — no new pseudo-class, no new outline style. Style invalidation mirrors `Document::set_target_element()`, so the ring repaints on both the old and new targets.

The target is set by a new `scroll_into_view` accessibility action on the WebContent side:

```
perform_accessibility_action(page_id, node_id, "scroll_into_view")
  —> Element::scroll_into_view(Nearest/Nearest)
  —> Document::set_accessibility_focus_target(element)
```

Nothing about DOM focus changes. The ring appears because the selector now matches. When Orca moves on, the action fires again on the new target and the ring moves with it.

The route that ends up calling that action is:

```
Orca structural nav (H/K/I/L) or Say All iteration
  —> AXEventSynthesizer.scroll_to_center(obj)               [Orca side]
  —> AXText.scroll_substring_to_location(obj)               [added by our monkey patch]
  —> Atspi.Text.ScrollSubstringTo (D-Bus)
  —> AccessibilityInterface::scrollToSubstring              [Qt side]
  —> perform_accessibility_action(..., "scroll_into_view")
```

The monkey patch is necessary because Orca’s stock `scroll_to_center` uses `ScrollSubstringToPoint` — which Qt’s bridge doesn’t route to `QAccessibleTextInterface::scrollToSubstring`. Qt _does_ route `ScrollSubstringTo` (offset-based) — so we patch Orca to also call that (see the _Orca-side monkey patches_ section).

### The Orca script

Embedded as Qt resources and auto-installed on Ladybird startup to `~/.local/share/orca/orca-scripts/Ladybird/` — and only overwritten when the embedded content differs from what’s on disk) — three files:

- **`__init__.py`**: re-exports `Script` so Orca’s script loader can find it.

- **`script.py`**: `Script(web.Script, _QtToolkitScript)`. Overrides `get_toolkit_name()` (returns `"Ladybird"`), `get_utilities()` (returns our `Utilities`), `get_app_key_bindings()` (delegates to `web.Script`), `activate()`, and `on_focused_changed()`.

  The `activate()` override sets the structural navigator’s mode to `NavigationMode.DOCUMENT` on script activation. Without that, `H`/`K`/`L` stay unregistered until the user tabs into a focusable element inside the page — and pressing them on a freshly loaded page would echo the keystroke rather than structurally navigate.

  The `on_focused_changed` override causes browse-mode commands work from the very first document-root Focus event, while also ensuring that typing into the address bar and tabbing through chrome widgets don’t break.

- **`script_utilities.py`** — `Utilities(web.Utilities)`. Class-level overrides: `active_document()`, `get_caret_context()`, nd `_find_first_content_child()`. Also installs several runtime monkey patches at instance-construction time.

### Orca-side monkey patches

Each patch closes a specific gap. All are installed from `Utilities.__init__`, use module-level “patched already?” flags so they only take effect once per Orca session, and wrap `try/except` around both install and run paths so a patch that fails never takes Orca down with it.

1. **Collection fallback** (`_install_collection_fallback_patch`).

   Fixes Qt 6.10’s `Collection::GetMatches` returning empty even when matches exist. Forward-compatible: on Qt 6.11+ the original returns real results and the fallback branch is never taken.

2. **Scroll bridge** (`_install_scroll_patch`).

   Wraps `orca.ax_event_synthesizer.AXEventSynthesizer.scroll_to_center`. Makes the focus ring get painted during structural navigation and Say All.

3. **Say All redirect** (`_install_sayall_patch`).

   Wraps `orca.say_all_presenter.SayAllPresenter.say_all`. Makes Say All pick up where it last left off.

4. **Flat-review zone dedup** (`_install_flat_review_dedup_patch`).

   Wraps `orca.ax_utilities.AXUtilities._get_on_screen_objects`. Prevents Orca for repeating link text in list items.

5. **Flat-review document scope** (`_install_flat_review_document_scope_patch`).

   Wraps `orca.flat_review.Context._get_showing_zones`. Makes Orca’s flat review continue past the bottom of the current viewport.

6. **Flat-review auto-scroll** (`_install_flat_review_scroll_patch`).

   Wraps the cursor-advance methods on `orca.flat_review.Context`. Makes the viewport follow the flat-review cursor the same way it follows Say All.

7. **Hypertext fallback** (`_install_hypertext_fallback_patch`).

   Wraps `orca.ax_hypertext.AXHypertext.find_child_at_offset`, `get_link_start_offset`, `get_link_end_offset`, and `get_character_offset_in_parent`. Makes Say All read link text as expected.

### How the Orca script integrates with Orca

Orca historically loaded per-application scripts from `~/.local/share/orca/orca-scripts/`: the `orca_bin.py` script inserted that directory into `sys.path`, and for each application, [`script_manager`](https://gitlab.gnome.org/GNOME/orca/-/blob/main/src/orca/script_manager.py) tried `importlib.import_module("orca-scripts.<AppName>")` — and, if found, instantiated its `Script` class.

[That mechanism was removed in Orca 49.7](https://gitlab.gnome.org/GNOME/orca/-/commit/5a603529) - on the grounds that customization should go through `orca-customizations.py` instead.

To deal with that Orca change, we still ship our script in the same layout (`~/.local/share/orca/orca-scripts/Ladybird/`) — but re-register it with Orca via a bootstrap block that Ladybird writes into `~/.local/share/orca/orca-customizations.py` on first launch.

That bootstrap adds our `orca-scripts/` directory back to `sys.path` — then monkey-patches `ScriptManager._new_named_script` so that any app whose name matches a subdirectory in `orca-scripts/` loads that user-installed script in preference to a built-in one.

The managed region is delimited by `# --- Ladybird-bootstrap-begin ---`/`# --- Ladybird-bootstrap-end ---` marker lines — so anything the user has in the rest of `orca-customizations.py` is preserved across Ladybird launches.

After that loading occurs, the `Script` class in our `script.py` subclasses one of Orca’s built-in script base classes (`default.Script`, `web.Script`, or a toolkit script).

We use that mechanism in both “sanctioned” and “unsanctioned” ways:

- Sanctioned **subclass overrides** of `Script` (for `on_focused_changed`, etc.) and `Utilities` (for `active_document`, `get_caret_context`, `_find_first_content_child`). Those are documented/supported stable extension points. They’ll keep working as long as Orca preserves its documented script API.

- Unsanctioned **monkey patches** of Orca-internal classes (`AXUtilitiesCollection`, `AXEventSynthesizer`, `SayAllPresenter`, `AXUtilities`). The fact that we can do those is a consequence of Python’s dynamic nature and Orca’s script loader giving us the same address space as Orca itself. It’s _not_ a documented/supported API Orca intentionally exposes.

The two classes of change have different maintenance costs. Subclass overrides move with the script. Monkey patches depend on the names, signatures, and call sites of internal Orca classes — none of which the Orca project makes stability guarantees about.

If Orca renames `AXUtilitiesCollection`, restructures `SayAllPresenter`, or moves `scroll_to_center` out of `AXEventSynthesizer`, our corresponding patch will silently stop applying — and the feature it was working around will silently regress.

Every new Orca release needs the patches re-verified. See _Candidates for upstreaming into Orca itself_ below for which of our patches we believe Orca might accept upstream (and thereby let us delete on our side).

### Qt AT-SPI2 bridge limitations

The accessibility quality we can deliver for Orca users is bounded by `QSpiAccessibleBridge`’s coverage of AT-SPI2. Some gaps are worked around on the Orca side by our script; some can only be fixed by patching Qt itself; some are addressed upstream in Qt 6.11 compared to Qt 6.10; and a few remain as architectural properties of the Orca-plus-Qt combination even after patches.

#### User-visible features vs. what our script recovers

✅ — Our script enables the feature to work as expected.

❌ — Our script cannot recover the feature from the Orca side.

| Feature | | Details |
| ------- | - | ------- |
| Structural navigation (`H`/`K`/`I`/`L`/`B`/`F`/`T`/`R`/—) | ✅ | `_install_collection_fallback_patch` wraps `find_all_with_role` and `find_all_with_role_and_all_states` with a manual DFS fallback when `Collection::GetMatches` returns empty (Qt 6.10 and earlier) |
| `active_document()` for Say All and structural nav | ✅ | `Utilities.active_document()` override tries `super()` first, then cached, then tree-search by role with `is_showing` disambiguation |
| Say All starting in document content on first page load | ✅ | `_install_sayall_patch` redirects `SayAllPresenter.say_all` to the document’s first content child when locus is on chrome |
| Say All resumption after `Esc` | ✅ | Orca keeps locus inside the document during iteration; our tree-exposure rules let Orca’s sentence-extension logic break at paragraph boundaries |
| `get_caret_context()` returning document content instead of window title | ✅ | `Utilities.get_caret_context()` override |
| Focus ring during structural navigation (`H`/`K`/`I`/`L`) | ✅ | `_install_scroll_patch` + the `scroll_into_view` accessibility action + the `:focus-visible` extension described above |
| Flat review not re-reading embedded link text | ✅ | With text-leaf children hidden from AT-SPI2 for every container (matching Firefox’s `EmbeddedObjCollector`), text chunks and inline link zones interleave cleanly in the paragraph’s own zone. |
| Flat review reaching content below the viewport | ✅ | `_install_flat_review_document_scope_patch` expands `_get_showing_zones` so Orca+L keeps going past the last on-screen zone |
| Flat-review cursor auto-scrolling the page | ✅ | `_install_flat_review_scroll_patch` wraps `flat_review.Context.go_next_*`/`go_previous_*` methods — to match the scroll path Say All uses |
| Flat review reading wrapped text as visual lines | ✅ | Per-line `characterRect` plus overrides of `text*Offset` for `LineBoundary` let Orca cluster each paragraph’s chunks and link zones onto their actual visual lines, rather than flattening into one logical line |
| Browse-mode navigation (`H`/`K`/`L`) active from page load | ✅ | `Script.activate()` + `Script.on_focused_changed` set `NavigationMode.DOCUMENT` — so `H`/`K`/`L` work without you needing to manually navigate into a focusable in-page element first |
| Chrome widgets (address bar, tabs) interacting cleanly with structural nav | ✅ | `Script.on_focused_changed` override suspends structural nav for editable chrome, so tabbing through back/forward/tab-strip works |
| `AtkHypertext` / `AtkHyperlink` features — Say All walking into links, link-offset-aware navigation | ✅ | Qt’s bridge exposes no Hypertext/ Hyperlink interfaces. `_install_hypertext_fallback_patch` (re)computes offsets so that Say All can handle links correctly |
| Landmark type distinction | N/A | No patch needed: `xml-roles` object attribute comes from our `QAccessibleAttributesInterface` (C++ side), which works on any Qt 6.8+ |
| Heading-level announcement | N/A | Same: `level` object attribute |
| Orca’s “this is web content” detection | N/A | Same: `tag` object attribute (Orca’s `is_web_element()` checks for it) |
| `AtkDocument` features — document-level locale, MIME metadata | ❌ | Same reason; recoverable via the Qt appendix patch |

#### Qt-bridge bugs, by version

| Qt bridge limitation | Qt 6.10 | Qt 6.11 | Addressed by our Qt patches (appendix)? |
| -------------------- | ------- | ------- | --------------------------------------- |
| `Collection::GetMatches` returns empty | Broken | Fixed | N/A on 6.11; on 6.10, covered by the Collection fallback patch |
| `QSpiDBusCache::GetItems()` returns an empty array | Broken | Broken | Yes — Qt appendix patch 1 |
| `RELATION_EMBEDS` not exposed from window to `WebDocument` child | Missing | Missing | Yes — Qt appendix patch 2 |
| `AtkDocument` interface not exposed on document root | Missing | Missing | Yes — Qt appendix patch 3 (Document portion) |
| `AtkHypertext` / `AtkHyperlink` interfaces not exposed on text containers | Missing | Missing | Worked around from the Orca side via `_install_hypertext_fallback_patch` (enough for Say All link reading and sentence extension). Qt appendix patch 3 is a deeper fix, but it’s blocked by a `libatspi` wrapping bug for full Orca utility |
| `ScrollSubstringToPoint` not routed to `QAccessibleTextInterface::scrollToSubstring` | Broken | Broken | Worked around from the Orca side via `_install_scroll_patch`; a Qt fix would also be welcome |

The only accessibility-related improvement in the Qt 6.11 cycle that materially affects Orca is the `Collection::GetMatches` fix above: 6.11 added a `QSpiMatchRuleMatcher` class (`src/gui/accessible/linux/qspimatchrulematcher.cpp`) that makes `Collection::GetMatches` return real results. On 6.10 the Collection _interface_ was registered but the _method_ returned an empty array — which is why the Orca script carries `_install_collection_fallback_patch`: without it, structural navigation is otherwise entirely dead on 6.10.

#### The AT-SPI2 client-side cache and why `GetItems()` matters

AT-SPI2 defines a `Cache` D-Bus interface with a `GetItems()` method. It has two sides:

- **Server side** (in the application’s process): the AT-SPI2 bridge walks the app’s accessibility tree and returns a bulk snapshot — every object’s D-Bus path, parent, children, supported interfaces, name, role, description, state.

- **Client side** (in Orca’s process): `libatspi` calls `GetItems()` when it first encounters an application, stores the response in a local hash table, and serves subsequent queries for role/name/state/children from that table — **with no D-Bus round-trips**. `AddAccessible` and `RemoveAccessible` signals keep the table in sync as the tree changes.

Firefox and Chromium use `libatk-bridge-2.0` as their AT-SPI2 bridge. Its `GetItems()` returns a fully-populated array — so Orca’s client-side cache works as intended. Qt’s `QSpiDBusCache::GetItems()` returns an _empty_ array. `libatspi` has no cached data; **_every_ query is a D-Bus round-trip** — including for static metadata like role and name.

And that “every query is a D-Bus round-trip” problem is the root cause of the multi-second delay we encounter on first Say All: Orca’s web script builds each line of content by querying role, name, text, states, and attributes on each element — and the Qt architecture, _every_ one of those queries crosses the process boundary. Qt appendix patch 1 addresses that by implementing a real `GetItems()` response.

> [!NOTE]
> Both `libatk-bridge-2.0` and Qt’s `QSpiAccessibleBridge` run inside the application’s process — they are both “in-process” bridges relative to the application. The performance difference is not about _where_ the bridge runs; instead it’s about whether the bridge properly implements `GetItems()`, and thereby populates `libatspi`’s client-side cache in Orca’s process.

#### Limitations that remain even with patches

- **First Say All has a visible startup pause.** Even with `GetItems` populated (Qt patch 1), text and geometry queries (`GetText`, `GetCharacterExtents`, `GetRangeExtents`) are still per-element D-Bus calls, by design; they are too expensive to pre-serialize into the cache reply. Combined with Orca’s own Python and GLib event-loop overhead and `speech-dispatcher` startup, some user-visible delay on the very first Say All will remain. Firefox and Chromium avoid this because their ATK bridge pre-serializes more aggressively.
- **Stale AT-SPI2 proxy references after a tab switch.** Orca’s Python-side caches hold `Atspi.Accessible` proxies keyed by D-Bus object path. When Ladybird switches tabs, our `hideEvent` de-registers the inactive tab’s `QAccessibleInterface` wrappers (so the paths become invalid), and `scrollToSubstring` refuses to operate on non-visible views — but Orca’s Python proxies still point at the old paths.

  Say All on the second tab reads correct content (because `_patched_say_all` looks up the current tab’s document via `active_document()`) — but `scroll_into_view` calls on stale proxies can fail silently, so the focus ring may not appear for the first item after a tab switch. Fixing that would require changes to how Orca manages its proxy cache across tree transitions — not reachable from a Qt-bridge or application-side fix.

#### Orca-side patch lifecycle when Qt 6.11 is the baseline

What should happen to each Orca-side patch once Ladybird’s minimum Qt is 6.11 or later? Short answer: **keep all of them**. Longer answer:

- **`_install_collection_fallback_patch`**: _Keep until Ladybird drops support for Qt < 6.11._ On 6.11+, `Collection::GetMatches` returns real results; the fallback branch is never taken. The wrapper itself stays in the call chain but is dormant. Removing earlier would silently break structural navigation for users still on an older system Qt.
- **`_install_scroll_patch`**: _Keep until Qt adds `ScrollSubstringToPoint` handling._ Qt 6.11’s `atspiadaptor.cpp` still only handles `ScrollSubstringTo`, not `ScrollSubstringToPoint` (verified directly in the 6.11.0 source). Without the extra `scroll_substring_to_location` call, the focus ring during structural nav would stop working on Qt 6.11 too.
- **`_install_sayall_patch`**: _Keep indefinitely._ This patches Orca-side behavior (starting Say All from locus even when locus is on chrome). It is not a Qt-bridge bug; Qt versioning is irrelevant. It stops being needed only if Orca itself changes how it picks the Say All start element.
- **`_install_flat_review_dedup_patch`**: _Keep indefinitely._ This patches Orca-side behavior (flat review zone-per-object without dedup). Qt version is irrelevant.
- **`_install_flat_review_document_scope_patch`**: _Keep indefinitely._ Orca’s default `_get_showing_zones` uses the window rect, which is correct for native toolkit widgets but wrong for web content that scrolls. A Qt-version change won’t affect this. It could become unnecessary if Orca upstream gives web scripts a way to declare a larger review rect.
- **`_install_flat_review_scroll_patch`**: _Keep indefinitely._ Orca’s default flat review doesn’t auto-scroll; Say All does. Closing that gap is Orca-side behavior, not a Qt bug. Could become unnecessary if Orca upstream adopts the same scroll-on-advance behavior for flat review.
- **`_install_hypertext_fallback_patch`**: _Keep until Qt exposes `AtkHypertext`/`AtkHyperlink`_ The fallback is what lets Say All read link text in place. Qt appendix patch 3 addresses the underlying Qt-bridge gap, but a `libatspi` wrapping bug currently blocks full Orca utility even with that patch. So long as Orca relies on `AXHypertext` for hypertext offsets, the fallback stays useful.

The subclass overrides on `Utilities` and `Script` (`active_document`, `get_caret_context`, `_find_first_content_child`, `activate`, `on_focused_changed`) are normal Python OO method overrides on our extension classes, not runtime patches — they stay as long as the extension does.

#### Candidates for upstreaming into Orca itself

Which of the monkey patches would plausibly be accepted upstream by the Orca project, and thereby let us delete the corresponding wrapper on our side? Sorted by priority:

1. **`scroll_to_center` also calls `ScrollSubstringTo`.** Our `_install_scroll_patch` adds a `scroll_substring_to_location` call after the existing `ScrollSubstringToPoint` attempt. The added call is harmless on toolkits that already handle `ScrollSubstringToPoint` — and recovers focus-ring behavior on toolkits (Qt) that don’t. No plausible downside. One-line upstream fix. Upstream source: `orca/ax_event_synthesizer.py::AXEventSynthesizer.scroll_to_center`.

2. **Flat-review auto-scroll.** Our `_install_flat_review_scroll_patch` wraps `flat_review.Context.go_next_*`/`go_previous_*` to scroll the current zone into view after each advance — matching Say All’s existing behavior. Orca-side concern; benefits every toolkit with a scroll-substring action on text interfaces (Firefox and Chromium included). Upstream source: `orca/flat_review.py::Context` advance methods, or `orca/flat_review_presenter.py` equivalents if upstream prefers the Presenter layer.

3. **Say All redirect when `obj` is None.** Our `_install_sayall_patch` makes `SayAllPresenter.say_all` find the active document’s first content child and start from there when called with `obj=None` _and_ locus is outside document content. Benefits any web context where the user invokes Say All without first clicking into the page; happens in Firefox and Chromium too — just less visibly because their AT-SPI2 trees come up faster. Upstream source: `orca/say_all_presenter.py::SayAllPresenter.say_all`.

4. **Flat review zone dedup.** Our `_install_flat_review_dedup_patch` filters zones whose text is contained in an ancestor zone’s text (scoped to leaf-like container ancestors). Orca-side behavior; benefits every browser engine whose leaf-like containers flatten descendant text — Firefox and Chromium have the same architectural setup. Upstream source: `orca/ax_utilities.py::AXUtilities._get_on_screen_objects`.

5. **Flat-review document scope.** Our `_install_flat_review_document_scope_patch` expands `_get_showing_zones` ’ bounding box to cover the web document, not just the viewport. Shape of fix depends on Orca’s intent. Upstream source: `orca/flat_review.py::Context._get_showing_zones`.

6. **Hypertext-offset fallback.** Our `_install_hypertext_fallback_patch` computes offsets from the parent’s text and exposed children when the underlying AT-SPI2 call returns -1/None. Orca could adopt the same fallback unconditionally. Would help any engine on a Qt-bridge-style toolkit — not just Ladybird. Upstream source: `orca/ax_hypertext.py::AXHypertext`.

7. **Collection fallback to manual tree search.** `_install_collection_fallback_patch` wraps `find_all_with_role` and `find_all_with_role_and_all_states`. As Qt 6.11 spreads, the affected case will shrink — but other AT-SPI2 implementations (custom toolkits, embedded environments, older Qt that distros may carry for years) would still benefit. Possible pushback: Orca maintainers may take the position that broken-Collection is a toolkit bug Orca shouldn’t paper over. Upstream source: `orca/ax_utilities_collection.py`.

8. **`active_document()` fallback to tree search.** Same shape of argument and same possible pushback as #7. Tree search is slower than the EMBEDS relation, so the override must be conditional (only fall back if EMBEDS returned nothing). Upstream source: `orca/scripts/web/script_utilities.py::Utilities.active_document`.

Probably not upstreamable:

- **`Script.on_focused_changed()` chrome-widget handling and un-suspend on document focus.** The un-suspend half is structurally similar to what `web.Script` already does — but depends on Ladybird-specific Focus events fired on the document root. The chrome-widget half depends on a clear chrome-vs-document distinction, which is browser-specific and entangled with `web.Script`’s existing focus state machine. Upstream would need a redesign against that state machine.
- **`Script.activate()` setting `NavigationMode.DOCUMENT`.** Needed here only because Ladybird posts a document-root Focus event that `web.Script` short-circuits on. If upstream stopped short-circuiting (or Ladybird stopped relying on the document-root event), the override would be moot.
- **`Utilities.get_caret_context()` fallback.** Possibly upstreamable, but the rationale is entangled with `active_document` and `_find_first_content_child`; untangling for upstream submission would be non-trivial.

The `_install_*_patch` _functions themselves_ (the runtime mechanism that mutates Orca’s classes via `setattr`) are scaffolding that exists only because the underlying fix isn’t upstream yet. Once any of the changes land in Orca itself, the corresponding wrapper becomes a no-op and can be removed.

### Alternative: the private-bridge prototype

Before the current Qt-bridge-plus-Orca-script design was settled on, we tried prototyping a different path: Implement a full ATK bridge ourselves, run it on a private D-Bus instance, and have our Orca script consult that private bus whenever Qt’s built-in bridge let Orca down.

The prototype looked like an attractive way to essentially make us match how every other major Linux browser works — modulo the fact that the other engines don’t need to implement their own private bridges to achieve it. The goal was to work around the three serious gaps in Qt’s bridge on Qt 6.10 that we had run into and that actively harm user experience of web content for Orca users: `Collection::GetMatches` returns empty, `Cache::GetItems` returns empty, and `AtkHypertext`/`AtkHyperlink`/`AtkDocument` aren’t exposed at all. The private bridge seemed like a possible way to close all three gaps at once — because we could use `libatk-bridge-2.0` to populate the cache and route all the interfaces correctly.

#### What the prototype was constituted from

On the Ladybird side, the private-bridge prototype added the following major new chunks of code:

- `PrivateAccessibilityBus` (~280 lines of C++ code). Spawned an isolated `dbus-daemon` for our Orca script to pick up.

- `AtkBridge` (~400 lines of C++ code). `dlopen`ed `libatk-bridge-2.0.so.0` at runtime (matching Firefox’s approach in `accessible/atk/Platform.cpp`), forked an `at-spi2-registryd` child onto the private bus, and ran a dedicated pthread that pumped the bridge’s private `GMainContext`.

- `LadybirdAtkObject` (~1900 lines of C++ code). Per-node GObject/ATK wrapper with dynamic GType registration per interface combination (the pattern Chromium uses in `AXPlatformNodeAuraLinux`). It implemented the full ATK surface Qt doesn’t expose — 11 interfaces: `AtkObject`, `AtkText`, `AtkHypertext`, `AtkHyperlink`, etc.

- `OrcaScripts` (additional ~675 lines of Python code). Connected to the private bus, used it as a fast oracle, and mapped results back to Qt-bridge `Atspi.Accessible` objects. Orca otherwise still navigated Qt-bridge objects normally; the private bus was only ever used for operations the Qt bridge did poorly.

Total additional footprint: ~3500 lines of new C++, Python, and build glue, plus two new runtime dependencies (`at-spi2-core` development headers and `libatk-bridge-2.0` at load time).

#### Why we abandoned the prototype

Four things came up in practice that made the cost-benefit calculation flip:

1. **The bridge needed a registryd on the private bus, not just a daemon** — and the process-lifecycle code we had to implement as as result of that ended up being relatively large and unwieldy.

2. **Private-bus registration didn’t come for free, either** — and relied on an undeclared ABI from a library whose maintainers explicitly don’t consider those symbols stable.

3. **The hoped-for performance win didn’t get delivered:** the very first Say All still had a noticeable startup delay — and ordinary arrow-key reading was already fast enough with the Qt bridge that the prototype didn’t make it noticeably any faster.

4. **We still needed all the same custom runtime-monkey-patching Orca scripts:** Because the private bus in practice could only be used as an _additional oracle_ — not a full replacement — Orca still received events from Qt’s bridge, and still needed the same monkey patches for Say-All-redirect, etc.

So in the end, all the additions — adding ~3500 lines of new code, two extra child processes, dependence on unexported ATK symbols, and no reduction in runtime-monkey-patching by our custom Orca scripts — resulted in neither any real end-user performance benefits, nor any new end-user features beyond what we could _already_ deliver with a far-smaller footprint and far-less-complicated architecture.

## macOS: Browser-engine source code research

The Ladybird accessibility implementation was informed by studying the Blink/Chromium, WebKit/Safari, and Gecko/Firefox accessibility code. On macOS, all three implement the macOS accessibility protocol directly — they create their own Objective-C wrapper objects (not going through a toolkit bridge), giving them full control over role mapping, search predicates, and tree traversal. None of them need “workarounds” for the issues Qt’s bridge creates (see _Qt: shared LadybirdAccessibilityElement on macOS_ above).

The remaining subsections of this section mostly provide macOS-specific details about the platform code for Chromium, Firefox, and WebKit. Information about the Linux platform code for Chromium and Firefox is in other parts of the document.

<details>
<summary><h3>Chromium</h3></summary>
Key files: `ui/accessibility/platform/ax_platform_node_cocoa.h` / `.mm`, `ui/accessibility/platform/one_shot_accessibility_tree_search.h` / `.cc`

**What we adopted:**

1. **IPC serialization model.** Chromium serializes the accessibility tree via Mojo IPC into the browser process. That is the fundamental architecture we adopted.

2. **`OneShotAccessibilityTreeSearch`.** Chromium implements `AXUIElementsForSearchPredicate` via a depth-first tree-search class that skips subtrees based on search criteria. That informed our search-predicate implementation — particularly the concept of “leaf-like” roles whose children aren’t individually navigable.

3. **`AXWebArea` role.** Chromium maps the document role to `@"AXWebArea"` (via `NSAccessibilityWebAreaRole`) — which causes VoiceOver to offer web-specific navigation features (rotor categories, search predicates, text markers). That behavior isn’t documented by Apple; it is inferred from observation.

4. **Custom landmark role descriptions.** Chromium has custom role-description strings for landmarks — rather than relying on `NSAccessibilityRoleDescription`, which doesn’t recognize `AXLandmark*` subrole strings. Landmark subroles are mapped via a static `BuildSubroleMap()` function.

**Where we diverge:**

1. **Base class.** `AXPlatformNodeCocoa` inherits from `NSAccessibilityElement`, not `NSObject`. We tried that but hit property-competition issues (see _“Why NSObject, not NSAccessibilityElement”_ above). Chromium makes `NSAccessibilityElement` work because it has a more elaborate initialization path and implements both the older “informal” protocol and “modern” property API in tandem.

2. **Dual API.** Chromium implements both the older “informal” protocol (`accessibilityAttributeValue:`, `accessibilityAttributeNames`, `accessibilityIsIgnored`) and the “modern” property API (`accessibilityRole`, `accessibilityTitle`, etc.) — with a migration flag to shift between them. We also implement both, but our informal protocol is the source of truth — with modern overrides delegating to it.

3. **Text markers on all elements.** Chromium exposes text-marker parameterized attributes (`AXTextMarkerForPosition`, `AXAttributedStringForTextMarkerRange`, etc.) on all elements — not just the document root. We restrict text markers to the `AXWebArea` element — because we found that exposing them on every element caused VoiceOver to treat structural containers as text content.

4. **No `shouldGroupAccessibilityChildren`.** Chromium doesn’t implement this method at all. We return `NO` on all elements — to prevent VoiceOver from grouping children by screen position, rather than document order.

5. **ListItem not ignored.** Chromium maps `ListItem` to `NSAccessibilityGroupRole` and doesn’t ignore it — it’s a full navigation stop. Chromium’s `OneShotAccessibilityTreeSearch` handles the navigation semantics so that VoiceOver doesn’t double-stop.
</details>

<details>
<summary><h3>WebKit</h3></summary>
Key files: `Source/WebCore/accessibility/mac/WebAccessibilityObjectWrapperMac.mm`, `Source/WebCore/accessibility/mac/AccessibilityObjectMac.mm`, `Source/WebCore/accessibility/AXObjectCache.h`

**What we adopted:**

1. **Coordinate system.** By comparing against Safari’s element bounds in Accessibility Inspector, we confirmed that CSS pixels equal points on macOS — no device-pixel-ratio scaling is needed.

2. **Landmark role descriptions.** Safari provides custom role-description strings for landmarks — confirming that `NSAccessibilityRoleDescription()` doesn’t recognize `AXLandmark*` subrole strings.

**Architecture notes:**

1. **Private API.** WebKit uses Apple’s private `NSAccessibilityRemoteUIElement` Mach-port token-exchange API for cross-process accessibility. We chose not to use that approach.

2. **Search predicates.** `handleUIElementsForSearchPredicateAttribute()` parses VoiceOver’s search dictionary into an `AccessibilitySearchCriteria` struct and delegates to a multi-frame search engine. The search is more sophisticated than ours — it handles cross-frame searches and a wider set of search keys.

3. **Lazy cache.** `AXObjectCache` creates accessibility wrapper objects on demand via `getOrCreate()` and maps them via multiple lookup tables (Node → AXID, RenderObject → AXID). Objects are removed when the DOM node is destroyed.
</details>

<details>
<summary><h3>Firefox</h3></summary>
Key files: `accessible/mac/mozAccessible.h` / `.mm`, `accessible/mac/MOXAccessibleBase.h` / `.mm`, `accessible/mac/MOXSearchInfo.mm`

**What we adopted:**

1. **Same IPC model as Chromium.** Firefox serializes the accessibility tree from the content process to the parent process via IPDL IPC — the de facto standard for multi-process browsers.

2. **Pre-order depth-first traversal.** Firefox’s `Pivot` class (`accessible/base/Pivot.h` / `.cpp`) implements pre-order DFS for search predicates. `Pivot` is a stack-allocated tree searcher that takes a root accessible and traverses with `Next()`/`Prev()` using `PivotRule` subclasses. `MOXSearchInfo` creates specific `RotorRule` subclasses (`RotorRoleRule`, `RotorLinkRule`, `RotorControlRule`, etc. in `accessible/mac/RotorRules.h`) for each VoiceOver search key (`AXHeadingSearchKey`, `AXLinkSearchKey`, etc.).

3. **Search predicate parameters.** Firefox’s `MOXSearchInfo` processes `AXDirection`, `AXStartElement`, and `AXResultsLimit` from the search-predicate dictionary — the same fields we handle.

**Where Firefox aligns with us (and differs from Chromium):**

1. **NSObject base class.** Firefox’s `mozAccessible` inherits from `NSObject` (via `MOXAccessibleBase`), not `NSAccessibilityElement`. That is the same choice we made — suggesting the `NSAccessibilityElement` base-class issues are a known difficulty.

2. **Modern protocol as primary API.** Firefox defines a `MOXAccessibleProtocol` with property-based getters (`moxRole`, `moxSubrole`, `moxTitle`, etc.) and bridges the older informal protocol methods to them. The protocol serves as an internal abstraction layer.

**Where we diverge from Firefox:**

1. **Text markers on all elements.** Like Chromium, Firefox exposes text-marker support on all elements (via `moxTextMarkerDelegate`), not just the root. Our restriction to `AXWebArea` is specific to our implementation.

2. **ListItem as a dedicated class.** Firefox has a `MOXListItemAccessible` subclass with specific overrides (e.g., `moxTitle` returns empty to avoid verbose announcements). ListItems aren’t ignored.

3. **No `shouldGroupAccessibilityChildren`.** Firefox doesn’t implement this, same as Chromium.
</details>

### Key cross-engine observations for Chromium, WebKit, and Firefox on macOS

- All three engines implement NSAccessibility directly on their own wrapper objects. None go through a toolkit bridge.
- All three map the document root to `AXWebArea`.
- All three provide custom role-description strings for landmarks.
- All three implement `AXUIElementsForSearchPredicate` directly.
- Chromium and Firefox both serialize the tree to the UI process; only WebKit does that differently (via private `NSAccessibilityRemoteUIElement` API).
- Firefox uses `NSObject` as the base class (like us); Chromium uses `NSAccessibilityElement`.
- Both Chromium and Firefox expose text markers on all elements; we restrict them to the document root.
- Neither Chromium nor Firefox implements `shouldGroupAccessibilityChildren`.

### Why our macOS implementation diverges from Firefox/Chromium/WebKit implementations

The divergence of Ladybird’s macOS accessibility implementation and that of other browsers isn’t caused by the IPC model — Chromium and Firefox also serialize over IPC; instead, the divergence stems from differences in wrapper-object lifecycle and tree management:

1. **Snapshot vs. live tree.** Our macOS NSAccessibility wrappers are thin shells over a full-tree snapshot that is replaced wholesale on each update. Chromium and Firefox back their wrappers with rich, live object models that support incremental updates and on-demand attribute computation.

2. **Purpose-built traversal.** Chromium’s `OneShotAccessibilityTreeSearch` and Firefox’s `Pivot` are sophisticated tree-traversal engines with predicate matching, subtree skipping, and cross-frame support. Our search predicate is a simpler DFS that required explicit leaf-role and container-descendant handling to avoid double-reading.

3. **Years of iteration.** For macOS, Chromium and Firefox have accumulated extensive VoiceOver interaction debugging over many years. Constraints we discovered through testing (text markers restricted to `AXWebArea`, `shouldGroupAccessibilityChildren` returning `NO`, `NSAccessibilityElement` property competition) are the kind of platform-specific quirks that the other engines must have either solved structurally — or else, because of their architecture — maybe never encountered to begin with.

### AccessKit

[AccessKit](https://github.com/AAccessKit/accesskit) is a Rust library that provides cross-platform accessibility adapters for macOS (NSAccessibility), Linux (AT-SPI2), and Windows (UIA). It handles the platform plumbing — role mapping, element lifecycle, event notifications, coordinate conversion — so applications don’t have to implement each platform’s accessibility protocol directly.

Key files: `platforms/macos/src/node.rs` (role mapping, attributes, actions), `platforms/macos/src/context.rs` (element caching), `platforms/macos/src/event.rs` (notifications), `consumer/src/filters.rs` (tree filtering).

<details><summary><b>What AccessKit handles well:</b></summary>
1. **Role mapping.** ~140 roles including `AXWebArea` for web content, landmark subroles (`AXLandmarkNavigation`, `AXLandmarkMain`, etc.), and custom role descriptions.

2. **`NSAccessibilityElement` base class.** AccessKit successfully uses `NSAccessibilityElement` as its base class (we switched to `NSObject` due to property-competition issues; Firefox also uses `NSObject`).

3. **Incremental tree updates.** `Tree::update_and_process_changes()` supports partial updates, unlike our full-snapshot replacement.

4. **Filtering model.** `FilterResult::Include` / `ExcludeNode` / `ExcludeSubtree` is cleaner than `accessibilityIsIgnored` — `ExcludeNode` promotes children without needing platform-specific ignore swizzles.

5. **Element lifecycle.** HashMap cache with on-demand creation, proper cleanup, and `UIElementDestroyedNotification` on drop.

6. **Focus forwarding.** Patches NSWindow to forward `accessibilityFocusedUIElement` to the content view — solving the same initial-focus problem we solved with `makeFirstResponder`.

7. **Cross-platform.** A single tree representation serves macOS, Linux (AT-SPI2), and Windows (UIA). The Linux adapter would give us AT-SPI2 support without relying on Qt’s bridge.
</details>

<details>
<summary><b>What AccessKit does not provide for web content:</b></summary>
1. **No `AXUIElementsForSearchPredicate`.** Without search predicates, VoiceOver falls back to sibling-only navigation within groups — it cannot do VO+Right/Left linear navigation through web content. That is the most complex part of our implementation, and even if we used AccessKit, we would still need to build that part ourselves.

2. **No `AXTextMarker`.** No cursor-level text navigation. AccessKit has NSRange-based text attributes but not the native marker objects that web content requires for character-by-character navigation and text selection.

3. **No web-content navigation semantics.** AccessKit is designed for native app accessibility, not browser-engine accessibility. The DFS traversal logic (leaf-role skipping, container-descendant skipping, search-key filtering) that makes VoiceOver navigation work smoothly in web content appears not to be part of AccessKit  — and that seems like a critical deficiency.
</details>

**Assessment:** AccessKit would eliminate some of the relatively low-implementation-effort platform plumbing we’re doing — but not the hard parts. The web-specific code — search predicates, text markers, container-descendant skipping, ignored-element semantics for web content — would still need to be implemented on top. The biggest win would have been for the Qt port, where AccessKit could have replaced the swizzles on `QMacAccessibilityElement` with a proper native accessibility layer — but we already reduced the swizzles from seven to three by sharing `LadybirdAccessibilityElement` between AppKit and Qt. For the AppKit port, the gain is more modest — since `LadybirdAccessibilityElement` already implements NSAccessibility directly. So the trade-off would be adding a Rust build dependency for what amounts to a cleaner version of the platform-plumbing layer. But even then, with AccessKit, it seems we’d still be lacking the DFS traversal logic (leaf-role skipping, container-descendant skipping, search-key filtering) that makes VoiceOver navigation work smoothly in web content. And that on its own is a critical deficiency that would make AccessKit unsuitable for our needs.

### Future work: search-key filtering on macOS

Our macOS search-predicate implementation returns all non-ignored elements in DFS order without filtering by the `AXSearchKey` parameter. VoiceOver sends search keys like `AXHeadingSearchKey`, `AXLinkSearchKey`, `AXLandmarkSearchKey`, etc., to request elements of a specific type. Currently VoiceOver handles the filtering itself, but implementing search-key filtering on our side would improve performance and correctness.

Chromium’s `OneShotAccessibilityTreeSearch` (`ui/accessibility/platform/one_shot_accessibility_tree_search.h` / `.cc`) uses `PredicateForSearchKey()` to map each VoiceOver search key to a C++ predicate function, then walks the tree returning only matching elements. Firefox’s `MOXSearchInfo` (`accessible/mac/MOXSearchInfo.mm`) does the same — via `PivotRule` subclasses. Both approaches avoid returning the entire flattened tree when VoiceOver only wants headings or links.

## Tests

Four categories of tests cover four different layers: tree-dump tests, Qt AT-SPI2 bridge tests, Orca-integration tests, and macOS NSAccessibility tests.

### Tree-dump tests (`Tests/LibWeb/Accessibility/`)

An added Accessibility test mode in `test-web` follows the same basic pattern as Layout and Text tests:

- Tests are HTML files in `Tests/LibWeb/Accessibility/input/`
- Expectations files are in `Tests/LibWeb/Accessibility/expected/`
- The expectations tree-dump format shows the raw tree structure, including ignored elements and text leaves.
- You can use `test-web --rebaseline` to generate/update the expectations files.

These catch role/name/ancestor regressions observable in the serialized data. They do _not_ catch what our Qt AT-SPI2 bridge emits over D-Bus (role mapping, U+FFFC placement, per-character rects, event timing, action routing, multi-tab lifecycle), nor what the macOS NSAccessibility wrapper exposes at runtime, nor what Orca or VoiceOver output.

### Qt AT-SPI2 bridge tests (`Tests/LibWeb/AccessibilityBridge/tests/`)

Python tests that launch a real Ladybird process on a fixture HTML page, connect as an AT-SPI2 client via `gi.repository.Atspi`, and assert on what the bridge actually exposes. These catch bridge-level regressions we can’t see just with a tree dump

### Orca-integration tests (`Tests/LibWeb/AccessibilityBridge/tests_orca/`)

Python tests that import Orca and our Ladybird script into the test process, instantiate the script against the live Ladybird AT-SPI2 accessible, monkey-patch `speech.speak`, drive Orca commands directly, and assert on what Orca would have spoken.

### macOS NSAccessibility tests (`Tests/LibWeb/AccessibilityBridgeMac/`)

Python tests that launch a Ladybird process on a fixture HTML page, connect as an NSAccessibility client via PyObjC + `AXUIElement`, and assert on what the macOS AppKit accessibility surface actually exposes — and on what VoiceOver would see while walking that surface.

> [!NOTE]
> We have no VoiceOver tests that correspond to our Orca tests — because VoiceOver is a closed binary, and there’s no public hook for capturing synthesized speech without installing a custom TTS driver.

Fixtures are reused from `Tests/LibWeb/AccessibilityBridge/input/` rather than duplicated — since the role/name/structure invariants those fixtures exercise are platform-agnostic.

Running the tests requires the Accessibility (TCC) permission to be granted to the calling shell/IDE in System Settings → Privacy & Security → Accessibility.

## File guide

### `Libraries/LibWeb/CSS/`

`SelectorEngine.cpp`
- Extends the `:focus-visible` pseudo-class match to also match when the element is the document’s _accessibility focus target_ (set by the accessibility `scroll_into_view` action, described under “Qt on Linux” above). That gives keyboard and screen-reader navigation the same focus-ring behavior without actually moving DOM focus.

### `Libraries/LibWeb/DOM/`

`AccessibilityTreeNode.cpp` / `AccessibilityTreeNode.h`
- Walks the existing GC-managed accessibility tree and produces a flat `Vector<AccessibilityNodeData>`. Extracts role, name, description, bounds, heading level, `aria-live` value, and focus state for each node. For text-leaf nodes, also populates per-character layout data.

`Document.cpp` / `Document.h`
- Builds the accessibility tree and then serializes it. Notifies the page client when the focused element changes.
- Has a mechanism for driving Orca focus-ring display movement without moving DOM focus.

### `Libraries/LibWeb/Page/Page.h`
- Has a mechanism for notifying the UI process when focus changes.

### `Libraries/LibWebView/`

`AccessibilityNodeData.cpp` / `AccessibilityNodeData.h`
- IPC-serializable struct that represents one node in the accessibility tree. Contains id, parent/child IDs, role, name, description, value, bounds, focus/disabled state, heading level, and `aria-live` value — along with the IPC `encode`/`decode` specializations.

`AccessibilityTreeManager.cpp` / `AccessibilityTreeManager.h`
- Platform-agnostic class that caches the accessibility tree in the UI process. Provides node lookup, hit-testing, focused-node tracking, and DFS-ordered text-leaf enumeration. On `update_tree()`, compares old and new trees — to detect `aria-live` region content changes.

`ViewImplementation.cpp` / `ViewImplementation.h`
- Has `request_accessibility_tree()` and `perform_accessibility_action()` methods, plus `on_accessibility_tree_received` and `on_accessibility_focus_changed` callbacks.

`WebContentClient.cpp` / `WebContentClient.h`
- Client-side IPC handlers for `did_get_accessibility_tree` and `did_accessibility_focus_change` messages from WebContent. Dispatches to the `ViewImplementation` callbacks.

`Forward.h`
- Forward declaration for `AccessibilityNodeData`.

`PageInfo.h`
- Adds `AccessibilityTree` to the `PageInfoType` enum (used by `test-web` for accessibility tree dumps).

### `Services/WebContent/`

`ConnectionFromClient.cpp` / `ConnectionFromClient.h`
- Builds and sends the full tree, and has `perform_accessibility_action` — which looks up DOM nodes by `UniqueNodeID` and dispatches to the `focus` and `scroll_into_view` action handlers. Also implements the `AccessibilityTree` page-info dump for `test-web`.

`PageClient.cpp` / `PageClient.h`
- Sends focus-change IPC and pushes tree updates when the page changes.

`WebContentServer.ipc`
- Has `request_accessibility_tree(page_id)` and `perform_accessibility_action(page_id, node_id, action)` endpoints.

`WebContentClient.ipc`
- Has `did_get_accessibility_tree(page_id, nodes)` and `did_accessibility_focus_change(page_id, node_id)` endpoints.

### `UI/AppKit/Interface/`

`LadybirdAccessibilityElement.h` / `LadybirdAccessibilityElement.mm`
- The main NSAccessibility wrapper. Handles role mapping, ignored-element transparency, search-predicate navigation, hit testing, actions, table-navigation attributes, text-marker navigation, and coordinate conversion. Also exposes the extra attributes VoiceOver uses to anchor its cursor on page load.

`LadybirdAccessibilityViewProtocol.h`
- Cross-port protocol implemented by `LadybirdWebView` (AppKit) and `WebContentAccessibilityView` (Qt-on-macOS). Exposes the methods `LadybirdAccessibilityElement` calls back into the host view for: element lookup, screen/view coordinate conversion, action dispatch, page URL, and first-responder state.

`LadybirdWebView.h` / `LadybirdWebView.mm`
- Integration point on macOS. Owns the `AccessibilityTreeManager` and element cache. Wires up IPC callbacks. Ensures VoiceOver moves its cursor into the web content on page load. Also posts layout-changed notifications and live-region-announcement notifications.

### `UI/Qt/` (macOS-specific files)

`WebContentViewAccessibility.h` / `WebContentViewAccessibility.mm` (macOS only)
- NSView overlay conforming to `LadybirdAccessibilityViewProtocol`. Handles tree-update notifications, focus changes, and live-region announcements. Ensures the shared `LadybirdAccessibilityElement` can answer `AXURL` and `AXFocused` on the `AXWebArea`.

### `UI/Qt/` (Linux files)

`AccessibilityInterface.h` / `AccessibilityInterface.cpp`
- `QAccessibleInterface` implementation. Maps `AccessibilityNodeData` nodes to Qt’s accessibility API. Also contains `WebContentViewAccessible`: a `QAccessibleWidget` that bridges the `WebContentView` widget into Qt’s accessibility tree.

`Application.cpp`
- Enables loading of our custom Orca script, suppresses some log messages that end up flooding the console when Orca’s in use, and fixes a problem that prevents Orca and other AT clients from seeing our accessibility tree.

`WebContentView.h` / `WebContentView.cpp`
- Owns the `AccessibilityTreeManager` and element cache. Sets up IPC callbacks and debounced tree requests. Posts the document-root accessibility focus event. Rewires `QAccessibleTextInterface::scrollToSubstring` to the `scroll_into_view` accessibility action.

`Tab.cpp`
- Calls `schedule_accessibility_tree_request()` in `on_load_start`, `on_url_change`, and `on_title_change` callbacks (which overwrite `WebContentView`’s callbacks).

### `UI/Qt/OrcaScripts/Ladybird/`

Custom Orca screen-reader scripts, embedded as Qt resources and auto-installed to `~/.local/share/orca/orca-scripts/Ladybird/` on startup. A bootstrap block written into `~/.local/share/orca/orca-customizations.py` re-registers that directory with Orca’s script manager (required due to Orca 49.7 having removed the loading mechanism it had previously supported).

`script.py`
- Extends Orca’s `web.Script` with overrides for `activate()` and `on_focused_changed()`.

`script_utilities.py`
- Extends Orca’s `web.Utilities` with overrides for `active_document()`, `get_caret_context()`, and `_find_first_content_child()`. Installs several runtime monkey patches (described in the “Qt on Linux” section).

`__init__.py`
- `from .script import Script`

## Appendix: Qt AT-SPI2 bridge patches

The patches shown in this section are against Qt 6.11.0 (`src/gui/accessible/linux/`) and complement the “Qt-bridge bugs, by version” section above. They’ve not been upstreamed to the Qt sources; instead they’re included here as a way of having a record of them until they can do get upstreamed (at which point this appendix should be removed).

<details>
<summary><h3>How to use these patches</h3></summary>
Here is a full sequence of instructions creating a Ladybird build linked against Qt 6.11.0 with our patches applied.

1. **Get the Qt 6.11.0 sources.** Either fetch the official source archive and extract it:

   ```sh
   curl -LO https://download.qt.io/official_releases/qt/6.11/6.11.0/single/qt-everywhere-src-6.11.0.tar.xz
   tar xf qt-everywhere-src-6.11.0.tar.xz
   ```

   Or clone qtbase only (sufficient for these patches, since they only touch `src/gui/accessible/linux/`):

   ```sh
   git clone --branch v6.11.0 --depth 1 https://code.qt.io/qt/qtbase.git qtbase-6.11.0-src
   ```

2. **Apply the patches.** Save each fenced `diff` block from this appendix to its own file (e.g. `patch1.diff`, `patch2.diff`, `patch3.diff`); then, from inside the qtbase source tree, run:

   ```sh
   patch -p1 < /path/to/patch1.diff
   patch -p1 < /path/to/patch2.diff
   patch -p1 < /path/to/patch3.diff
   ```

3. **Configure and build Qt.** AT-SPI2 support requires `pkg-config` and the AT-SPI2 development package — `libatspi-dev` on Debian/Ubuntu, `at-spi2-core-devel` on Fedora — to be installed before configuring. Then, from a fresh build directory next to the source tree:

   ```sh
   mkdir build-qt-6.11-patched
   cd build-qt-6.11-patched
   ../qtbase-6.11.0-src/configure -accessibility -feature-accessibility-atspi-bridge -prefix /opt/qt-6.11.0-patched
   cmake --build . --parallel
   cmake --install .
   ```

   The `accessibility` and `accessibility-atspi-bridge` features are on by default if their dependencies are present; passing them explicitly is just to fail loudly if anything is missing.

4. **Build Ladybird against the patched Qt.** Set `CMAKE_PREFIX_PATH` so Ladybird’s `find_package(Qt6 REQUIRED COMPONENTS Core Widgets)` (in `UI/Qt/CMakeLists.txt`) discovers the patched build, then run the normal build entry point:

   ```sh
   CMAKE_PREFIX_PATH=/opt/qt-6.11.0-patched ./Meta/ladybird.py build
   ```

   Equivalent forms:

   ```sh
   # Pointing CMake directly at the Qt6 cmake config dir:
   Qt6_DIR=/opt/qt-6.11.0-patched/lib/cmake/Qt6 ./Meta/ladybird.py build

   # If the patched Qt was installed over an aqtinstall layout at $HOME/Qt/6.11.0:
   CMAKE_PREFIX_PATH=$HOME/Qt/6.11.0 ./Meta/ladybird.py build
   ```

   The environment variable only takes effect on the first configure pass — `Meta/ladybird.py` short-circuits configure if `Build/release/build.ninja` already exists, and CMake then uses the cached Qt path on subsequent builds. To switch an existing build directory from system Qt to the patched Qt, delete it first:

   ```sh
   rm -rf Build/release
   CMAKE_PREFIX_PATH=/opt/qt-6.11.0-patched ./Meta/ladybird.py build
   ```

5. **Verify the link.** Confirm Ladybird picked up the patched Qt rather than the system one:

   ```sh
   ldd Build/release/bin/Ladybird | grep libQt6Gui
   ```

   The output should reference `libQt6Gui.so.6` under your install prefix (`/opt/qt-6.11.0-patched/lib/...` in the example above) — not a system path such as `/usr/lib/...`.
</details>

<details>
<summary><h3>Patch 1: QSpiDBusCache::GetItems populates the AT-SPI2 cache</h3></summary>
Addresses the _“`QSpiDBusCache::GetItems()` returns an empty array”_ limitation. Firefox and Chromium pre-populate the AT-SPI2 cache through `libatk-bridge-2.0`, so that Orca can do its initial tree walk locally; Qt’s stub returns an empty array — forcing Orca to make per-element D-Bus calls for every node it needs to inspect. This patch walks the active `QAccessibleInterface` tree from `qApp` and returns a populated `QSpiAccessibleCacheArray` with path, parent, children, supported interfaces, name, role, description, and state for each accessible.

```diff
diff --git a/src/gui/accessible/linux/qspidbuscache_p.h b/src/gui/accessible/linux/qspidbuscache_p.h
index 7a6e111f..7a960f25 100644
--- a/src/gui/accessible/linux/qspidbuscache_p.h
+++ b/src/gui/accessible/linux/qspidbuscache_p.h
@@ -39,6 +39,13 @@ Q_SIGNALS:
 
 public Q_SLOTS:
     QSpiAccessibleCacheArray GetItems();
+
+private:
+    void collectAccessibles(QAccessibleInterface *interface,
+                            QSpiAccessibleCacheArray &items);
+    QSpiAccessibleCacheItem buildCacheItem(QAccessibleInterface *interface);
+
+    QDBusConnection m_connection;
 };
 
 QT_END_NAMESPACE
diff --git a/src/gui/accessible/linux/qspidbuscache.cpp b/src/gui/accessible/linux/qspidbuscache.cpp
index 4d7463ad..ce188c12 100644
--- a/src/gui/accessible/linux/qspidbuscache.cpp
+++ b/src/gui/accessible/linux/qspidbuscache.cpp
@@ -6,7 +6,12 @@
 #include "qspiaccessiblebridge_p.h"
 
 #if QT_CONFIG(accessibility)
+#include "atspiadaptor_p.h"
 #include "cache_adaptor.h"
+#include "qspi_constant_mappings_p.h"
+
+#include <QtGui/qaccessible.h>
+#include <QtGui/qguiapplication.h>
 
 #define QSPI_OBJECT_PATH_CACHE "/org/a11y/atspi/cache"
 
@@ -28,12 +33,10 @@ using namespace QtGuiPrivate; // for D-Bus accessibility wrappers
 
     Additionally the AddAccessible and RemoveAccessible signals
     are responsible for adding/removing objects from the cache.
-
-    Currently the Qt bridge chooses to ignore these.
 */
 
 QSpiDBusCache::QSpiDBusCache(QDBusConnection c, QObject* parent)
-    : QObject(parent)
+    : QObject(parent), m_connection(c)
 {
     new CacheAdaptor(this);
     c.registerObject(QSPI_OBJECT_PATH_CACHE ""_L1, this, QDBusConnection::ExportAdaptors);
@@ -49,9 +52,85 @@ void QSpiDBusCache::emitRemoveAccessible(const QSpiObjectReference& item)
     emit RemoveAccessible(item);
 }
 
+static QString cachePathForInterface(QAccessibleInterface *interface)
+{
+    if (!interface || !interface->isValid())
+        return QString::fromLatin1(ATSPI_DBUS_PATH_NULL);
+    if (interface->role() == QAccessible::Application)
+        return QString::fromLatin1(ATSPI_DBUS_PATH_ROOT);
+    QAccessible::Id id = QAccessible::uniqueId(interface);
+    return QSPI_OBJECT_PATH_PREFIX ""_L1 + QString::number(id);
+}
+
+QSpiAccessibleCacheItem QSpiDBusCache::buildCacheItem(QAccessibleInterface *interface)
+{
+    QSpiAccessibleCacheItem item;
+
+    const QString path = cachePathForInterface(interface);
+    item.path = QSpiObjectReference(m_connection, QDBusObjectPath(path));
+
+    item.application = QSpiObjectReference(m_connection,
+                           QDBusObjectPath(QString::fromLatin1(ATSPI_DBUS_PATH_ROOT)));
+
+    QAccessibleInterface *parent = interface->parent();
+    if (parent)
+        item.parent = QSpiObjectReference(m_connection,
+                          QDBusObjectPath(cachePathForInterface(parent)));
+    else
+        item.parent = QSpiObjectReference(m_connection,
+                          QDBusObjectPath(QString::fromLatin1(ATSPI_DBUS_PATH_NULL)));
+
+    const int childCount = interface->childCount();
+    item.children.reserve(childCount);
+    for (int i = 0; i < childCount; ++i) {
+        QAccessibleInterface *child = interface->child(i);
+        item.children.append(QSpiObjectReference(m_connection,
+                                 QDBusObjectPath(cachePathForInterface(child))));
+    }
+
+    item.supportedInterfaces = AtSpiAdaptor::accessibleInterfaces(interface);
+    item.name = interface->text(QAccessible::Name);
+    item.role = static_cast<uint>(AtSpiAdaptor::getRole(interface));
+    item.description = interface->text(QAccessible::Description);
+
+    quint64 spiState = spiStatesFromQState(interface->state());
+    if (interface->tableInterface())
+        setSpiStateBit(&spiState, ATSPI_STATE_MANAGES_DESCENDANTS);
+    QAccessible::Role role = interface->role();
+    if (role == QAccessible::TreeItem || role == QAccessible::ListItem)
+        setSpiStateBit(&spiState, ATSPI_STATE_TRANSIENT);
+    item.state = spiStateSetFromSpiStates(spiState);
+
+    return item;
+}
+
+void QSpiDBusCache::collectAccessibles(QAccessibleInterface *interface,
+                                        QSpiAccessibleCacheArray &items)
+{
+    if (!interface || !interface->isValid())
+        return;
+
+    items.append(buildCacheItem(interface));
+
+    if (interface->tableInterface())
+        return;
+
+    const int childCount = interface->childCount();
+    for (int i = 0; i < childCount; ++i)
+        collectAccessibles(interface->child(i), items);
+}
+
 QSpiAccessibleCacheArray QSpiDBusCache::GetItems()
 {
-    return QSpiAccessibleCacheArray();
+    QSpiAccessibleCacheArray items;
+
+    QAccessibleInterface *root = QAccessible::queryAccessibleInterface(qApp);
+    if (!root || !root->isValid())
+        return items;
+
+    collectAccessibles(root, items);
+
+    return items;
 }
 
 QT_END_NAMESPACE
```
</details>

<details>
<summary><h3>Patch 2: Expose EMBEDS relation from window to WebDocument child</h3></summary>
Addresses the _“No `RELATION_EMBEDS` from window to `WebDocument` child”_ limitation. Orca’s `Utilities.active_document()` normally walks the AT-SPI2 EMBEDS relation from the active window to its embedded web document; stock Qt doesn’t expose that relation, so `active_document()` returns `None` and Orca falls back to its own tree search (which we also duplicate on the Ladybird side in `script_utilities.py`, for robustness).

This patch extends `AtSpiAdaptor::relationSet()` so that for window-class accessibles (`QAccessible::Window` / `Grouping` / `Pane`) it walks up to five levels of children looking for a `QAccessible::WebDocument` — and if found, appends an `ATSPI_RELATION_EMBEDS` entry pointing at that document’s D-Bus path. That makes `Utilities.active_document()`’s fast path succeed on Qt-based browsers without requiring the Orca-side fallback.

```diff
diff --git a/src/gui/accessible/linux/atspiadaptor.cpp b/src/gui/accessible/linux/atspiadaptor.cpp
index dae83437..6c53219c 100644
--- a/src/gui/accessible/linux/atspiadaptor.cpp
+++ b/src/gui/accessible/linux/atspiadaptor.cpp
@@ -1825,6 +1860,35 @@ QSpiRelationArray AtSpiAdaptor::relationSet(QAccessibleInterface *interface,
         if (!related.isEmpty())
             relations.append(QSpiRelationArrayEntry(qAccessibleRelationToAtSpiRelation(pair.second), related));
     }
+    // Add EMBEDS relation for frames/windows containing a WebDocument.
+    // Orca's active_document() uses this to find the web content.
+    if (interface->role() == QAccessible::Window
+        || interface->role() == QAccessible::Grouping
+        || interface->role() == QAccessible::Pane) {
+        for (int i = 0; i < interface->childCount(); ++i) {
+            QAccessibleInterface *child = interface->child(i);
+            if (!child)
+                continue;
+            // Walk down to find WebDocument (may be nested)
+            QList<QAccessibleInterface*> stack;
+            stack.append(child);
+            while (!stack.isEmpty()) {
+                auto *c = stack.takeLast();
+                if (c->role() == QAccessible::WebDocument) {
+                    QSpiObjectReferenceArray related;
+                    related.append(QSpiObjectReference(connection, QDBusObjectPath(pathForInterface(c))));
+                    relations.append(QSpiRelationArrayEntry(ATSPI_RELATION_EMBEDS, related));
+                    stack.clear();
+                    break;
+                }
+                for (int j = 0; j < c->childCount() && j < 5; ++j) {
+                    if (auto *gc = c->child(j))
+                        stack.append(gc);
+                }
+            }
+        }
+    }
+
     return relations;
 }
 
```
</details>

<details>
<summary><h3>Patch 3: Expose AT-SPI2 Document, Hypertext, and Hyperlink interfaces</h3></summary>
Addresses the _“`AtkDocument` interface not exposed” and “`AtkHypertext` / `AtkHyperlink` interfaces not exposed”_ limitations. Together, those three interfaces let Orca treat a web document as a true hypertext container — with per-link offsets, link enumeration, document-level locale, and MIME attributes — rather than just a tree of text leaves and elements.

Three separable additions in one patch:

- **`org.a11y.atspi.Document`**: minimal stub in `handleMessage()` that answers `GetLocale`, `GetAttributeValue`, and `GetAttributes` queries (the latter reports `DocType` / `MimeType` = `text/html`). `accessibleInterfaces()` advertises this interface on `QAccessible::WebDocument` accessibles so Orca finds it.
- **`org.a11y.atspi.Hypertext`**: new `hypertextInterface()` method answering `GetNLinks`, `GetLink`, and `GetLinkIndex` by walking the accessible’s child list and treating non-`StaticText` children as embedded “links” in hypertext terms. Note that `accessibleInterfaces()` does _not_ advertise Hypertext — see the comment in the diff: `libatspi`’s client wraps `GetLink` results as `Hyperlink` objects, and `GetObject` on those paths returns `Hyperlink` instead of `Accessible`, breaking Orca’s subsequent queries. The method is reachable via direct D-Bus probing by assistive technologies that don’t rely on `libatspi`’s Hypertext wrapping.
- **`org.a11y.atspi.Hyperlink`**: new `hyperlinkInterface()` method answering `NAnchors`, `GetObject`, `GetURI`, `IsValid`, and start/end index queries. Start/end indices are computed by walking siblings and summing `StaticText` character counts and U+FFFC object-replacement-character widths for non-text siblings.

The Document stub is clean and should be straightforward to upstream. Hypertext/Hyperlink is partial for the reason above — the `libatspi` interaction needs to be resolved before this is useful to `libatspi`-based assistive technologies, including Orca.

```diff
diff --git a/src/gui/accessible/linux/atspiadaptor_p.h b/src/gui/accessible/linux/atspiadaptor_p.h
index 2b25fe57..57035b6a 100644
--- a/src/gui/accessible/linux/atspiadaptor_p.h
+++ b/src/gui/accessible/linux/atspiadaptor_p.h
@@ -83,6 +83,8 @@ private:
     bool selectionInterface(QAccessibleInterface *interface, const QString &function, const QDBusMessage &message, const QDBusConnection &connection);
     bool tableInterface(QAccessibleInterface *interface, const QString &function, const QDBusMessage &message, const QDBusConnection &connection);
     bool tableCellInterface(QAccessibleInterface *interface, const QString &function, const QDBusMessage &message, const QDBusConnection &connection);
+    bool hypertextInterface(QAccessibleInterface *interface, const QString &function, const QDBusMessage &message, const QDBusConnection &connection);
+    bool hyperlinkInterface(QAccessibleInterface *interface, const QString &function, const QDBusMessage &message, const QDBusConnection &connection);
 
     void sendReply(const QDBusConnection &connection, const QDBusMessage &message, const QVariant &argument) const;
 
diff --git a/src/gui/accessible/linux/atspiadaptor.cpp b/src/gui/accessible/linux/atspiadaptor.cpp
index dae83437..6c53219c 100644
--- a/src/gui/accessible/linux/atspiadaptor.cpp
+++ b/src/gui/accessible/linux/atspiadaptor.cpp
@@ -1555,6 +1562,25 @@ bool AtSpiAdaptor::handleMessage(const QDBusMessage &message, const QDBusConnect
         return tableInterface(accessible, function, message, connection);
     if (interface == ATSPI_DBUS_INTERFACE_TABLE_CELL ""_L1)
         return tableCellInterface(accessible, function, message, connection);
+    if (interface == "org.a11y.atspi.Hypertext"_L1)
+        return hypertextInterface(accessible, function, message, connection);
+    if (interface == "org.a11y.atspi.Hyperlink"_L1)
+        return hyperlinkInterface(accessible, function, message, connection);
+    if (interface == "org.a11y.atspi.Document"_L1) {
+        if (function == "Locale"_L1 || function == "GetLocale"_L1) {
+            sendReply(connection, message, QString::fromUtf8("en_US"));
+        } else if (function == "GetAttributeValue"_L1) {
+            sendReply(connection, message, QString());
+        } else if (function == "GetAttributes"_L1) {
+            QSpiAttributeSet attrs;
+            attrs.insert(u"DocType"_s, u"text/html"_s);
+            attrs.insert(u"MimeType"_s, u"text/html"_s);
+            sendReply(connection, message, QVariant::fromValue(attrs));
+        } else {
+            sendReply(connection, message, QString());
+        }
+        return true;
+    }
 
     qCDebug(lcAccessibilityAtspi) << "AtSpiAdaptor::handleMessage with unknown interface: " << message.path() << interface << function;
     return false;
@@ -1804,6 +1830,15 @@ QStringList AtSpiAdaptor::accessibleInterfaces(QAccessibleInterface *interface)
     if (interface->tableCellInterface())
         ifaces <<  u"" ATSPI_DBUS_INTERFACE_TABLE_CELL ""_s;
 
+    if (interface->role() == QAccessible::WebDocument)
+        ifaces << u"org.a11y.atspi.Document"_s;
+
+    // NOTE: Hypertext is *not* exposed here because libatspi's client
+    // wraps GetLink results as Hyperlink objects, and GetObject on
+    // those returns Hyperlink instead of Accessible — breaking Orca.
+    // Instead, Orca navigates via tree traversal (get_child_count /
+    // get_child_at_index) which works correctly.
+
     return ifaces;
 }
 
@@ -3066,6 +3130,123 @@ bool AtSpiAdaptor::tableCellInterface(QAccessibleInterface *interface, const QSt
     return true;
 }
 
+// Hypertext interface — allows Orca to navigate through mixed text
+// and embedded objects (links, images, etc.) in web content.
+// Builds the hypertext model from the QAccessibleInterface tree:
+// text-leaf children contribute plain text, others contribute U+FFFC.
+bool AtSpiAdaptor::hypertextInterface(QAccessibleInterface *interface, const QString &function, const QDBusMessage &message, const QDBusConnection &connection)
+{
+    if (!interface->textInterface())
+        return false;
+
+    // Build list of embedded (non-text) children — these are "links"
+    // in AT-SPI2 Hypertext terminology.
+    QList<QAccessibleInterface*> embeddedChildren;
+    for (int i = 0; i < interface->childCount(); ++i) {
+        auto *child = interface->child(i);
+        if (!child)
+            continue;
+        if (child->role() != QAccessible::StaticText)
+            embeddedChildren.append(child);
+    }
+
+    if (function == "Introspect"_L1) {
+        // Minimal introspection
+        return false;
+    } else if (function == "GetNLinks"_L1) {
+        sendReply(connection, message, static_cast<int>(embeddedChildren.size()));
+    } else if (function == "GetLink"_L1) {
+        int index = message.arguments().at(0).toInt();
+        if (index >= 0 && index < embeddedChildren.size()) {
+            QSpiObjectReference ref(connection, QDBusObjectPath(pathForInterface(embeddedChildren.at(index))));
+            sendReply(connection, message, QVariant::fromValue(ref));
+        } else {
+            sendReply(connection, message, QVariant::fromValue(QSpiObjectReference()));
+        }
+    } else if (function == "GetLinkIndex"_L1) {
+        int charOffset = message.arguments().at(0).toInt();
+        // Walk children to find which embedded object is at charOffset.
+        int pos = 0;
+        int linkIndex = 0;
+        int result = -1;
+        for (int i = 0; i < interface->childCount(); ++i) {
+            auto *child = interface->child(i);
+            if (!child)
+                continue;
+            if (child->role() == QAccessible::StaticText) {
+                int len = child->textInterface() ? child->textInterface()->characterCount() : 0;
+                pos += len;
+            } else {
+                if (charOffset == pos) {
+                    result = linkIndex;
+                    break;
+                }
+                pos += 1; // U+FFFC
+                linkIndex++;
+            }
+        }
+        sendReply(connection, message, result);
+    } else {
+        qCWarning(lcAccessibilityAtspi) << "AtSpiAdaptor::hypertextInterface does not implement" << function << message.path();
+        return false;
+    }
+    return true;
+}
+
+// Hyperlink interface — provides link URL, anchor text, and position
+// within the parent's hypertext model.
+bool AtSpiAdaptor::hyperlinkInterface(QAccessibleInterface *interface, const QString &function, const QDBusMessage &message, const QDBusConnection &connection)
+{
+    if (function == "Introspect"_L1) {
+        return false;
+    } else if (function == "NAnchors"_L1 || function == "GetNAnchors"_L1) {
+        sendReply(connection, message, QVariant::fromValue(QDBusVariant(1)));
+    } else if (function == "GetObject"_L1) {
+        // Return the Accessible at this link — same object, accessed via Accessible interface.
+        QDBusMessage reply = message.createReply();
+        QSpiObjectReference ref(connection, QDBusObjectPath(pathForInterface(interface)));
+        reply << QVariant::fromValue(ref);
+        connection.send(reply);
+    } else if (function == "GetURI"_L1) {
+        // The link URL. AccessibilityInterface stores it in the Value text.
+        QString uri = interface->text(QAccessible::Value);
+        sendReply(connection, message, uri);
+    } else if (function == "IsValid"_L1) {
+        sendReply(connection, message, true);
+    } else if (function == "StartIndex"_L1 || function == "GetStartIndex"_L1
+               || function == "EndIndex"_L1 || function == "GetEndIndex"_L1) {
+        // Find this element's position in the parent's hypertext model.
+        auto *parent = interface->parent();
+        int pos = 0;
+        int result = 0;
+        if (parent) {
+            for (int i = 0; i < parent->childCount(); ++i) {
+                auto *sibling = parent->child(i);
+                if (!sibling)
+                    continue;
+                if (sibling == interface || (sibling->object() && sibling->object() == interface->object())) {
+                    result = pos;
+                    break;
+                }
+                if (sibling->role() == QAccessible::StaticText) {
+                    int len = sibling->textInterface() ? sibling->textInterface()->characterCount() : 0;
+                    pos += len;
+                } else {
+                    pos += 1; // U+FFFC
+                }
+            }
+        }
+        if (function == "StartIndex"_L1 || function == "GetStartIndex"_L1)
+            sendReply(connection, message, QVariant::fromValue(QDBusVariant(result)));
+        else
+            sendReply(connection, message, QVariant::fromValue(QDBusVariant(result + 1)));
+    } else {
+        qCWarning(lcAccessibilityAtspi) << "AtSpiAdaptor::hyperlinkInterface does not implement" << function << message.path();
+        return false;
+    }
+    return true;
+}
+
 QT_END_NAMESPACE
 
 #include "moc_atspiadaptor_p.cpp"
```
</details>

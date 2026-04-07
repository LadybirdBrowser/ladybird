# Accessibility support

Ladybird exposes its internal accessibility tree to platform assistive technologies (AT) by serializing tree data from the WebContent process and presenting it through platform-specific wrapper objects in the UI process.

The AppKit port uses `LadybirdAccessibilityElement`, an Objective-C wrapper that implements the `NSAccessibility` protocol.

The Qt port on macOS essentially bypasses Qt’s `QMacAccessibilityElement` Cocoa bridge almost entirely — by using the same `LadybirdAccessibilityElement` wrapper as the AppKit port, but also using an NSView overlay along with three very small, targeted swizzles on `QMacAccessibilityElement` to redirect Qt’s bridge to the NSView overlay.

The Qt port on Linux uses Qt’s built-in AT-SPI2 bridge (`QSpiAccessibleBridge`) — by way of `AccessibilityInterface` (a `QAccessibleInterface` wrapper), along with a custom Orca script.

> [!NOTE]
> Qt’s built-in AT-SPI2 bridge implements direct communication over the Linux AT-SPI2 D-Bus accessibility protocol — rather than, say, making calls using the ATK C API, which is what Firefox and Chromium use for AT-SPI2 D-Bus communication.

The core underlying “infrastructure” code for IPC serialization, tree management, and WebContent-side tree building is shared across all platforms and ports/UIs.

## Architecture

```
        WebContent process                              UI process
-------------------------------------+-------------------------------------------
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
LadybirdAccessibilityElement  LadybirdAccessibilityElement  AccessibilityInterface
 (Obj-C, NSObject subclass)    (via NSView overlay, plus    (QAccessibleInterface)
            \                   three method swizzles on               /
             \                  QMacAccessibilityElement)             /
              \                        /                             /
               +-----------+----------+                     Qt AT-SPI2 bridge,
                           |                             plus custom Orca script
                           |                                        |
                 NSAccessibility protocol                    AT-SPI2 protocol
                           |                                        |
                       VoiceOver                                  Orca
```

That mostly follows the Chromium/Firefox model rather than the WebKit model:

- No dependency on private Apple APIs (WebKit uses the private `NSAccessibilityRemoteUIElement` API)
- The accessibility wrapper objects live in the UI process
- The platform-agnostic parts (`AccessibilityNodeData` IPC data format, IPC endpoints, WebContent-side serialization, `AccessibilityTreeManager` UI-process tree manager) are shared between both UIs

## IPC data format: AccessibilityNodeData

`AccessibilityNodeData` (in `Libraries/LibWebView/`) is the serialization format. Each node carries:

- `id` / `parent_id` / `child_ids` — tree structure via flat ID references
- `role` — ARIA role string (`button`, `heading`, `banner`, etc.)
- `name` / `description` / `value` — accessible name, description, value
- `bounds` — bounding rect in CSS pixels (viewport-relative)
- `is_focused` / `is_disabled` / `heading_level` — essential states
- `live` — `aria-live` value (`assertive`, `polite`, or empty)

The IPC payload is a flat `Vector<AccessibilityNodeData>` with parent/child ID references, rather than a nested tree. That simplifies IPC encoding and lets the `AccessibilityTreeManager` build its own lookup tables.

## IPC endpoints

| Endpoint | Direction | Purpose |
| ---------- | ----------- | --------- |
| `request_accessibility_tree(page_id)` | UI → WebContent | Request full tree snapshot |
| `did_get_accessibility_tree(page_id, nodes)` | WebContent → UI | Full tree response (also used for live updates) |
| `did_accessibility_focus_change(page_id, node_id)` | WebContent → UI | Focus changed |
| `perform_accessibility_action(page_id, node_id, action)` | UI → WebContent | VoiceOver action (press, focus) |

## WebContent-side serialization

`AccessibilityTreeNode::serialize_tree_as_node_data()` walks the existing GC-managed accessibility tree (the same tree used for DevTools JSON inspection) and produces the flat vector. For each node:

- Role comes from `element→role_or_default()` via `ARIA::role_name()`
- Name/description from the existing ACCNAME 1.2 implementation
- Bounds from `element→get_bounding_client_rect()`
- Heading level from `aria_level()` (works for both `h1`-`h6` and `role="heading" aria-level="N"`)
- Live value from explicit `aria-live`, or implicit from role (alert → assertive, status/log/marquee/timer → polite)

`update_layout()` must be called before building the tree — because `exclude_from_accessibility_tree()` checks `layout_node()`, which asserts layout is up-to-date.

Text children of `head` (e.g., `title` text) are filtered out during serialization, since they’re not visible content. Text leaf nodes inherit their parent element’s bounding rect — since text DOM nodes don’t have their own layout box.

## UI-process tree manager: AccessibilityTreeManager

`AccessibilityTreeManager` (in `Libraries/LibWebView/`) is a platform-agnostic C++ class that caches the tree in the UI process. It provides:

- `node(id)` — lookup by node ID
- `hit_test(point)` — recursive hit testing (reverse child order, deepest node wins)
- `set_focused_node(node_id)` — updates focus tracking
- `text_leaves_in_order()` — flat DFS-ordered list of text leaf node IDs for text marker navigation
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

### Live-region announcements

When `AccessibilityTreeManager::update_tree()` detects that a node’s `name` changed, and that node is inside a live region (has a non-empty, non-“off” `live` value in its ancestor chain), it fires the `on_live_region_changed` callback. `LadybirdWebView` posts `NSAccessibilityAnnouncementRequestedNotification` with `NSAccessibilityPriorityHigh` for “assertive” or `NSAccessibilityPriorityMedium` for “polite”.

### Live DOM-mutation updates

After an accessibility action is performed (press, focus), `schedule_accessibility_tree_update()` is called with a 200ms debounce timer to rebuild the tree and push it via `did_get_accessibility_tree`. That handles cases where the action changes the page content.

### LadybirdWebView integration

`LadybirdWebView` (`NSView` subclass) acts as the scroll-area container, with role `NSAccessibilityScrollAreaRole` and a single child: the document root `LadybirdAccessibilityElement`. It owns the `AccessibilityTreeManager` and element cache. Callbacks:

- `on_load_finish` → `request_accessibility_tree()` (direct)
- `on_load_start`, `on_url_change`, `on_title_change` → `scheduleAccessibilityTreeRequest` (debounced, 500ms). Multiple callbacks may fire in quick succession during navigation; `performSelector:afterDelay:` with `cancelPreviousPerformRequests` ensures only one tree request fires after they settle. That covers pages where `on_load_finish` doesn’t fire.
- `on_accessibility_tree_received` → update manager, clear cache, post `NSAccessibilityLayoutChangedNotification` and `@"AXLoadComplete"`
- `on_accessibility_focus_changed` → update manager, post `NSAccessibilityFocusedUIElementChangedNotification`
- `on_live_region_changed` → post `NSAccessibilityAnnouncementRequestedNotification`

`accessibilityFocusedUIElement` returns the first non-ignored element in document order (DFS), ensuring VoiceOver starts reading from the top of the page rather than from wherever JavaScript may have set DOM focus.

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

## Qt on Linux: QAccessibleInterface with custom Orca script

On Linux, the Qt port uses `QAccessibleInterface` (`UI/Qt/AccessibilityInterface.cpp`) to expose the accessibility tree through Qt’s built-in AT-SPI2 bridge (`QSpiAccessibleBridge`). A custom Orca screen-reader script at `~/.local/share/orca/orca-scripts/Ladybird/` extends Orca’s `web.Script` to enable browse-mode navigation for web content.

What works:

- Object navigation through the accessibility tree
- Say All reads through web content
- Structural navigation (H for headings, K for links, I for list items, etc.) — via a Collection fallback patch
- Focus ring shown during screen-reader navigation (CSS `:focus-visible`)
- Text content via `QAccessibleTextInterface` with a hypertext model using U+FFFC for embedded objects
- Object attributes (`tag`, `xml-roles`, `level`) via `QAccessibleAttributesInterface`
- Text run attributes returning `"direction:ltr"` (prevents Orca crash)
- Actions (press, focus) on all elements — enables GrabFocus for focus ring
- Table-cell interface

**The custom Orca scripts** (`~/.local/share/orca/orca-scripts/Ladybird/`) are required; three files:

- `script.py` extends `web.Script` (and Qt’s toolkit script, for issubclass priority). Kept minimal to avoid interfering with the web script’s natural focus flow. Only overrides `activate()` (for cache pre-warming) and `on_focused_changed()` (to handle address bar focus mode). Does _not_ override `on_window_activated` or other event handlers — falling through to `default.Script` for those events causes non-deterministic structural nav suspension on fresh startup.
- `script_utilities.py` extends `web.Utilities`. Overrides `active_document()` (EMBEDS-first with tree-search fallback), `get_caret_context()` (document content fallback), and `get_line_contents_at_offset()` (fast path avoiding layout-mode geometry scanning). Includes a Collection fallback patch wrapping `find_all_with_role` for DFS fallback when Qt’s `GetMatches` returns empty, a SayAll monkey patch of the `SayAllPresenter` handler to redirect to the first document content element when locus is outside the document — fixing Say All on first page load — and background cache pre-warming.
- `__init__.py` contains `from .script import Script`

The scripts are embedded as Qt resources and auto-installed to `~/.local/share/orca/orca-scripts/Ladybird/` on Ladybird startup (only overwritten when content differs).

*Text interface:* Exposed on text leaves and leaf-like roles (links, headings, buttons). Containers build hypertext by walking unignored children: text leaves contribute text, non-text children contribute U+FFFC.

*Text attributes:* `attributes()` returns `"direction:ltr"`. Orca crashes if text attributes are None (`result[0].get("justification")` on None).

*Object attributes:* `tag` (required by Orca’s `is_web_element()`), `xml-roles` (distinguishes landmarks), `level` (heading level).

*Document focus:* After the accessibility tree is received, a 1000ms delayed focus event is posted on the document root — but only if no other widget (e.g. the address bar) currently has focus. If the timer skips the event, `focusInEvent` posts it instead when the user later navigates to the web content area. The Orca script’s `activate()` handles the late-start case by setting caret context (not locus of focus, which would unsuspend structural nav and break address bar typing).

*Focus ring:* During Say All, Orca calls `ScrollSubstringTo` (AT-SPI2 Text interface) on each element, which reaches our `scrollToSubstring` and triggers a “focus” action to WebContent. During structural navigation (H/K/I/L keys), the web script calls `scroll_to_center`, which tries `ScrollSubstringToPoint` — but Qt’s bridge doesn’t handle that method. A monkey patch on `AXEventSynthesizer.scroll_to_center` adds a `ScrollSubstringTo` call that Qt _does_ handle — triggering the same focus action. WebContent sets `tabindex="-1"` on non-focusable elements and calls `run_focusing_steps` with `FocusTrigger::Key` — so CSS `:focus-visible` matches and the native focus ring is drawn.

*Structural navigation:* Orca’s structural navigator uses the AT-SPI2 Collection interface to find elements by role. Qt’s bridge exposes Collection but `GetMatches` returns empty results. The Orca script wraps both `find_all_with_role` (used for headings and other role-only searches) and `find_all_with_role_and_all_states` (used for links, which search for LINK role with FOCUSABLE state) in a fallback patch that falls back to a manual DFS tree search when those calls return empty. `list` and `listitem` roles are added to `QAccessibleTextInterface` so they have the AT-SPI2 Text interface required for scroll-into-view. The fallback patch is forward-compatible: it’ll be bypassed when Qt fixes Collection.

*Address bar typing:* Structural navigation keys (H, K, I, etc.) register keyboard grabs when unsuspended. The `on_focused_changed` handler falls through to `default.Script` only for text entries and editable widgets, explicitly suspending structural nav for those elements. For other chrome events (tabs, frames), the event is dropped to prevent `default.Script` from setting locus to a non-document element which would suspend nav. The `on_window_activated` handler is intentionally _not_ overridden — falling through to `default.Script.on_window_activated` sets locus to the window frame, causing the web script to permanently suspend structural nav on fresh Ladybird start.

### How the script integrates with Orca

Orca has a documented mechanism for loading per-application scripts. At startup, [`orca_bin.py`](https://gitlab.gnome.org/GNOME/orca/-/blob/main/src/orca/orca_bin.py.in) inserts the user’s preferences directory (`~/.local/share/orca/`) into Python’s `sys.path`; for each application Orca encounters, [`script_manager`](https://gitlab.gnome.org/GNOME/orca/-/blob/main/src/orca/script_manager.py) tries `importlib.import_module("orca-scripts.<AppName>")` and, if a module is found, instantiates its `Script` class — which is expected to subclass one of Orca’s built-in script base classes (`default.Script`, `web.Script`, or a toolkit script). The instantiated script then receives the focus, key, and event callbacks for that application. That mechanism is described in the [Orca’s Scripts and Features](https://gitlab.gnome.org/GNOME/orca/-/blob/main/README.md#orcas-scripts-and-features) section of the Orca README.

We use that “sanctioned” mechanism in two ways:

1. **`Script` subclassing.** Our `class Script(web.Script, _QtToolkitScript)` overrides Orca’s standard event handlers (`activate`, `on_focused_changed`, `get_app_key_bindings`) to customize per-application behavior — exactly the use case the Orca docs describe: *“writing a custom script for that application might be the correct solution.”*
2. **`Utilities` subclassing.** Our `class Utilities(web.Utilities)` overrides methods on Orca’s web utilities (`active_document`, `get_caret_context`, `get_line_contents_at_offset`) that Orca calls into during navigation and Say All. Orca looks up the script’s `utilities` attribute and calls the overridden methods at well-defined points.

In addition, our script does something Orca does _not_ explicitly sanction: At script-import time, it monkey-patches three of Orca’s _other_ internal classes — `AXUtilitiesCollection`, `SayAllPresenter`, and `AXEventSynthesizer` — by direct attribute reassignment.

None of those classes exposes a patching API; they’re modified in-place because Python permits any module loaded into a process to mutate any other class in that process. The mechanism that lets us do that monkey patching is _not_ some sanctioned “Orca exposes a sanctioned mechanism for monkey patching” mechanism. Instead, it’s just that because Orca _does_ have the “sanctioned” script-loading mechanism described above, an unavoidable side effect of providing that mechanism is: Any module loaded using that mechanism effectively becomes just like any code originating from Orca itself.

So any module “side loaded” using that sanctioned mechanism also unavoidably becomes free to do anything the Orca code itself can do — anything Python lets any Python code do. And so, we exploit that freedom to do the “unsanctioned” monkey-patching we’re doing.

**Why this matters for maintenance.** The “sanctioned” subclass overriding we’re doing will keep working across Orca releases as long as Orca preserves its documented script API. However, our “unsanctioned” monkey patching will not.

Our monkey patching depends on the names, signatures, and call sites of internal Orca classes that the Orca team makes no stability guarantees for. If Orca renames `AXUtilitiesCollection`, restructures `SayAllPresenter`, or moves `scroll_to_center` out of `AXEventSynthesizer`, our corresponding patch will silently stop applying — and the feature it was working around will silently regress.

So, each of our monkey patches will need re-verification on every new Orca release. The *Orca-side patch lifecycle when Qt 6.11 becomes the system baseline* sub-section below summarizes what to do for each one.

### What was tried and didn’t work: direct ATK

A direct ATK implementation was built, following Firefox’s `MaiAtkObject` pattern: custom `AtkUtil` (toolkit "Gecko"), App/Frame/Document hierarchy with EMBEDS, separate `AtkHyperlink` GObject, `AtkHypertext`, dynamic GType registration, `atk_bridge_adaptor_init()`. We abandoned it because Qt’s `QSpiAccessibleBridge` (compiled into QtGui.so) cannot be disabled at runtime. Two AT-SPI2 providers on the same bus seem to corrupt Orca system-wide. Approaches tried: `QT_ACCESSIBILITY=0` (ineffective), `AT_SPI_BUS_ADDRESS` poisoning (deadlocks), pre-QApplication init (Qt re-registers later), `QDBusConnection::disconnectFromBus` (Qt holds reference). Only viable path: rebuild Qt with `-no-feature-accessibility-atspi-bridge`.

### Observations from Chromium and Firefox ATK implementations

- Both Chromium and Firefox implement ATK directly, creating custom `AtkObject` subclasses. Neither goes through GTK’s accessibility layer.
- `AtkText` is critical — Orca reads text exclusively through this interface. Without it, no text content is announced.
- The `xml-roles` object attribute is essential for distinguishing landmark types, since all landmarks map to the single `ATK_ROLE_LANDMARK` enum value.
- Focus requires dual signals: `focus-event` (for AT-SPI2) and `ATK_STATE_FOCUSED` state change (for direct ATK clients). The old focus must be explicitly cleared.
- Chromium does _not_ implement the AT-SPI2 Collection interface directly — the ATK bridge provides it generically through the GObject introspection mechanism.
- Chromium creates dynamic GTypes per interface combination (e.g., one type for elements with Text+Component, another for Text+Component+Document). Ladybird uses Qt’s AT-SPI2 bridge instead of direct ATK (see _Qt on Linux_ above for why).

So, Firefox and Chromium both use GTK on Linux — but both bypass GTK’s accessibility entirely; instead, they implement ATK directly, with their own custom `AtkObject` subclasses (`MaiAtkObject` and `AXPlatformNodeAuraLinux` respectively) — and call `atk_bridge_adaptor_init()` to register with AT-SPI2.

That works because GTK’s ATK bridge is a separate, modular library (`libatk-bridge-2.0`) that can be prevented from auto-initializing (via `NO_AT_BRIDGE=1`) — letting the application replace it.

Qt’s bridge is the opposite: `QSpiAccessibleBridge` is compiled into `QtGui.so` and activates automatically when it detects a screen reader on D-Bus. There is no environment variable or API to prevent that. And that one key architectural difference is why the direct ATK approach works for GTK-based browsers but not for Qt-based ones.

### What was tried and didn’t work: patching Qt’s bridge

Patching Qt 6.11’s `atspiadaptor.cpp` for Document/Hypertext/Hyperlink partially worked, but hit a libatspi bug: GetLink returns a path wrapped as Hyperlink; GetObject on that path returns Hyperlink, not Accessible. Requires separate GObject (which Qt’s bridge can’t do).

### Alternative to consider: private AT-SPI2 bus with Orca-side redirect

The current architecture uses Qt’s built-in AT-SPI2 bridge with Python-side workarounds in our Orca script. An alternative worth investigating is bypassing Qt’s bridge entirely by running our own direct ATK implementation on a **private AT-SPI2 bus**, with the Orca script connecting to that bus to fetch the accessibility tree.

**Why not just run two bridges on the same bus?** We tried that (see “What was tried and didn’t work: direct ATK” above). The problem was that two AT-SPI2 providers on the same bus — Qt’s `QSpiAccessibleBridge` and our `atk_bridge_adaptor_init()` — corrupted Orca system-wide. The corruption was not limited to Ladybird; it affected all Orca operation. That suggests the problem was at the AT-SPI2 registry or `libatspi` level, not at the Orca level — so having an Orca script ignore one provider’s events would not help if the bus itself is already confused.

**The private-bus variant avoids this entirely.** Instead of two providers on one bus:

1. Qt’s `QSpiAccessibleBridge` registers on the normal AT-SPI2 bus as it always does (we can’t stop it).
2. Ladybird also starts a small private D-Bus instance and runs a direct ATK bridge (`atk_bridge_adaptor_init()` with `AT_SPI_BUS_ADDRESS` pointing at the private bus). This gives us a full in-process ATK bridge with working `GetItems` cache, Hypertext/Hyperlink/Document interfaces, and EMBEDS relation — everything Qt’s bridge lacks.
3. Our Orca script connects to the private bus from Python (via `gi.repository.Atspi` or raw D-Bus calls) and fetches the accessibility tree directly. When Orca’s event handlers fire for the Qt-bridge application, the script redirects navigation to the private-bus tree instead.
4. The normal bus sees only Qt’s bridge (no collision, no corruption). The private bus sees only our ATK bridge.

**Comparison of the three approaches:**

| | Current (patch Qt’s bridge) | Same-bus redirect (Orca ignores Qt) | Private bus |
| --- | --- | --- | --- |
| Qt bridge interference | Eliminated by patch | Still fires events; Orca script must filter | No interference (separate buses) |
| System-wide corruption risk | None | High (the documented failure) | None |
| Orca script complexity | Low (Python workarounds) | Very high (full event redirect) | High (second bus connection) |
| Performance | Good (patched `GetItems` cache) | Depends on redirect overhead | Best (in-process ATK bridge + cache) |
| AT-SPI2 interface coverage | Partial (`GetItems`, EMBEDS; Hyperlink blocked by `libatspi` bug) | Full (direct ATK) | Full (direct ATK) |
| Maintenance burden | Qt patches need rebasing per Qt release | Fragile dependency on Orca internals | Moderate (private bus + ATK bridge) |
| Upstreamable? | Qt patches: yes; Orca workarounds: partially | Hard (very app-specific) | Possible (generic pattern) |
| Other AT clients see our tree? | Yes (via Qt’s bridge, with its limitations) | No (only Orca with our script) | No (only Orca with our script) |


**What this would gain over the current approach:**

- Full `GetItems` cache populated by `libatk-bridge-2.0` (the ATK bridge Chromium and Firefox use) — no need for our `QSpiDBusCache` Qt patch.
- Native Hypertext/Hyperlink/Document interfaces via ATK — no `libatspi` `GetLink`/`GetObject` impedance mismatch.
- EMBEDS relation exposed natively by the ATK bridge.
- Collection interface handled natively by the ATK bridge (via GObject introspection) — no Python fallback patch needed.
- Architecture matches how Chromium and Firefox work, making future maintenance and upstream contributions easier.

**What it would cost:**

- Significant engineering effort: building and maintaining the ATK bridge (dynamic GType registration, `AtkText`, `AtkHypertext`, `AtkHyperlink`, `AtkDocument`, `AtkComponent`), managing the private bus lifecycle, and writing the Orca-side connection logic.
- The Orca script would need to manage a second `Atspi` connection, which `pyatspi` may not support natively (it normally assumes a singleton connection to the standard a11y bus). We might need raw D-Bus calls via `dbus-python` or `gi.repository.Gio` instead.
- Qt’s bridge would still be active on the normal bus, so other AT clients (Accerciser, magnifiers) would see Qt’s tree, not ours. Only Orca (with our script) would see the ATK tree.
- The private-bus approach is novel — no existing screen reader or application uses this pattern, so we would be pioneering the integration and there would be no prior art to reference.

**Prerequisite investigation:** Before committing to this approach, the “system-wide corruption” failure from the two-providers-on-one-bus attempt needs to be precisely characterized. If it turns out the corruption was caused by a specific `atk_bridge_adaptor_init()` call sequence that could be avoided (e.g., by initializing our bridge before Qt’s, or by using a different toolkit name), the simpler same-bus approach might work after all — making the private bus unnecessary. That investigation would need hands-on testing with `dbus-monitor` to trace exactly what goes wrong.


### Qt AT-SPI2 bridge limitations

The accessibility quality we can deliver for Orca users on Linux is bounded by limitations in Qt’s AT-SPI2 bridge (`QSpiAccessibleBridge`). Some of those limitations can be worked around from the Orca side by our custom Orca script (which installs fallback patches and monkey patches inside Orca’s own Python modules at script-load time); some can only be fixed by patching Qt itself; some are addressed in Qt 6.11 upstream compared to Qt 6.10; and some are architectural properties of the Orca-plus-Qt-AT-SPI2 combination that remain even after Qt 6.11 plus patches.

#### Overview by user-facing feature

For each necessary key user-facing accessibility feature we’ve identified as missing in Qt due to limitations in Qt’s AT-SPI2 bridge, the table below shows whether our custom Orca script successfully works around the Qt limitations and enable the “missing” feature to actually work as expected.

✅ – Our Python script enables the “missing” feature to work as expected

❌ - Our Python script _does not_ enable the “missing” feature to work as expected

| Feature |   | Details |
| ------- | - | ------- |
| Structural navigation (`H`/`K`/`I`/`L`/`B`/`F`/`T`/`R`/…) | ✅ | `_install_collection_fallback_patch` wraps both `find_all_with_role` and `find_all_with_role_and_all_states` with a manual DFS tree-search fallback that takes over when Collection’s results are empty (or when the interface is missing entirely) |
| `active_document()` for Say All and structural nav (knowing what document they’re navigating) | ✅ | `Utilities.active_document()` override tries `super()` first and falls back to tree-search with multi-tab `is_showing()` disambiguation |
| Say All starting from document content on first page load | ✅ | `_install_sayall_patch` monkey-patches `SayAllPresenter.say_all` to redirect to the first content child |
| `get_caret_context()` returning document content instead of window title | ✅ | Yes — `Utilities.get_caret_context()` override |
| Focus ring during structural navigation (`H`/`K`/`I`/`L`) | ✅ | `_install_scroll_patch` monkey-patches `AXEventSynthesizer.scroll_to_center` to also call `AXText.scroll_substring_to_location`, which Qt’s Text interface does handle on 6.10 |
| Fast `get_line_contents_at_offset`| ✅ | Override builds line content directly from the accessible tree instead of the web script’s layout-mode geometry scanning |
| Chrome widgets (address bar, tabs) interacting cleanly with structural nav | ✅ | `on_focused_changed` override suspends structural nav on editable widgets and drops other chrome events |
| Landmark type distinction | N/A | No Orca-side patch needed — the `xml-roles` object attribute comes from our `QAccessibleAttributesInterface` in `AccessibilityInterface.cpp`, which is ours on the Ladybird side and works on any Qt 6.8+ |
| Heading-level announcement | N/A | Same as above — `level` object attribute from our Attributes implementation |
| Orca’s “this is web content” detection | N/A | Same as above — `tag` object attribute from our Attributes implementation (Orca’s `is_web_element()` checks for it) |
| AtkHypertext / AtkHyperlink features — link-offset-aware navigation, link enumeration through `getNLinks()` / `getLink()` | ❌ | Cannot be worked around in Python (Orca queries these interfaces directly over D-Bus; no Python hook point) — _but_ recoverable (partially) via the Qt patch in the appendix here) |
| AtkDocument features — document-level locale, URL, MIME metadata | ❌ | Same reason as above — _but_ recoverable (fully) via the Qt patch in the appendix here) |

#### Underlying Qt bridge limitations and where they’re addressed

Below: each Qt-side bug or missing feature, mapped to where which of our Qt (C++) patches address it.

| Qt bridge limitation | Qt 6.10 | Qt 6.11 | Addressed by our Qt patches in this appendix? |
| -------------------- | ------- | ---------------- | --------------------------------------------- |
| `Collection::GetMatches` returns empty | Broken | Fixed | N/A on 6.11; on 6.10, our Orca (Python) patching script covers it |
| `QSpiDBusCache::GetItems()` returns empty array | Broken | Broken | Yes — Qt patch 1 |
| `RELATION_EMBEDS` not exposed from window to `WebDocument` child | Missing | Missing | Yes — Qt patch 2 |
| `AtkDocument` interface not exposed on document root | Missing | Missing | Yes — Qt patch 3 (Document portion) |
| `AtkHypertext` / `AtkHyperlink` interfaces not exposed on text containers | Missing | Missing | Partial — Qt patch 3 (blocked by a `libatspi` bug for full Orca utility; see Qt patch 3 for the details) |

The only upstream accessibility improvement in the Qt 6.11 cycle that materially affects Orca is the `Collection::GetMatches` row above: 6.11 added a new `QSpiMatchRuleMatcher` class (`src/gui/accessible/linux/qspimatchrulematcher.cpp` and `.h`) that the 6.11 `AtSpiAdaptor::collectionInterface()` uses — so in 6.11, `Collection::GetMatches` returns real results. On 6.10 and earlier, the Collection _interface_ was registered, but the `GetMatches` _method_ returned an empty array — which is why `script_utilities.py` Orca carries the Collection fallback patch: because without it, structural navigation would otherwise be _entirely_ dead on 6.10.

#### The AT-SPI2 client-side cache and why GetItems() matters

AT-SPI2 defines a `Cache` D-Bus interface with a `GetItems()` method. The mechanism has two sides:

- **Server side** (in the application’s process): the AT-SPI2 bridge implements `GetItems()` by walking the application’s accessibility tree and returning a bulk snapshot: every object’s D-Bus path, parent, children, supported interfaces, name, role, description, and state.
- **Client side** (in Orca’s process): `libatspi` (the C library that Orca’s `pyatspi` Python bindings wrap) calls `GetItems()` when it first encounters an application, stores the response in a local hash table, and then serves subsequent queries for role, name, state, and children from that local table — _without_ making D-Bus round-trips. `AddAccessible` and `RemoveAccessible` D-Bus signals keep the hash table in sync as the tree changes.

When the cache is populated, Orca’s queries for tree metadata are served from `libatspi`’s local hash table in Orca’s own process — no D-Bus message leaves the process. Only queries for dynamic data that isn’t part of the cache snapshot (text content, character geometry) still require per-element D-Bus calls to the application.

Firefox and Chromium both use `libatk-bridge-2.0` as their AT-SPI2 bridge. Its `GetItems()` implementation returns a fully-populated array — so `libatspi`’s client-side cache works as intended. Qt’s `QSpiDBusCache::GetItems()` instead _returns an empty array_ — so `libatspi` has _no_ cached data, and _every_ query, even for static metadata like role and name, requires a D-Bus round-trip from Orca’s process to the application. That is the root cause of the multi-second Say All delay we’ve encountered: Orca’s web script builds each line of content by querying role, name, text, states, and attributes on each element, and on Qt _every_ one of those queries is a _cross-process_ D-Bus call. Qt patch 1 in the appendix below addresses that problem by implementing a real `GetItems()` response.

> [!NOTE]
> Both `libatk-bridge-2.0` and Qt’s `QSpiAccessibleBridge` run inside the application’s process — they are both “in-process” bridges relative to the application. The performance difference is not about where the bridge runs; it’s instead is about whether the bridge properly implements `GetItems()` and thereby populates `libatspi`’s client-side cache in Orca’s process.

#### Architectural limitations (not addressable in either Qt or Orca code)

The following remaining limitations are not fixable with either a Qt (C++) patch or a with any patch our custom Orca (Python) script could install.

- **Residual Say All startup delay.** Even with `GetItems` populated (Qt patch 1), text and geometry queries (`GetText`, `GetCharacterExtents`, `GetRangeExtents`) are still per-element D-Bus calls by design; the data would be expensive to pre-serialize into a cache reply. Combined with Orca-internal Python and GLib event-loop overhead and `speech-dispatcher` startup, some significant user-noticeable delay on the very first Say All will remain. Firefox and Chromium avoid that because `libatk-bridge-2.0` properly implements `Cache::GetItems()` — so `libatspi` in Orca’s process has a local copy of the tree metadata, and most queries never need a D-Bus round-trip (see the *AT-SPI2 client-side cache* sub-section above).
- **Stale AT-SPI2 proxy references after a tab switch.** Orca’s Python-side caches hold `Atspi.Accessible` proxies keyed by the D-Bus object path they came from. When Ladybird switches tabs, `hideEvent` de-registers the inactive tab’s `QAccessibleInterface` wrappers (so the paths become invalid) and `scrollToSubstring` refuses to operate on non-visible views — but Orca’s Python proxies still point at the old paths. Say All on the second tab reads the correct content (because `_patched_say_all` looks up the current tab’s document via `active_document()`) — but the `scroll_into_view` calls go to stale proxies from the first tab, and the focus ring may fail to appear. Fixing that would require changes to how Orca manages its proxy cache across accessibility-tree transitions — not reachable from a Qt-bridge or application-side fix.

#### Orca-side patch lifecycle when Qt 6.11 becomes the system baseline

What should happen to the Orca-side patches once Ladybird’s minimum-required Qt is 6.11 or later? The short answer is: **we keep all of them**. The longer answer is:

- **`_install_collection_fallback_patch` (Collection fallback patch)** — *Keep until Ladybird drops support for Qt < 6.11.* On Qt 6.11+, `Collection::GetMatches` returns real results, so the fallback branch inside the wrapper is never taken; the wrapper itself stays in the call chain but is otherwise dormant. Removing it earlier would silently break structural navigation for users still running an older system Qt.
- **`_install_sayall_patch` (SayAll monkey patch)** — *Keep indefinitely.* This patches Orca’s own `SayAllPresenter` to redirect Say All into document content when the locus of focus is elsewhere. It’s a workaround for Orca-side behavior, not a Qt bridge bug, and Qt versioning is irrelevant. It only stops being needed if Orca itself changes how it picks the Say All start element.
- **`_install_scroll_patch` (scroll_to_center monkey patch)** — *Keep until Qt adds `ScrollSubstringToPoint` handling.* Qt 6.11’s `atspiadaptor.cpp` still only handles `ScrollSubstringTo`, not `ScrollSubstringToPoint` (verified directly in the 6.11.0 source). The patch’s extra `scroll_substring_to_location` call is what reaches our `scrollToSubstring` and triggers the focus ring during structural navigation; without it, structural-nav focus rings would stop working on Qt 6.11 too.

The subclass-overriding we do on `Utilities` and `Script` (`active_document`, `get_caret_context`, `get_line_contents_at_offset`, `on_focused_changed`) are normal Python OO method overrides on our extension classes, not runtime patches — they stay as long as the extension does.

#### Candidates for upstreaming into Orca itself

The Orca-side patch lifecycle sub-section above covers what to do with each patch when Qt itself improves. This sub-section is the complement: It shows (sorted by upstreaming priority) which of the changes our script makes might be worth proposing as patches to upstream Orca — so that no Ladybird-side workaround is needed at all. .

##### Strong candidates

1. **`get_line_contents_at_offset()` performance optimization.** Our `Utilities.get_line_contents_at_offset()` override builds line content directly from the accessible tree, rather than via Orca’s stock layout-mode geometry scanning, which costs roughly 75 –115 D-Bus calls per line. Our path costs a small constant number of calls per line. The fast path is purely a different algorithm to compute the same answer — no new dependencies, no behavioral change visible to the user beyond “Say All starts faster.” It would benefit every Orca-on-AT-SPI2 setup where `GetItems()` is unimplemented (i.e., any application on a toolkit whose AT-SPI2 bridge doesn’t populate `libatspi`’s client-side cache — see the *AT-SPI2 client-side cache* sub-section above). Even with a populated cache, it would see some speedup. Upstream source: `orca/scripts/web/utilities.py::Utilities.get_line_contents_at_offset`.

2. **SayAll: redirect to first content child when `obj` is None.** Our `_install_sayall_patch` makes `SayAllPresenter.say_all` find the active document’s first content child and start from there when called with `obj=None`, rather than starting from wherever the locus of focus happens to be. The current upstream behavior is a usability bug for any web context where the user invokes Say All without first having clicked into the page; it happens with Firefox and Chromium too — just less visibly, because their AT-SPI2 trees come up faster. The fix is defensive: in a web-script context, if `obj` is None and locus is outside document content, fall back to first content child of the active document. Upstream source: `orca/say_all_presenter.py::SayAllPresenter.say_all` (or the web script’s wrapper, if any).

3. **`scroll_to_center`: also call `ScrollSubstringTo` as a fallback.** Our `_install_scroll_patch` augments `AXEventSynthesizer.scroll_to_center` to call `AXText.scroll_substring_to_location` after the existing `ScrollSubstringToPoint` attempt. The added call is harmless on toolkits that already handle `ScrollSubstringToPoint`, and recovers focus-ring behavior on toolkits (Qt) that don’t. There’s no plausible downside to upstreaming it. Upstream source: `orca/ax_event_synthesizer.py::AXEventSynthesizer.scroll_to_center`.

##### Medium candidates

4. **Collection fallback to manual tree search.** `_install_collection_fallback_patch` wraps `find_all_with_role` and `find_all_with_role_and_all_states` with a manual DFS fallback when the AT-SPI2 Collection interface returns empty results. As of Qt 6.11, the affected case is shrinking — but other AT-SPI2 implementations (custom toolkits, embedded environments, older Qt that distros may carry for years) would still benefit. Possible pushback: Orca maintainers may take the position that broken-Collection is a toolkit bug Orca shouldn’t paper over; there’s also a small per-call overhead even on the “happy” path. Upstream source: `orca/ax_utilities_collection.py::AXUtilitiesCollection.find_all_with_role` (and `find_all_with_role_and_all_states`).

5. **`active_document()` fallback to tree search.** Our `Utilities.active_document()` override tries `super()` first, then falls back to a tree search for the first `WebDocument`-role descendant of the active window, with multi-tab `is_showing` disambiguation. Same defensive-programming argument and same possible pushback as #4 — Orca maintainers may consider this a toolkit bug to fix at the source. Tree search is also slower than EMBEDS, so the override would need to be conditional (“only fall back if EMBEDS returned nothing”). Upstream source: `orca/scripts/web/utilities.py::Utilities.active_document`.

##### Probably not upstreamable

6. **`Script.activate()` cache pre-warm.** This kicks off a background thread that pre-fetches role/name/state/attribute caches for the focused application’s accessibility tree. It’s a workaround for `libatspi`’s client-side cache being empty (because Qt’s `GetItems()` returns nothing — see the *AT-SPI2 client-side cache* sub-section above). The “right” upstream fix is the Qt-side `QSpiDBusCache::GetItems()` patch in this appendix (or the equivalent for whichever toolkit). Adding a pre-warm hack to Orca would be papering over the underlying architecture problem rather than fixing it; it would also risk pathological cases on huge documents.

7. **`Script.on_focused_changed()` chrome-widget handling.** Our override drops events for non-editable chrome widgets and explicitly suspends structural nav when focus moves into editable chrome. This depends on the application having a clear chrome-vs.-document-content distinction, which is browser-specific. Orca’s `web.Script` already has logic in this area; our override is fixing a _gap_ in that logic for the Ladybird/Qt case. There may be a smaller, more targeted upstream fix possible, but it would need to be designed against `web.Script`’s existing focus-handling state machine — not just dropped in as-is.

8. **`Utilities.get_caret_context()` fallback.** Returns document content as the caret-context fallback when the original returns something that isn’t in document content. Possibly upstreamable, but the rationale and edge cases are entangled with `active_document` and `_find_first_content_child` — and untangling for upstream submission would be non-trivial.

##### Definitely not upstreamable

The `_install_*_patch` _functions themselves_ (the runtime mechanism that mutates Orca’s classes via `setattr`) are scaffolding that exists only because the underlying behavior fix isn’t upstream yet. Once any of items 1 –5 lands in Orca itself, the corresponding script-side wrapper becomes a no-op and can be removed in a future Ladybird release.

##### Summary

| Priority | Change | Effort to upstream | Acceptance odds |
| -------- | ------ | ------------------ | --------------- |
| 1 | `get_line_contents_at_offset` fast path | Low (algorithmic patch, no new deps) | High |
| 2 | SayAll redirect when `obj` is None | Low (single-method change) | Medium-high |
| 3 | `scroll_to_center` adds `ScrollSubstringTo` call | Trivial (one-line addition) | High |
| 4 | Collection fallback to tree search | Low (wrapper logic) | Medium |
| 5 | `active_document` fallback to tree search | Low (wrapper logic) | Medium |
| 6 | `activate` cache pre-warm | N/A — wrong fix | N/A |
| 7 | `on_focused_changed` chrome-widget handling | High — needs redesign | Low |
| 8 | `get_caret_context` fallback | Medium — entangled with #5 | Low-medium |

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

**Assessment:** AccessKit would eliminate some of the relatively low-implementation-effort platform plumbing we’re doing — but not the hard parts. The web-specific code — search predicates, text markers, container-descendant skipping, ignored-element semantics for web content — would still need to be implemented on top. The biggest win would have been for the Qt port, where AccessKit could have replaced the swizzles on `QMacAccessibilityElement` with a proper native accessibility layer — but we already reduced the swizzles from seven to three by sharing `LadybirdAccessibilityElement` between AppKit and Qt. For the AppKit port, the gain is more modest — since `LadybirdAccessibilityElement` already implements NSAccessibility directly. So the trade-off would adding a Rust build dependency for what amounts to a cleaner version of the platform-plumbing layer. But even then, with AccessKit, it seems we’d still be lacking the DFS traversal logic (leaf-role skipping, container-descendant skipping, search-key filtering) that makes VoiceOver navigation work smoothly in web content. And that on its own is a critical deficiency that would make AccessKit unsuitable for our needs.

### Future work: search-key filtering on macOS

Our macOS search-predicate implementation returns all non-ignored elements in DFS order without filtering by the `AXSearchKey` parameter. VoiceOver sends search keys like `AXHeadingSearchKey`, `AXLinkSearchKey`, `AXLandmarkSearchKey`, etc., to request elements of a specific type. Currently VoiceOver handles the filtering itself, but implementing search-key filtering on our side would improve performance and correctness.

Chromium’s `OneShotAccessibilityTreeSearch` (`ui/accessibility/platform/one_shot_accessibility_tree_search.h` / `.cc`) uses `PredicateForSearchKey()` to map each VoiceOver search key to a C++ predicate function, then walks the tree returning only matching elements. Firefox’s `MOXSearchInfo` (`accessible/mac/MOXSearchInfo.mm`) does the same — via `PivotRule` subclasses. Both approaches avoid returning the entire flattened tree when VoiceOver only wants headings or links.

## Tests

An added Accessibility test mode in `test-web` follows the same basic pattern as Layout and Text tests:

- Tests are HTML files in `Tests/LibWeb/Accessibility/input/`
- Expectations files are in `Tests/LibWeb/Accessibility/expected/`
- The expectations tree-dump format shows the raw tree structure, including ignored elements and text leaves.
- You can use `test-web --rebaseline` to generate/update the expectations files.

## File guide

### `Libraries/LibWeb/DOM/`

`AccessibilityTreeNode.cpp` / `AccessibilityTreeNode.h`
- Added `serialize_tree_as_node_data()`, which walks the existing GC-managed accessibility tree and produces a flat `Vector<AccessibilityNodeData>`. Extracts role, name, description, bounds, heading level, `aria-live` value, and focus state for each node. Text leaf nodes inherit their parent element’s bounding rect.

`Document.cpp` / `Document.h`
- Added `build_accessibility_node_data()`, which builds the accessibility tree and then calls `serialize_tree_as_node_data()` on it. Also hooks into `set_active_element()` to notify the page client when the focused element changes.

### `Libraries/LibWeb/Page/Page.h`
- Added `page_did_change_active_element(UniqueNodeID)` virtual method to the `PageClient` interface, so `Document` can notify the UI process when focus changes.

### `Libraries/LibWebView/`

`AccessibilityNodeData.cpp` / `AccessibilityNodeData.h`
- The IPC-serializable struct that represents one node in the accessibility tree. Contains id, parent/child IDs, role, name, description, value, bounds, focus/disabled state, heading level, and `aria-live` value. The `.cpp` file has the IPC `encode`/`decode` specializations.

`AccessibilityTreeManager.cpp` / `AccessibilityTreeManager.h`
- Platform-agnostic C++ class that caches the accessibility tree in the UI process. Provides node lookup, hit-testing, focused-node tracking, and DFS-ordered text leaf enumeration. On `update_tree()`, compares old and new trees to detect aria-live-region content changes.

`ViewImplementation.cpp` / `ViewImplementation.h`
- Added `request_accessibility_tree()` and `perform_accessibility_action()` methods, plus `on_accessibility_tree_received` and `on_accessibility_focus_changed` callbacks.

`WebContentClient.cpp` / `WebContentClient.h`
- Client-side IPC handlers for `did_get_accessibility_tree` and `did_accessibility_focus_change` messages from WebContent. Dispatches to the `ViewImplementation` callbacks.

`Forward.h`
- Forward declaration for `AccessibilityNodeData`.

`PageInfo.h`
- Added `AccessibilityTree` to the `PageInfoType` enum, used by `test-web` for accessibility tree dumps.

### `Services/WebContent/`

`ConnectionFromClient.cpp` / `ConnectionFromClient.h`
- Implements `request_accessibility_tree` (builds and sends the full tree) and `perform_accessibility_action` (looks up a DOM node by `UniqueNodeID` — walks from text nodes to parent elements, sets `tabindex="-1"` on non-focusable elements, calls `run_focusing_steps` with `FocusTrigger::Key` for focus actions so `:focus-visible` matches). Also implements the `AccessibilityTree` page-info dump for `test-web`.

`PageClient.cpp` / `PageClient.h`
- Implements `page_did_change_active_element` (sends focus-change IPC). Adds `schedule_accessibility_tree_update()` with a 200ms debounce timer, called after accessibility actions to push tree updates when the page changes.

`WebContentServer.ipc`
- Added `request_accessibility_tree(page_id)` and `perform_accessibility_action(page_id, node_id, action)` endpoints.

`WebContentClient.ipc`
- Added `did_get_accessibility_tree(page_id, nodes)` and `did_accessibility_focus_change(page_id, node_id)` endpoints.

### `UI/AppKit/Interface/`

`LadybirdAccessibilityElement.h` / `LadybirdAccessibilityElement.mm`
- The main NSAccessibility wrapper. An `NSObject` subclass implementing the older “informal” protocol plus “modern” property overrides. Handles role mapping, ignored-element transparency, search-predicate navigation (DFS with leaf-role and container-descendant skipping), hit testing, actions, table navigation attributes, text-marker navigation, and coordinate conversion.

`LadybirdWebView.h` / `LadybirdWebView.mm`
- Integration point. Owns the `AccessibilityTreeManager` and element cache. Wires up IPC callbacks (tree received, focus changed, live-region announcements). Acts as `NSAccessibilityScrollAreaRole` containing the document root element. Posts `AXLoadComplete`, layout-changed, focus-changed, and announcement notifications. Delegates search predicates and text-marker queries to the document root.

### `UI/Qt/`

`WebContentViewAccessibility.h` / `WebContentViewAccessibility.mm` (macOS only)
- NSView overlay conforming to `LadybirdAccessibilityViewProtocol`. Contains the `WebContentAccessibilityView` class, a minimal `WebContentViewAccessible` factory, and three swizzles on `QMacAccessibilityElement` (role, children, focused). Handles tree-update notifications, focus changes, and live-region announcements.

`AccessibilityInterface.h` / `AccessibilityInterface.cpp` (Linux)
- `QAccessibleInterface` implementation. Maps `AccessibilityNodeData` nodes to Qt’s accessibility API. Implements `QAccessibleTextInterface` (hypertext model with U+FFFC), `QAccessibleActionInterface`, `QAccessibleTableCellInterface`, and `QAccessibleAttributesInterface` (tag, xml-roles, level). Also contains `WebContentViewAccessible` (a `QAccessibleWidget` that bridges the `WebContentView` widget into Qt’s accessibility tree).

`WebContentView.h` / `WebContentView.cpp`
- Owns the `AccessibilityTreeManager` and element cache. Sets up IPC callbacks, debounced tree requests. Posts document root focus events via `notify_accessibility_focus_on_document_root()` (from `focusInEvent` and a 1000ms delayed timer that skips when the address bar has focus). Deregisters inactive tab accessibility interfaces in `hideEvent`.

`Tab.cpp`
- Calls `schedule_accessibility_tree_request()` in `on_load_start`, `on_url_change`, and `on_title_change` callbacks (which overwrite `WebContentView`’s callbacks).

### `UI/Qt/OrcaScripts/Ladybird/`

Custom Orca screen-reader scripts, embedded as Qt resources and auto-installed to `~/.local/share/orca/orca-scripts/Ladybird/` on startup.

`script.py`
- Extends `web.Script` (and Qt’s toolkit script for `issubclass` priority). Minimal: only overrides `activate()` (cache pre-warming) and `on_focused_changed()` (address bar focus mode with explicit structural nav suspension for editable widgets). Does _not_ override `on_window_activated` or other event handlers — `default.Script` fallthroughs for those cause non-deterministic structural nav suspension.

`script_utilities.py`
- Extends `web.Utilities`. Overrides `active_document()` (EMBEDS-first with tree-search fallback), `get_caret_context()` (document content fallback for Say All), and `get_line_contents_at_offset()` (fast path avoiding layout-mode geometry scanning). Includes a Collection fallback patch (wraps `find_all_with_role` so it falls back to a manual DFS tree search when Qt’s Collection returns empty), a SayAll monkey patch (monkey-patches `SayAllPresenter` to redirect to document content), and background cache pre-warming.

`__init__.py`
- `from .script import Script`

## Appendix: Qt AT-SPI2 bridge patches

The patches shown in this section are against Qt 6.11.0 (`src/gui/accessible/linux/`) and complement the _“Limitations that require patching Qt itself”_ section above. They’ve not been upstreamed to the Qt sources; instead they’re included here as a way of having a record of them until they can do get upstreamed (at which point this appendix should be removed).

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

   The output should reference `libQt6Gui.so.6` under your install prefix (`/opt/qt-6.11.0-patched/lib/...` in the example above), not a system path such as `/usr/lib/...`.
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

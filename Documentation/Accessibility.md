# macOS accessibility (VoiceOver) support

Ladybird exposes its internal accessibility tree to macOS assistive technologies (VoiceOver, Accessibility Inspector) by serializing tree data from the WebContent process and presenting it through Objective-C wrapper objects in the UI process. This document describes the architecture, design decisions, and implementation details.

## Architecture

```
WebContent process                    UI process (AppKit)
-------------------                   --------------------
DOM + ARIA tree
    |
build_accessibility_tree()
    |
serialize_tree_as_node_data()
    |
    +--- IPC: did_get_accessibility_tree(page_id, Vector<AccessibilityNodeData>) --->
                                      |
                                      AccessibilityTreeManager
                                      (C++, in LibWebView)
                                      HashMap<i64, AccessibilityNodeData>
                                      |
                                      LadybirdAccessibilityElement(s)
                                      (Obj-C, wraps AccessibilityNodeData)
                                      |
                                      NSAccessibility informal protocol
                                      + modern property API overrides
                                      |
                                      VoiceOver / Accessibility Inspector
```

That follows the Chromium/Firefox model rather than the WebKit model. We chose that because:

- No dependency on private Apple APIs (WebKit uses the private `NSAccessibilityRemoteUIElement` API)
- The `NSAccessibility` objects live in the UI process — which is natural for AppKit
- Extensible to other platforms (AT-SPI on Linux, IA2 on Windows)
- Ladybird’s IPC already has patterns for push notifications

## IPC data type

`AccessibilityNodeData` (in `Libraries/LibWebView/AccessibilityNodeData.h`) is the serialization format. Each node carries:

- `id` / `parent_id` / `child_ids` — tree structure via flat ID references
- `role` — ARIA role string (`button`, `heading`, `banner`, etc.)
- `name` / `description` / `value` — accessible name, description, value
- `bounds` — bounding rect in CSS pixels (viewport-relative)
- `is_focused` / `is_disabled` / `heading_level` — essential states
- `live` — `aria-live` value (`assertive`, `polite`, or empty)

The IPC payload is a flat `Vector<AccessibilityNodeData>` with parent/child ID references, rather than a nested tree. That simplifies IPC encoding and lets the `AccessibilityTreeManager` build its own lookup tables.

### IPC endpoints

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

## UI-process tree manager

`AccessibilityTreeManager` (in `Libraries/LibWebView/`) is a platform-agnostic C++ class that caches the tree in the UI process. It provides:

- `node(id)` — lookup by node ID
- `hit_test(point)` — recursive hit testing (reverse child order, deepest node wins)
- `set_focused_node(node_id)` — updates focus tracking
- `text_leaves_in_order()` — flat DFS-ordered list of text leaf node IDs for text marker navigation
- `update_tree(nodes)` — replaces the tree, with `aria-live` change detection: compares old and new node names, fires `on_live_region_changed` callback when content changes inside a live region

The manager lives in LibWebView (not in the AppKit layer) — so it can be reused for AT-SPI on Linux.

## NSAccessibility wrapper

`LadybirdAccessibilityElement` (in `UI/AppKit/Interface/`) is an `NSObject` subclass implementing the `NSAccessibility` “informal” protocol. Each instance wraps one node by holding a node ID and a pointer to the `AccessibilityTreeManager`. Elements are cached in a dictionary keyed by node ID on the `LadybirdWebView`.

### Why NSObject, not NSAccessibilityElement

Chromium uses `NSAccessibilityElement` as its base class. We tried that but encountered two problems:

1. `NSAccessibilityElement`’s synthesized read-write properties (internal ivar storage) competed with our getter overrides — the accessibility system read the stored nil values rather than calling our methods.
2. `accessibilityParameterizedAttributeNames` overrides were never called — `NSAccessibilityElement` swallowed them.

`NSObject` with the “informal” protocol gives us full control. Tradeoff: we have to override certain “modern” property API methods that `NSAccessibilityElement` would provide automatically. In particular, we found that missing overrides for `accessibilityWindow` and `accessibilityTopLevelUIElement` caused VoiceOver to treat nested elements as orphaned (not part of any window) — breaking VoiceOver forward navigation into nested groups.

### Dual API support

The macOS accessibility system queries objects via two code paths:

1. **Older “informal” protocol** — `accessibilityAttributeValue:`, `accessibilityAttributeNames`, `accessibilityIsIgnored`
2. **“Modern” property API** — `accessibilityRole`, `accessibilityTitle`, `accessibilityChildren`, etc.

Both paths must return consistent data. The “informal” protocol is the source of truth, with “modern” property overrides that delegate to `accessibilityAttributeValue:`. All relevant modern overrides must be present — on `NSObject`, the defaults return nil, and VoiceOver may treat nil-returning elements as invalid.

### Role mapping

ARIA roles map to `NSAccessibility` roles via `aria_role_to_ns_role()`. Landmark roles (banner, navigation, main, etc.) map to `NSAccessibilityGroupRole`, with `AXLandmark*` subrole strings. `NSAccessibilityRoleDescription()` doesn’t recognize the `AXLandmark*` subrole strings (it returns `group` for all of them) — so custom role description strings are provided, matching what Safari, Chrome, and Firefox all do.

The `document` role maps to `@"AXWebArea"` (not a public `NSAccessibilityRole` constant, but used by all other engines) with role description `@“HTML content”`. Without `AXWebArea`, VoiceOver treats the entire page as a generic group and summarizes it — rather than navigating through web content.

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

1. **Leaf-like roles are navigation terminals.** Links, buttons, headings, menu items, tabs, radio buttons, checkboxes, and images do not have their children included in the navigation order. Their accessible name already carries the text content. Without that, for e.g., a link with the text _“Planning”_, VoiceOver would visit both, _“link Planning”_ and _“text leaf Planning”_ — as separate stops.

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

After an accessibility action is performed (press, focus), `schedule_accessibility_tree_update()` is called with a 200ms debounce timer to rebuild the tree and push it via `did_get_accessibility_tree`. This handles cases where the action changes the page content.

### LadybirdWebView integration

`LadybirdWebView` (`NSView` subclass) acts as the scroll-area container, with role `NSAccessibilityScrollAreaRole` and a single child: the document root `LadybirdAccessibilityElement`. It owns the `AccessibilityTreeManager` and element cache. Callbacks:

- `on_load_finish` → `request_accessibility_tree()` (direct)
- `on_load_start`, `on_url_change`, `on_title_change` → `scheduleAccessibilityTreeRequest` (debounced, 500ms). Multiple callbacks may fire in quick succession during navigation; `performSelector:afterDelay:` with `cancelPreviousPerformRequests` ensures only one tree request fires after they settle. This covers pages where `on_load_finish` does not fire.
- `on_accessibility_tree_received` → update manager, clear cache, post `NSAccessibilityLayoutChangedNotification` and `@"AXLoadComplete"`
- `on_accessibility_focus_changed` → update manager, post `NSAccessibilityFocusedUIElementChangedNotification`
- `on_live_region_changed` → post `NSAccessibilityAnnouncementRequestedNotification`

`accessibilityFocusedUIElement` returns the first non-ignored element in document order (DFS), ensuring VoiceOver starts reading from the top of the page rather than from wherever JavaScript may have set DOM focus.

## Browser-engine source code research

The Ladybird accessibility implementation was informed by studying the Blink/Chromium, WebKit/Safari, and Gecko/Firefox accessibility code.

### Chromium

- `content/browser/accessibility/browser_accessibility_cocoa.h`
- `content/browser/accessibility/browser_accessibility_cocoa.mm`
- Chromium “Accessibility Overview” doc

**Lessons learned:**

1. **Base class: `NSAccessibilityElement`.** `BrowserAccessibilityCocoa` inherits from `NSAccessibilityElement`, not `NSObject`. We tried doing it that way, but hit property-competition issues (see _“Why NSObject, not NSAccessibilityElement”_ above).

2. **IPC serialization model.** Chromium serializes the accessibility tree via Mojo IPC into the browser process. That is the fundamental architecture we adopted.

3. **`OneShotAccessibilityTreeSearch`.** Chromium implements `AXUIElementsForSearchPredicate` via a depth-first tree-search class that skips subtrees based on search criteria. That informed our search-predicate implementation — particularly the concept of “leaf-like” roles whose children are not individually navigable.

4. **`AXWebArea` role.** Chromium maps the document role to `@"AXWebArea"` — the critical signal that tells VoiceOver to use web-content navigation mode.

5. **Custom landmark role descriptions.** Chromium has custom role-description strings for landmarks — rather than relying on `NSAccessibilityRoleDescription`, which doesn’t recognize `AXLandmark*` subrole strings.

### WebKit

- Apple developer documentation on `NSAccessibilityRemoteUIElement`
- WebKit accessibility architecture descriptions
- Safari behavior in Accessibility Inspector (for comparison)

**Lessons learned:**

1. **Private API: `NSAccessibilityRemoteUIElement`.** WebKit uses Apple’s private Mach-port token-exchange API for cross-process accessibility. We chose not to use this approach.

2. **Coordinate system.** By comparing against Safari's element bounds in Accessibility Inspector, we confirmed that CSS pixels equal points on macOS — no device-pixel-ratio scaling is needed.

3. **Landmark role descriptions.** Just as Chrome does, Safari also provides custom role-description strings for landmarks (`banner`, `navigation`, `main`, etc.) — rather than relying on `NSAccessibilityRoleDescription()` — confirming that it must be necessary to implement those custom role-description strings.

### Firefox

- Firefox `MOXAccessibleBase` and `MOXSearchInfo` class references
- Firefox `Pivot` class for accessibility tree traversal

**Lessons learned:**

1. **Same IPC model as Chromium.** Firefox serializes the accessibility tree from the content process to the parent process via IPDL IPC. So that seems like the de facto standard way of doing it in multi-process browsers.

2. **Pre-order depth-first traversal.** Firefox’s `Pivot` class implements pre-order DFS for search predicates, confirming our DFS approach.

3. **Search predicate parameters.** Firefox’s `MOXSearchInfo` processes `AXDirection`, `AXStartElement`, and `AXResultsLimit` from the search-predicate dictionary — the same fields we handle.

### Key cross-engine observations

- All three other engines map the document root to `AXWebArea`.
- All three other engine provide custom role-description strings for landmarks — rather than relying on `NSAccessibilityRoleDescription()`.
- Chromium and Firefox both serialize the tree to the UI process; only WebKit does that differently.

## Tests

An added Accessibility test mode in `test-web` follows the same basic pattern as Layout and Text tests:

- Tests are HTML files in `Tests/LibWeb/Accessibility/input/`
- Expectations files are in `Tests/LibWeb/Accessibility/expected/`
- The expectations tree-dump format show the raw tree structure, including ignored elements and text leaves.
- You can use `test-web --rebaseline` to generate/update the expectations files.

## File guide

### `Libraries/LibWeb/DOM/`

`AccessibilityTreeNode.cpp` / `AccessibilityTreeNode.h`
↳ Added `serialize_tree_as_node_data()`, which walks the existing GC-managed accessibility tree and produces a flat `Vector<AccessibilityNodeData>`. Extracts role, name, description, bounds, heading level, `aria-live` value, and focus state for each node. Text leaf nodes inherit their parent element’s bounding rect.

`Document.cpp` / `Document.h`
↳ Added `build_accessibility_node_data()`, which builds the accessibility tree and then calls `serialize_tree_as_node_data()` on it. Also hooks into `set_active_element()` to notify the page client when the focused element changes.

### `Libraries/LibWeb/Page/`

`Page.h`
↳ Added `page_did_change_active_element(UniqueNodeID)` virtual method to the `PageClient` interface, so `Document` can notify the UI process when focus changes.

### `Libraries/LibWebView/`

`AccessibilityNodeData.cpp` / `AccessibilityNodeData.h`
↳ The IPC-serializable struct that represents one node in the accessibility tree. Contains id, parent/child IDs, role, name, description, value, bounds, focus/disabled state, heading level, and `aria-live` value. The `.cpp` file has the IPC `encode`/`decode` specializations.

`AccessibilityTreeManager.cpp` / `AccessibilityTreeManager.h`
↳ Platform-agnostic C++ class that caches the accessibility tree in the UI process. Provides node lookup, hit-testing, focused-node tracking, and DFS-ordered text leaf enumeration. On `update_tree()`, compares old and new trees to detect aria-live-region content changes.

`ViewImplementation.cpp` / `ViewImplementation.h`
↳ Added `request_accessibility_tree()` and `perform_accessibility_action()` methods, plus `on_accessibility_tree_received` and `on_accessibility_focus_changed` callbacks.

`WebContentClient.cpp` / `WebContentClient.h`
↳ Client-side IPC handlers for `did_get_accessibility_tree` and `did_accessibility_focus_change` messages from WebContent. Dispatches to the `ViewImplementation` callbacks.

`Forward.h`
↳ Forward declaration for `AccessibilityNodeData`.

`PageInfo.h`
↳ Added `AccessibilityTree` to the `PageInfoType` enum, used by `test-web` for accessibility tree dumps.

### `Services/WebContent/`

`ConnectionFromClient.cpp` / `ConnectionFromClient.h`
↳ Implements `request_accessibility_tree` (builds and sends the full tree) and `perform_accessibility_action` (looks up a DOM node by `UniqueNodeID` and calls `HTMLElement::click()` or `run_focusing_steps()`). Also implements the `AccessibilityTree` page-info dump for `test-web`.

`PageClient.cpp` / `PageClient.h`
↳ Implements `page_did_change_active_element` (sends focus-change IPC). Adds `schedule_accessibility_tree_update()` with a 200ms debounce timer, called after accessibility actions to push tree updates when the page changes.

`WebContentServer.ipc`
↳ Added `request_accessibility_tree(page_id)` and `perform_accessibility_action(page_id, node_id, action)` endpoints.

`WebContentClient.ipc`
↳ Added `did_get_accessibility_tree(page_id, nodes)` and `did_accessibility_focus_change(page_id, node_id)` endpoints.

### `UI/AppKit/Interface/`

`LadybirdAccessibilityElement.h` / `LadybirdAccessibilityElement.mm`
↳ The main NSAccessibility wrapper. An `NSObject` subclass implementing the older “informal” protocol plus “modern” property overrides. Handles role mapping, ignored-element transparency, search-predicate navigation (DFS with leaf-role and container-descendant skipping), hit testing, actions, table navigation attributes, text-marker navigation, and coordinate conversion.

`LadybirdWebView.h` / `LadybirdWebView.mm`
↳ Integration point. Owns the `AccessibilityTreeManager` and element cache. Wires up IPC callbacks (tree received, focus changed, live-region announcements). Acts as `NSAccessibilityScrollAreaRole` containing the document root element. Posts `AXLoadComplete`, layout-changed, focus-changed, and announcement notifications. Delegates search predicates and text-marker queries to the document root.

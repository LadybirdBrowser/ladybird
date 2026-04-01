# Accessibility support

Ladybird exposes its internal accessibility tree to platform assistive technologies (AT) by serializing tree data from the WebContent process and presenting it through platform-specific wrapper objects in the UI process. On macOS, the AppKit UI uses Objective-C `NSAccessibility` wrappers (VoiceOver, Accessibility Inspector). The Qt UI uses `QAccessibleInterface` subclasses, bridged to AT-SPI2 on Linux (Orca) and to NSAccessibility on macOS (VoiceOver). The infrastructure — IPC serialization, tree management, and WebContent-side tree building — is shared between both UIs.

## Architecture

```
WebContent process                           UI process
------------------                           ----------
DOM + ARIA tree
    |
build_accessibility_tree()
    |
serialize_tree_as_node_data()
    |                                 AccessibilityTreeManager
    +--- IPC: did_get_accessibility   (C++, in LibWebView, shared)
         _tree(page_id, nodes) --->   HashMap<i64, AccessibilityNodeData>
                                           |
                          +----------------+----------------+
                          |                                 |
                     AppKit UI                            Qt UI
                          |                                 |
              LadybirdAccessibilityElement      AccessibilityInterface
              (Obj-C, NSObject subclass)        (QAccessibleInterface subclass)
                          |                                 |
              NSAccessibility protocol          Qt platform bridge
              (informal + modern APIs)          (QMacAccessibilityElement on macOS,
                          |                        AT-SPI2 adapter on Linux)
                          |                                 |
                    VoiceOver /                 [macOS only: runtime swizzling
                    Accessibility Inspector        adds AXWebArea, landmarks,
                                                   search predicates]
                                                            |
                                                     Orca / VoiceOver
```

That follows the Chromium/Firefox model rather than the WebKit model:

- No dependency on private Apple APIs (WebKit uses the private `NSAccessibilityRemoteUIElement` API)
- The accessibility wrapper objects live in the UI process
- The platform-agnostic parts (`AccessibilityNodeData`, `AccessibilityTreeManager`, IPC endpoints, WebContent serialization) are shared between both UIs

## IPC data type

`AccessibilityNodeData` (in `Libraries/LibWebView/`) is the serialization format. Each node carries:

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

## AppKit UI: NSAccessibility wrapper

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

Both paths must return consistent data. The “informal” protocol is the source of truth, with “modern” property overrides that delegate to `accessibilityAttributeValue:`. All relevant modern overrides must be present; on `NSObject`, the defaults return nil, and VoiceOver may treat nil-returning elements as invalid.

### Role mapping

ARIA roles map to `NSAccessibility` roles via `aria_role_to_ns_role()`. Landmark roles (banner, navigation, main, etc.) map to `NSAccessibilityGroupRole`, with `AXLandmark*` subrole strings. `NSAccessibilityRoleDescription()` doesn’t recognize the `AXLandmark*` subrole strings (it returns `group` for all of them) — so custom role description strings are provided, matching what Safari, Chrome, and Firefox all do.

The `document` role maps to `@"AXWebArea"` with role description `@“HTML content”`. That was an undocumented private WebKit string adopted by all browser engines until Apple made it a public constant (`NSAccessibilityWebAreaRole`) in macOS 26. Without `AXWebArea`, VoiceOver treats the entire page as a generic group and summarizes it — rather than offering web-specific navigation (rotor, search predicates, text markers).

### Tree exclusion

Per the [ARIA spec’s tree-exclusion rules](https://www.w3.org/TR/wai-aria-1.2/#tree_exclusion), elements in the following cases are excluded from the accessibility tree during `build_accessibility_tree()`, along with all their descendants:

- Elements with no layout node (`display:none`, HTML `hidden` attribute)
- Elements with `visibility:hidden` or `visibility:collapse` (those have layout nodes but aren’t visually perceptible)
- Elements with `aria-hidden=true`, including descendants (checked via `Element::is_aria_hidden()`, which walks up the ancestor chain)

Elements with `role=none` or `role=presentation` are also excluded, but their children are promoted to the parent (per the spec: _“their descendants and text content are generally included”_). If such an element has global ARIA attributes (like `aria-label`), the presentational role is overridden and the element is not excluded.

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

After an accessibility action is performed (press, focus), `schedule_accessibility_tree_update()` is called with a 200ms debounce timer to rebuild the tree and push it via `did_get_accessibility_tree`. That handles cases where the action changes the page content.

### LadybirdWebView integration

`LadybirdWebView` (`NSView` subclass) acts as the scroll-area container, with role `NSAccessibilityScrollAreaRole` and a single child: the document root `LadybirdAccessibilityElement`. It owns the `AccessibilityTreeManager` and element cache. Callbacks:

- `on_load_finish` → `request_accessibility_tree()` (direct)
- `on_load_start`, `on_url_change`, `on_title_change` → `scheduleAccessibilityTreeRequest` (debounced, 500ms). Multiple callbacks may fire in quick succession during navigation; `performSelector:afterDelay:` with `cancelPreviousPerformRequests` ensures only one tree request fires after they settle. That covers pages where `on_load_finish` doesn’t fire.
- `on_accessibility_tree_received` → update manager, clear cache, post `NSAccessibilityLayoutChangedNotification` and `@"AXLoadComplete"`
- `on_accessibility_focus_changed` → update manager, post `NSAccessibilityFocusedUIElementChangedNotification`
- `on_live_region_changed` → post `NSAccessibilityAnnouncementRequestedNotification`

`accessibilityFocusedUIElement` returns the first non-ignored element in document order (DFS), ensuring VoiceOver starts reading from the top of the page rather than from wherever JavaScript may have set DOM focus.

## Qt UI: QAccessibleInterface wrapper

The `AccessibilityInterface` class (in `UI/Qt/`) wraps one `AccessibilityNodeData` node, implementing `QAccessibleInterface`, `QAccessibleActionInterface`, `QAccessibleTextInterface`, and `QAccessibleTableCellInterface`. Each instance holds a node ID and a pointer to the `AccessibilityTreeManager`, and creates a dummy `QObject` parented to the `WebContentView` widget (Qt’s macOS bridge requires `object()` to return non-null).

`WebContentViewAccessible` is a `QAccessibleWidget` subclass for `WebContentView` — role `Grouping` (_not_ `Pane`, which Qt’s bridge ignores) — that returns the document root `AccessibilityInterface` as its child. `accessibility_factory()`, registered via `QAccessible::installFactory()`, tells Qt to use `WebContentViewAccessible` for `WebContentView` widgets.

`WebContentView` owns the `AccessibilityTreeManager` and element cache (`QHash<i64, AccessibilityInterface*>`). `accessibility_interface_for_node(node_id)` lazily creates and caches instances.

### Element cache lifecycle

The element cache must _not_ be cleared when a new accessibility tree arrives. Existing `AccessibilityInterface` objects must stay alive because Qt’s macOS bridge (`QMacAccessibilityElement`) holds references to them. Each interface queries the `AccessibilityTreeManager` dynamically, so it automatically reflects updated tree data. Stale interfaces (nodes no longer in the tree) are pruned after each update via `isValid()` checks and `QAccessible::deleteAccessibleInterface()`.

### Qt accessibility events cannot be used on macOS

Posting `QAccessibleEvent` objects via `QAccessible::updateAccessibility()` causes _“Invalid child in QAccessibleEvent”_ errors  — because Qt’s bridge tries to look up the `QMacAccessibilityElement` by ID, but the element may not exist yet (they’re created lazily). The workaround: `post_accessibility_focus_changed()` ensures the element exists via `+[QMacAccessibilityElement elementWithId:]` before posting `NSAccessibilityFocusedUIElementChangedNotification` directly via `NSAccessibilityPostNotification`.

### Runtime swizzling of Qt’s cocoa bridge (macOS only)

Qt’s cocoa bridge (`QMacAccessibilityElement`) lacks web-content support: it maps `WebDocument` to `NSAccessibilityGroupRole` (not `AXWebArea`), has no landmark subroles or role descriptions, and doesn’t implement `AXUIElementsForSearchPredicate`.

To add those capabilities, `WebContentViewAccessibilityMac.mm` swizzles seven methods at runtime:

1. **`accessibilityRole`**: Returns `@"AXWebArea"` for `WebDocument`, `NSAccessibilityGroupRole` for `ListItem` (Qt maps `ListItem` to `StaticText`, breaking list navigation).
2. **`accessibilitySubRole`**: Reads `_qt_mac_subrole` dynamic property for landmarks.
3. **`accessibilityRoleDescription`**: Reads `_qt_mac_roleDescription` dynamic property.
4. **`accessibilityParameterizedAttributeNames`**: Adds search predicate attributes.
5. **`accessibilityAttributeValue:forParameter:`**: Handles search predicate DFS traversal.
6. **`accessibilityFocusedUIElement`**: Returns the first leaf child for AXWebArea elements.
7. **`accessibilityIsIgnored`**: Returns YES for `ListItem` (see below).

### ListItem must be ignored on macOS

Qt’s `shouldBeIgnored()` does not include `ListItem`. Without the swizzle, VoiceOver visits both the `<li>` container and the link/button inside it, causing double-reading and focus-ring lag. Making listitems ignored via `accessibilityIsIgnored` causes macOS’s `NSAccessibilityUnignoredChildren` to promote their children to the parent level. In the search predicate, `ListItem` is also a container-only role — descended into but not added as a navigation stop.

### Tree requests and initial focus

Tree requests are debounced: `on_url_change` and `on_title_change` trigger requests via a 500ms `QTimer`; `on_load_finish` requests directly. After the tree arrives, `notify_accessibility_tree_loaded()` (after a 100ms delay) makes the NSView the Cocoa first responder, posts `AXLoadComplete`, and calls `NSAccessibilityHandleFocusChanged()`.

## Differences between AppKit and Qt implementations

| Aspect | AppKit | Qt |
| ------ | ------ | -- |
| Base class | `NSObject` with informal NSAccessibility protocol | `QAccessibleInterface` subclass |
| Navigation | `AXUIElementsForSearchPredicate` implemented directly | Same, via runtime swizzling of `QMacAccessibilityElement` on macOS |
| Document root role | `AXWebArea` (public `NSAccessibilityWebAreaRole` constant since macOS 26; previously an undocumented WebKit string) | `QAccessible::WebDocument`, swizzled to `@"AXWebArea"` on macOS |
| Container widget role | `NSAccessibilityScrollAreaRole` | `QAccessible::Grouping` (NOT `Pane`, which Qt’s bridge ignores) |
| Element cache lifecycle | Cleared on every tree update, interfaces recreated | Never cleared; interfaces persist and re-query updated tree manager. Stale interfaces pruned after each update. |
| Accessibility events | `NSAccessibilityPostNotification` for layout changes, focus, announcements | Focus events and live region announcements posted via native `NSAccessibilityPostNotification` (bypassing Qt’s event system) |
| Ignored element handling | `accessibilityIsIgnored` + `collectUnignoredChildren` | `collect_unignored_children` + `find_unignored_parent` at the `QAccessibleInterface` level; `accessibilityIsIgnored` swizzle for `ListItem` on macOS |
| Coordinate conversion | `convertRect:toView:nil` + `convertRectToScreen:` | `QWidget::mapToGlobal()` |
| Text leaf content | AXTitle=nil, AXValue=text (VoiceOver reads Value for StaticText) | Name=text, Value=empty (Qt bridge reads Name for navigation) |
| Actions | `accessibilityPerformAction:` sends IPC | `QAccessibleActionInterface::doAction()` sends IPC |

## Browser-engine source code research

The Ladybird accessibility implementation was informed by studying the Blink/Chromium, WebKit/Safari, and Gecko/Firefox accessibility code. All three implement the macOS accessibility protocol directly — they create their own Objective-C wrapper objects (not going through a toolkit bridge), giving them full control over role mapping, search predicates, and tree traversal. None of them need “workarounds” for the issues Qt’s bridge creates (see _Qt UI: QAccessibleInterface wrapper_ above).

### Chromium

Key files: `ui/accessibility/platform/ax_platform_node_cocoa.h` / `.mm`, `ui/accessibility/platform/one_shot_accessibility_tree_search.h` / `.cc`

**What we adopted:**

1. **IPC serialization model.** Chromium serializes the accessibility tree via Mojo IPC into the browser process. That is the fundamental architecture we adopted.

2. **`OneShotAccessibilityTreeSearch`.** Chromium implements `AXUIElementsForSearchPredicate` via a depth-first tree-search class that skips subtrees based on search criteria. That informed our search-predicate implementation — particularly the concept of “leaf-like” roles whose children are not individually navigable.

3. **`AXWebArea` role.** Chromium maps the document role to `@"AXWebArea"` (via `NSAccessibilityWebAreaRole`) — which causes VoiceOver to offer web-specific navigation features (rotor categories, search predicates, text markers). That behavior is not documented by Apple; it is inferred from observation.

4. **Custom landmark role descriptions.** Chromium has custom role-description strings for landmarks — rather than relying on `NSAccessibilityRoleDescription`, which doesn’t recognize `AXLandmark*` subrole strings. Landmark subroles are mapped via a static `BuildSubroleMap()` function.

**Where we diverge:**

1. **Base class.** `AXPlatformNodeCocoa` inherits from `NSAccessibilityElement`, not `NSObject`. We tried that but hit property-competition issues (see _“Why NSObject, not NSAccessibilityElement”_ above). Chromium makes `NSAccessibilityElement` work because it has a more elaborate initialization path and implements both the older “informal” protocol and “modern” property API in tandem.

2. **Dual API.** Chromium implements both the older “informal” protocol (`accessibilityAttributeValue:`, `accessibilityAttributeNames`, `accessibilityIsIgnored`) and the “modern” property API (`accessibilityRole`, `accessibilityTitle`, etc.) — with a migration flag to shift between them. We also implement both, but our informal protocol is the source of truth — with modern overrides delegating to it.

3. **Text markers on all elements.** Chromium exposes text-marker parameterized attributes (`AXTextMarkerForPosition`, `AXAttributedStringForTextMarkerRange`, etc.) on all elements — not just the document root. We restrict text markers to the `AXWebArea` element — because we found that exposing them on every element caused VoiceOver to treat structural containers as text content.

4. **No `shouldGroupAccessibilityChildren`.** Chromium doesn’t implement this method at all. We return `NO` on all elements — to prevent VoiceOver from grouping children by screen position, rather than document order.

5. **ListItem not ignored.** Chromium maps `ListItem` to `NSAccessibilityGroupRole` and doesn’t ignore it — it’s a full navigation stop. Chromium’s `OneShotAccessibilityTreeSearch` handles the navigation semantics so that VoiceOver doesn’t double-stop.

### WebKit

Key files: `Source/WebCore/accessibility/mac/WebAccessibilityObjectWrapperMac.mm`, `Source/WebCore/accessibility/mac/AccessibilityObjectMac.mm`, `Source/WebCore/accessibility/AXObjectCache.h`

**What we adopted:**

1. **Coordinate system.** By comparing against Safari’s element bounds in Accessibility Inspector, we confirmed that CSS pixels equal points on macOS — no device-pixel-ratio scaling is needed.

2. **Landmark role descriptions.** Safari provides custom role-description strings for landmarks — confirming that `NSAccessibilityRoleDescription()` doesn’t recognize `AXLandmark*` subrole strings.

**Architecture notes:**

1. **Private API.** WebKit uses Apple’s private `NSAccessibilityRemoteUIElement` Mach-port token-exchange API for cross-process accessibility. We chose not to use this approach.

2. **Search predicates.** `handleUIElementsForSearchPredicateAttribute()` parses VoiceOver’s search dictionary into an `AccessibilitySearchCriteria` struct and delegates to a multi-frame search engine. The search is more sophisticated than ours — it handles cross-frame searches and a wider set of search keys.

3. **Lazy cache.** `AXObjectCache` creates accessibility wrapper objects on demand via `getOrCreate()` and maps them via multiple lookup tables (Node → AXID, RenderObject → AXID). Objects are removed when the DOM node is destroyed.

### Firefox

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

2. **ListItem as a dedicated class.** Firefox has a `MOXListItemAccessible` subclass with specific overrides (e.g., `moxTitle` returns empty to avoid verbose announcements). ListItems are not ignored.

3. **No `shouldGroupAccessibilityChildren`.** Firefox doesn’t implement this, same as Chromium.

### Key cross-engine observations

- All three engines implement NSAccessibility directly on their own wrapper objects. None go through a toolkit bridge.
- All three map the document root to `AXWebArea`.
- All three provide custom role-description strings for landmarks.
- All three implement `AXUIElementsForSearchPredicate` directly.
- Chromium and Firefox both serialize the tree to the UI process; only WebKit does that differently (via private `NSAccessibilityRemoteUIElement` API).
- Firefox uses `NSObject` as the base class (like us); Chromium uses `NSAccessibilityElement`.
- Both Chromium and Firefox expose text markers on all elements; we restrict them to the document root.
- Neither Chromium nor Firefox implements `shouldGroupAccessibilityChildren`.

### Why our implementation diverges

The divergences are not caused by the IPC model — Chromium and Firefox also serialize over IPC. They stem from differences in wrapper-object lifecycle and tree management:

1. **Snapshot vs. live tree.** Our NSAccessibility wrappers are thin shells over a full-tree snapshot that is replaced wholesale on each update. Chromium and Firefox back their wrappers with rich, live object models that support incremental updates and on-demand attribute computation.

2. **Purpose-built traversal.** Chromium’s `OneShotAccessibilityTreeSearch` and Firefox’s `Pivot` are sophisticated tree-traversal engines with predicate matching, subtree skipping, and cross-frame support. Our search predicate is a simpler DFS that required explicit leaf-role and container-descendant handling to avoid double-reading.

3. **Years of iteration.** Chromium and Firefox have accumulated extensive VoiceOver interaction debugging over many years. Constraints we discovered through testing (text markers restricted to `AXWebArea`, `shouldGroupAccessibilityChildren` returning `NO`, `NSAccessibilityElement` property competition) are the kind of platform-specific quirks that the other engines must have either solved structurally — or else, because of their architecture — maybe never encountered to begin with.

### AccessKit

[AccessKit](https://github.com/AAccessKit/accesskit) is a Rust library that provides cross-platform accessibility adapters for macOS (NSAccessibility), Linux (AT-SPI2), and Windows (UIA). It handles the platform plumbing — role mapping, element lifecycle, event notifications, coordinate conversion — so applications don’t have to implement each platform’s accessibility protocol directly.

Key files: `platforms/macos/src/node.rs` (role mapping, attributes, actions), `platforms/macos/src/context.rs` (element caching), `platforms/macos/src/event.rs` (notifications), `consumer/src/filters.rs` (tree filtering).

**What AccessKit handles well:**

1. **Role mapping.** ~140 roles including `AXWebArea` for web content, landmark subroles (`AXLandmarkNavigation`, `AXLandmarkMain`, etc.), and custom role descriptions.

2. **`NSAccessibilityElement` base class.** AccessKit successfully uses `NSAccessibilityElement` as its base class (we switched to `NSObject` due to property-competition issues; Firefox also uses `NSObject`).

3. **Incremental tree updates.** `Tree::update_and_process_changes()` supports partial updates, unlike our full-snapshot replacement.

4. **Filtering model.** `FilterResult::Include` / `ExcludeNode` / `ExcludeSubtree` is cleaner than `accessibilityIsIgnored` — `ExcludeNode` promotes children without needing platform-specific ignore swizzles.

5. **Element lifecycle.** HashMap cache with on-demand creation, proper cleanup, and `UIElementDestroyedNotification` on drop.

6. **Focus forwarding.** Patches NSWindow to forward `accessibilityFocusedUIElement` to the content view — solving the same initial-focus problem we solved with `makeFirstResponder`.

7. **Cross-platform.** A single tree representation serves macOS, Linux (AT-SPI2), and Windows (UIA). The Linux adapter would give us AT-SPI2 support without relying on Qt’s bridge.

**What AccessKit does not provide for web content:**

1. **No `AXUIElementsForSearchPredicate`.** Without search predicates, VoiceOver falls back to sibling-only navigation within groups — it cannot do VO+Right/Left linear navigation through web content. That is the most complex part of our implementation, and even if we used AccessKit, we would still need to build that part ourselves.

2. **No `AXTextMarker`.** No cursor-level text navigation. AccessKit has NSRange-based text attributes but not the native marker objects that web content requires for character-by-character navigation and text selection.

3. **No web-content navigation semantics.** AccessKit is designed for native app accessibility, not browser-engine accessibility. The DFS traversal logic (leaf-role skipping, container-descendant skipping, search-key filtering) that makes VoiceOver navigation work smoothly in web content appears not to be part of AccessKit  — and that seems like a critical deficiency.

**Assessment:** AccessKit would eliminate some of the relatively low-implementation-effort platform plumbing we’re doing — but not the hard parts. The web-specific code — search predicates, text markers, container-descendant skipping, ignored-element semantics for web content — would still need to be implemented on top. The biggest win would be for the Qt UI, where AccessKit would replace the 7-method swizzle of `QMacAccessibilityElement` with a proper native accessibility layer. For the AppKit UI, the gain is more modest — since `LadybirdAccessibilityElement` already implements NSAccessibility directly. So the trade-off would adding a Rust build dependency for what amounts to a cleaner version of the platform-plumbing layer. But even then, with AccessKit, it seems we’d still be lacking the DFS traversal logic (leaf-role skipping, container-descendant skipping, search-key filtering) that makes VoiceOver navigation work smoothly in web content. And that on its own is a critical deficiency that would make AccessKit unsuitable for our needs.

### Future work: search-key filtering

Our search-predicate implementation returns all non-ignored elements in DFS order without filtering by the `AXSearchKey` parameter. VoiceOver sends search keys like `AXHeadingSearchKey`, `AXLinkSearchKey`, `AXLandmarkSearchKey`, etc., to request elements of a specific type. Currently VoiceOver handles the filtering itself, but implementing search-key filtering on our side would improve performance and correctness.

Chromium’s `OneShotAccessibilityTreeSearch` (`ui/accessibility/platform/one_shot_accessibility_tree_search.h` / `.cc`) uses `PredicateForSearchKey()` to map each VoiceOver search key to a C++ predicate function, then walks the tree returning only matching elements. Firefox’s `MOXSearchInfo` (`accessible/mac/MOXSearchInfo.mm`) does the same — via `PivotRule` subclasses. Both approaches avoid returning the entire flattened tree when VoiceOver only wants headings or links.

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

### `UI/Qt/`

`AccessibilityInterface.h` / `AccessibilityInterface.cpp`
↳ `AccessibilityInterface`: class, `QAccessibleInterface` subclass wrapping one `AccessibilityNodeData` node. Implements `QAccessibleActionInterface`, `QAccessibleTextInterface`, and `QAccessibleTableCellInterface`. Sets `_qt_mac_subrole` and `_qt_mac_roleDescription` dynamic properties on its backing `QObject` for landmark roles.

`WebContentView.h` / `WebContentView.cpp`
↳ Owns the `AccessibilityTreeManager` and element cache. Sets up IPC callbacks, debounced tree requests, and element pruning.

`WebContentViewAccessibilityMac.h` / `WebContentViewAccessibilityMac.mm` (macOS only)
↳ Runtime swizzling of 7 methods on `QMacAccessibilityElement`. Handles initial VoiceOver focus, focus-change notifications, and live-region announcements via native `NSAccessibilityPostNotification`.

`Tab.cpp`
↳ Calls `schedule_accessibility_tree_request()` in `on_load_start`, `on_url_change`, and `on_title_change` callbacks (which overwrite `WebContentView`’s callbacks).

## Known limitations

- **Qt on Linux: not tested with Orca.** Qt’s AT-SPI2 bridge correctly maps `QAccessible::WebDocument` to `ATSPI_ROLE_DOCUMENT_WEB`, so Orca should recognize the web content root without workarounds (unlike macOS, where we swizzle the role to `AXWebArea`). The runtime swizzling is macOS-only; on Linux, Qt’s bridge handles roles natively.

- **Qt on Linux: Collection interface requires Qt 6.11+.** Orca makes use of the AT-SPI2 `Collection` interface (`GetMatches` with `MatchRule`) for structural navigation through web content — the Linux equivalent of macOS’s `AXUIElementsForSearchPredicate`. Qt added `Collection` support in [Qt 6.11](https://codereview.qt-project.org/c/qt/qtbase/+/669871) (merged August 2025) — but Ladybird currently pins Qt 6.10, which doesn’t include it. So with Ladybird built against Qt 6.10 (without an upgrade to 6.11): Orca users, when browsing with Ladybird, will have a somewhat-degraded UX — one that falls back to just “basic” tree traversal — functional, but without the heading/link/landmark navigation other browsers provide on Linux (and that Orca users are likely to expect).

## Alternative: Toolkit-independent accessibility

The current implementation has separate presentation layers for each UI toolkit: On macOS, `LadybirdAccessibilityElement` (Objective-C) for AppKit, and `AccessibilityInterface` + swizzles for Qt. On Linux, if/when Ladybird ends up adding a GTK UI, that approach would require a third implementation.

Chromium and Firefox avoid that problem by implementing platform accessibility APIs directly, independent of the UI toolkit.

### How Chromium does it

Chromium’s `AXPlatformNode` abstraction (`ui/accessibility/platform/ax_platform_node.h`) has platform-specific subclasses — `AXPlatformNodeCocoa` (macOS), `AXPlatformNodeAuraLinux` (Linux), `AXPlatformNodeWin` (Windows) — that implement native APIs directly. An `AXPlatformNodeDelegate` interface provides toolkit-agnostic methods (`GetBoundsRect(AXCoordinateSystem::kScreen)`, `GetFocus()`, etc.) so the platform node never knows which toolkit hosts it.

On Linux, Chromium creates ATK objects directly via `g_object_new()` and registers them with AT-SPI2. It doesn’t go through GTK’s accessibility layer.

### How Firefox does it

Firefox’s `Accessible` base class (`accessible/basetypes/Accessible.h`) defines the platform-independent tree. Each platform provides an `AccessibleWrap` subclass with a `GetNativeInterface()` method that lazily creates the native wrapper: `MaiAtkObject` on Linux (a custom GLib type implementing ATK directly), `mozAccessible` on macOS.

On Linux, Firefox creates its own ATK objects and loads `libatk-bridge-2.0.so.0` at runtime to register with AT-SPI2. It doesn’t go through GTK widgets.

### Implications for Ladybird

The shared `AccessibilityTreeManager` in LibWebView is already toolkit-independent. Only the presentation layer is toolkit-specific. A toolkit-independent approach would mean:

**On macOS:** use `LadybirdAccessibilityElement` for both AppKit and Qt builds. The elements attach to whatever `NSView` the toolkit provides (via `QWidget::winId()` for Qt). That would eliminate our ~250 lines of Qt swizzle code — no `QAccessibleInterface`, no `QMacAccessibilityElement` workarounds.

**On Linux:** implement ATK directly (as Chromium and Firefox do) — creating a custom `AtkObject` subclass that wraps `AccessibilityNodeData`. That would work for both Qt and GTK builds, and wouldn’t depend on Qt’s AT-SPI2 bridge version (eliminating the Qt 6.11 Collection interface dependency).

| | Current approach | Toolkit-independent |
|---|---|---|
| macOS | AppKit: `LadybirdAccessibilityElement`; Qt: `AccessibilityInterface` + 7 swizzles | One `LadybirdAccessibilityElement` for both |
| Linux | Qt: `AccessibilityInterface` → Qt’s AT-SPI2 bridge | One ATK wrapper for both Qt and GTK |
| New toolkit | Requires new presentation layer | No additional work |

# Omnibox

Ladybird's omnibox combines URL entry, search, history, bookmarks, adaptive learning, and remote
search suggestions. The implementation is shared by the Qt and AppKit frontends through
`LibWebView`.

The central rule is that ordering, automatic selection, and inline completion are separate
decisions. A result may be useful enough to display near the top without being safe to select or
complete automatically.

## Code structure

The implementation is divided into a few focused components:

- `Omnibox` owns an editing session. It tracks the user's query, displayed text, selected row,
  completion state, popup state, and active query generation.
- `Autocomplete` coordinates local and remote suggestion delivery.
- `AutocompleteService` runs local database work on a serial worker thread.
- `AutocompleteRanker` retrieves and scores history, bookmark, and adaptive candidates.
- `AutocompleteMuxer` merges duplicate destinations, applies diversity rules, and constructs the
  final result list.
- `HistoryStore`, `BookmarkStore`, and the omnibox engagement tables provide local evidence.
- `UI/Qt/Autocomplete` and `UI/AppKit/Interface/Autocomplete` render the shared suggestion model.

`OmniboxSuggestionProvider` is the boundary between the editing state machine and suggestion
generation. The production provider wraps `Autocomplete`; tests use a scripted provider so they can
deliver results in any order without relying on timing.

## Query lifecycle

Every edit starts a new query generation identified by an incrementing `AutocompleteQueryID`.
Local retrieval begins immediately. Remote search suggestions are debounced for 100 milliseconds.

The local worker owns a separate SQLite connection. There is at most one running local query and
one pending query per client. A newer pending query replaces an older one. When possible, a newer
query interrupts stale SQLite work rather than waiting for it to finish.

Results are delivered incrementally:

1. Local results are ranked and published.
2. A remote response, if enabled and successful, is merged into the same query generation.
3. The combined list is published again.

Remote suggestions from the preceding query may remain visible while a compatible continuation is
in flight. Only suggestions that still have the new input as a prefix are retained. A successful
response replaces these provisional rows.

The UI thread discards results when their query ID is no longer active. Ending an editing session,
dismissing the popup, or starting another session cancels provider work and invalidates the active
generation. Stale results can therefore neither reopen the popup nor become an Enter action.

## Suggestion model

`AutocompleteSuggestion` contains:

- Its source: literal URL, history, bookmark, adaptive engagement, or search.
- Destination text and optional title, subtitle, and favicon.
- The input used for presentation highlighting.
- A match class and separate relevance components.
- Whether it is the verbatim action.
- Whether it may be automatically selected.
- Whether it may be used for inline completion.

The score components are retained after ranking so duplicate destinations can combine independent
history, bookmark, and adaptive evidence without losing their provenance.

### Destination identity

URL destinations are compared semantically. Scheme differences remain meaningful, while equivalent
serializations of the same URL are merged. `www.` and bare hosts share a diversity family but are
not otherwise treated as the same destination.

Search destinations are identified by Unicode case-folded text with whitespace normalized. This
allows an exact remote suggestion to merge with the verbatim search action.

When history, bookmark, and adaptive candidates identify the same URL, their score components and
eligibility flags are combined. Bookmark presentation is preferred so its title and favicon remain
visible.

## Matching

URL matching accepts the forms users commonly type by ignoring an `http://` or `https://` prefix,
and optionally `www.`, when comparing a query with a candidate. Matching is case-insensitive.

Match classes, from strongest to weakest, include:

- Exact URL.
- URL prefix.
- Exact bookmark title.
- Title word prefix.
- URL substring.
- Title substring.
- Exact or prefix adaptive association.

Title matching uses Unicode case folding. URL completion uses ASCII case-insensitive comparison
because serialized non-ASCII URL components are already encoded.

Bookmark titles and folders normally participate after three input code points. An exact bookmark
title participates at any non-empty length, so a bookmark named `GH` is retrieved by `GH`, `gh`, or
`Gh`. This exception does not turn partial two-character title prefixes into candidates.

An exact title match remains ineligible for automatic selection and inline completion. It is a
strong retrieval signal, not permission to replace the user's search action or editor text.

## Ranking

Ranking is deterministic and uses bounded integer components. Match quality is the largest signal;
history quality, bookmark evidence, and adaptive engagement then refine candidates within the same
general match quality.

### History

History ranking distinguishes direct navigation intent from passive visits. Visit and direct-visit
scores decay over time and are bounded before being added to a candidate score. Reloading or
repeatedly encountering a page cannot produce unbounded relevance.

For host-like input, history also creates aggregate origin candidates. Evidence comes from a capped
number of distinct pages, which prevents one frequently reloaded deep page from making its origin
look intentional. The origin may borrow a favicon from a matching page.

Deep history pages are suppressed for one- and two-character host queries unless the input contains
a URL path. This keeps short result lists from filling with incidental pages.

### Bookmarks

A bookmark contributes a fixed relevance bonus in addition to match quality. URL-prefix bookmark
matches are strong navigation evidence. Bookmark title and folder matches are useful for retrieval
but are not automatically selected or inline-completed.

Deep bookmarks remain eligible for a two-character URL prefix. Exact short title matches also remain
eligible, but do not become automatic actions.

### Adaptive engagements

The omnibox records committed input-to-destination associations. Search and URL destinations are
stored separately. Explicit row choices and automatic commitments have separate counts.

Exact associations are stronger than prefix associations. Short adaptive prefixes require repeated
explicit use before they can become automatic:

- A one-character deep destination requires three explicit uses.
- A two-character deep destination requires two explicit uses.
- Search-like adaptive URL inputs require two explicit uses.

Adaptive scores decay with time. Prefix evidence is scaled by the square root of the typed-prefix
length divided by the learned-input length, so very short prefixes receive less credit.

Merely displaying, automatically highlighting, or hovering a suggestion does not record an
engagement. A private session never records engagements.

## Result composition

The muxer first merges candidates with the same destination and sorts them by relevance. It then
constructs a bounded result list while preserving the verbatim action and limiting repetition.

The current limits are soft:

- At most four local navigation rows before deferred local rows.
- At most three remote search rows before deferred remote rows.
- At most two rows per origin for ordinary host queries.
- At most one row per origin and five total rows for one- and two-character host queries.

Deferred source rows may fill unused space after the first pass. This prevents a source cap from
leaving the popup unnecessarily sparse.

For short queries, candidates from the same origin are collapsed to one representative. Proven
adaptive destinations are preferred, followed by matching deep bookmarks, followed by origin rows.
An exact bookmark-title match is treated as a matching deep bookmark for this decision, preventing
an origin row from hiding it.

The highest-ranked candidate eligible for automatic selection is moved to the first row. A
higher-scoring but ineligible title or substring match may remain visible below it.

## Automatic selection

Automatic selection determines what Enter does when the user has not deliberately chosen another
row. It does not imply inline completion.

A candidate may be selected automatically when it is supported by strong navigational evidence,
for example:

- A sufficiently long URL prefix backed by direct visits.
- A bookmark URL prefix of at least two characters.
- An adaptive association whose evidence meets the short-prefix thresholds.

Title-only, folder, substring, previous-search, and remote suggestion matches are not automatic URL
actions. The verbatim URL-or-search action remains available in every non-empty result list.

Automatic defaults use hysteresis during incremental refreshes. A challenger replaces the current
default only when it exceeds the incumbent by both 10 percent and 100 relevance points. This avoids
visible selection churn as local and remote results arrive.

## Inline completion

Inline completion is intentionally stricter than automatic selection. A candidate must:

- Be eligible for automatic selection.
- Be a URL-prefix match.
- Extend the user's input at the end of the editor.
- Produce a non-empty suffix after optional scheme and `www.` handling.
- Not contain a query string unless an exact adaptive association permits it.

Title, folder, substring, search, and literal URL candidates never inline-complete. The typed prefix
is preserved exactly; only the borrowed suffix is selected.

An existing completion is retained across forward typing while it remains valid and is within 150
relevance points of the best inline candidate. A materially better non-inline default clears it.

Backspace suppresses completion for the resulting input. Typing a different input lifts the
suppression. Moving the cursor away from the end removes the completion and prevents late results
from applying another one. Right Arrow and End accept a completion, adopt the displayed text as the
new query, and requery it.

## Editing and activation

`Omnibox` keeps the user's query separate from borrowed display text. Display text has one of three
provenances:

- User text.
- Inline completion.
- Keyboard row preview.

Arrow keys explicitly select rows. A selected row may preview its destination in the editor while
the original query remains available for restoration. Pointer hover only changes row appearance; it
does not select, preview, or train the result.

Escape closes the popup and restores the query. Clicking outside does the same. If the user edits or
rejects a completion, Enter commits the text currently owned by the user rather than silently
choosing an unrelated automatic row.

An explicitly selected row remains selected across a refresh of the same generation when a result
with the same destination still exists. If it disappears, its preview is removed. Automatic
selection is recalculated separately.

Enter never waits for suggestions. If the displayed rows belong to an older query generation, the
current input is immediately committed through normal URL-or-search resolution.

## Persistence and deletion

History stores visit counts, direct-visit counts, last-use timestamps, and decayed scores. Redirect
chains distinguish direct navigation from visits produced by redirects so redirect targets do not
inherit typed intent.

Engagement rows store normalized input, destination kind, destination identity, explicit and
automatic use counts, and the last-use time. Database migrations add these fields to existing
profiles. The database layer supports floating-point values, statement interruption, and a busy
timeout because autocomplete reads concurrently with history writes.

Removing a history row also removes its engagement associations unless the destination remains
bookmarked. Forgetting a site and clearing all history remove the corresponding associations.

## Private browsing

Private autocomplete reads normal-profile history, bookmarks, and adaptive associations so local
suggestions remain useful. It does not write history or engagement data.

Remote suggestions use the private RequestServer connection. No history, bookmark, or engagement
data is sent to the search provider; the request contains only the query required by the configured
autocomplete engine.

## Presentation

Both native frontends render the same ordered suggestion list without source headers. Search rows
use a search icon. Local navigation rows use a favicon when available and otherwise a globe.
Bookmarks receive a star badge.

Matched portions of titles and URLs use shared match ranges from `LibWebView`, ensuring Qt and
AppKit highlight the same text. The match uses both weight and brightness so it remains visible in
dark and light themes.

The popup displays at most six rows before scrolling. Rows with a title use two lines; URL-only and
search rows use one. Selection and pointer hover have separate visual treatments.

## Settings

Remote search suggestions are controlled by the autocomplete-engine setting. Disabling suggestions
clears the configured engine; the setting is persisted with the rest of the browser settings and is
restored on the next launch.

The search engine used for the verbatim action is independent of whether remote suggestions are
enabled.

## Tests

The implementation has three main deterministic test layers:

- `TestAutocompleteRanker` verifies matching, relevance components, eligibility, decay, and short
  query behavior.
- `TestAutocompleteMuxer` verifies destination merging, diversity, source limits, verbatim
  preservation, and representative selection.
- `TestOmnibox` drives the editing state machine with a scripted provider, covering stale delivery,
  completion acceptance and rejection, selection preservation, activation, and engagement recording.

`TestAutocomplete` exercises the asynchronous integration with the application, service worker,
bookmark updates, private-profile reads, and remote failure handling. History-store tests cover
persisted ranking state, migrations, and deletion behavior.

Tests control result delivery or inject a clock rather than depending on wall-clock timing.

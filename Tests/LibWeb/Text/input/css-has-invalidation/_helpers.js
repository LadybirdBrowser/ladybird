// Shared helpers for :has() invalidation tests.
// Each test includes include.js first, then this file.

function printCounters(label) {
    const c = internals.getStyleInvalidationCounters();
    println(`[${label}]`);
    println(`  hasAncestorWalkInvocations: ${c.hasAncestorWalkInvocations}`);
    println(`  hasAncestorWalkVisits: ${c.hasAncestorWalkVisits}`);
    println(`  hasMatchInvocations: ${c.hasMatchInvocations}`);
    println(`  hasResultCacheHits: ${c.hasResultCacheHits}`);
    println(`  hasResultCacheMisses: ${c.hasResultCacheMisses}`);
    println(`  styleInvalidations: ${c.styleInvalidations}`);
}

// Force a style pass and reset counters so a subsequent mutation can be
// measured in isolation. Forces the rule cache and style invalidation data
// to be built by mutating + reading style, so the per-feature has-selector
// metadata is populated before counters are observed.
function settleAndReset(triggerElement) {
    // Two passes: the first ensures the rule cache + style invalidation data
    // is built; the second ensures any deferred work from a probe-only mutation
    // has settled. Without this, the very first mutation in a test can look
    // like a hit on the conservative "data not yet built" fallback.
    document.documentElement.classList.add("__settle__");
    getComputedStyle(triggerElement || document.documentElement).color;
    document.documentElement.classList.remove("__settle__");
    getComputedStyle(triggerElement || document.documentElement).color;
    internals.resetStyleInvalidationCounters();
}

// Shared helpers for :has() invalidation tests.
// Each test includes include.js first, then this file.

// Forget what the style engine counted so far, so that the next mutation is measured alone.
function resetCounters() {
    internals.resetStyleCounters();
}

// Print how many elements the style engine styled since the counters were last reset. With a bound, print
// only whether the count stays within it.
function printCounters(label, bound) {
    const styled = internals.styleCounters().styledElements;
    println(`[${label}]`);
    if (bound === undefined) {
        println(`  styled: ${styled}`);
    } else {
        println(`  styled <= ${bound}: ${styled <= bound}`);
    }
}

// Have the style engine own the document, settle style and reset the counters, so that a subsequent
// mutation can be measured in isolation.
function settleAndReset(triggerElement) {
    internals.updateStyle();
    document.documentElement.classList.add("__settle__");
    getComputedStyle(triggerElement || document.documentElement).color;
    document.documentElement.classList.remove("__settle__");
    getComputedStyle(triggerElement || document.documentElement).color;
    internals.updateStyle();
    resetCounters();
}

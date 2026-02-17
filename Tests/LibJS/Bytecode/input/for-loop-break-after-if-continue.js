// Test that break target after if/continue in an infinite for-loop
// emits Jump to the function end block (not an inlined End).

function bok(d) {
    for (;;) {
        if (d) continue;
        break;
    }
}

bok(false);

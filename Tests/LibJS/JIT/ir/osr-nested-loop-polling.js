// OPTIONS: stress-osr,threshold=400
// A cold loop shifts tier-up checks to an inner back edge that has no OSR entry. The interpreter must still find
// the outer loop's entry, even when its regular polling budget is a multiple of the number of back edges.

function nestedLoops(inner) {
    if (inner) {
        let i = 0;
        do {
            ++i;
        } while (i < 1);
    }
    for (let i = 0; i < 2000; ++i) {
        if (jit.inCompiledCode()) return true;
        if (inner) {
            let j = 0;
            do {
                ++j;
            } while (j < 1);
        }
    }
    return false;
}
jit.prepare(nestedLoops);
nestedLoops(false);
jit.compile(nestedLoops);
if (!nestedLoops(true)) throw new Error("The outer loop's OSR entry was never entered");

// CHECK-FUNCTION: nestedLoops
// CHECK-PASS: graph building
// CHECK: osr entry at pc

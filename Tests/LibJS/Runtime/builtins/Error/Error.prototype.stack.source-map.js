// Tests that Error.stack reports correct line numbers after bytecode
// peephole optimizations that skip, replace, or fuse instructions.

function extractLineNumber(error, functionName) {
    if (!error || !error.stack) return undefined;
    const re = new RegExp(`${functionName}.*:(\\d+):\\d+`);
    const match = error.stack.match(re);
    return match ? parseInt(match[1]) : undefined;
}

describe("source map correctness after peephole optimizations", () => {
    // Optimization: Jump-to-next-block elision.
    // The if-body block ends with a Jump to the very next block (after_if),
    // and that Jump is elided. Before the fix, a phantom source map entry
    // from the elided Jump sat at the offset where null.x's first
    // instruction is placed, giving it the wrong line number.
    test("jump to next block elision", () => {
        function peepholeJumpElision(x) {
            if (x) {
                x = 1;
            }
            null.x;
        }
        let caught;
        try {
            peepholeJumpElision(true);
        } catch (e) {
            caught = e;
        }
        expect(extractLineNumber(caught, "peepholeJumpElision")).toBe(22);
    });

    // Optimization: JumpIf to JumpFalse conversion.
    // When a JumpIf's true target is the very next block, it gets replaced
    // with a JumpFalse to the false target.
    test("jump-if to jump-false conversion", () => {
        function peepholeJumpFalse(x) {
            if (x) {
                return 1;
            }
            null.x;
        }
        let caught;
        try {
            peepholeJumpFalse(false);
        } catch (e) {
            caught = e;
        }
        expect(extractLineNumber(caught, "peepholeJumpFalse")).toBe(41);
    });

    // Optimization: JumpIf to JumpTrue conversion.
    // The logical OR operator generates a JumpIf where the false target
    // is the next block (evaluate rhs), so it becomes JumpTrue
    // (to the short-circuit target).
    test("jump-if to jump-true conversion", () => {
        function peepholeJumpTrue(x) {
            let y = x || null;
            y.foo;
        }
        let caught;
        try {
            peepholeJumpTrue(0);
        } catch (e) {
            caught = e;
        }
        expect(extractLineNumber(caught, "peepholeJumpTrue")).toBe(59);
    });

    // Optimization: fuse_compare_and_jump.
    // A comparison (e.g., LessThan) immediately followed by JumpIf is fused
    // into a single instruction (e.g., JumpLessThan). The comparison is
    // rewound, leaving duplicate source map entries at the same offset.
    test("fused compare and jump", () => {
        function peepholeCompareFuse(x) {
            if (x < 0) {
                return -1;
            }
            null.x;
        }
        let caught;
        try {
            peepholeCompareFuse(1);
        } catch (e) {
            caught = e;
        }
        expect(extractLineNumber(caught, "peepholeCompareFuse")).toBe(79);
    });

    // Combined: Multiple jump elisions from sequential if-return blocks,
    // each with fused comparisons.
    test("multiple sequential peephole optimizations", () => {
        function peepholeMultiple(x) {
            if (x === 1) {
                return "one";
            }
            if (x === 2) {
                return "two";
            }
            if (x > 100) {
                return "big";
            }
            null.x;
        }
        let caught;
        try {
            peepholeMultiple(42);
        } catch (e) {
            caught = e;
        }
        expect(extractLineNumber(caught, "peepholeMultiple")).toBe(103);
    });
});

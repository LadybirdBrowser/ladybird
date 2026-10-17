// JIT code that keeps exiting is discarded, and its executable compiled
// again once no frame that may run the old code is live anymore. These tests
// discard code while frames of it run below, through recursion and through
// direct calls.

const iterations = 200;

describe("discarded JIT code", () => {
    test("recursive frames of discarded code keep running", () => {
        function recurse(value, depth) {
            if (depth > 0) return recurse(value, depth - 1) + 1;
            // NB: Changing types make the code exit until it is discarded.
            return typeof value === "number" ? value * 2 : value + "!";
        }
        for (let i = 0; i < iterations; ++i) {
            expect(recurse(i, 5)).toBe(i * 2 + 5);
            if (i % 3 === 0) expect(recurse(`${i}`, 3)).toBe(`${i}!111`);
        }
    });

    test("directly called callees whose code is discarded while their callers run", () => {
        function callee(value) {
            let result = value;
            for (let i = 0; i < 3; ++i) result = typeof result === "number" ? result + 1 : result + "?";
            return result;
        }
        function caller(value, depth) {
            if (depth > 0) return caller(value, depth - 1);
            return callee(value);
        }
        for (let i = 0; i < iterations; ++i) {
            expect(caller(i, 2)).toBe(i + 3);
            if (i % 4 === 0) expect(caller({ toString: () => "o" }, 2)).toBe("o???");
        }
    });

    test("loops continue in code installed or exited while they run", () => {
        let total = 0;
        function add(value) {
            total += value;
            // NB: A path the code has not seen makes it exit, inlined or not.
            if (value > 1000) return "big";
            return total & 7;
        }
        function loop(count) {
            let seen = 0;
            for (let i = 0; i < count; ++i) {
                const result = add(i);
                if (result === "big") ++seen;
            }
            return seen;
        }
        for (let i = 0; i < 20; ++i) add(1);
        expect(loop(1000)).toBe(0);
        expect(loop(5000)).toBe(3999);
        expect(total).toBe(20 + 499500 + 12497500);
    });
});

describe("recompiles after discarded code", () => {
    test("code that exits where it reaches code that never ran is compiled again", () => {
        // NB: Like the per-frame step of an emulator, which reaches new branches as the program it runs goes on.
        const cases = Array.from(
            { length: 16 },
            (_, phase) => `case ${phase}: x = (x ^ ${phase}) + ${phase} | 0; break;`
        );
        const step = new Function("phase", "x", `switch (phase) { ${cases.join(" ")} } return x;`);
        jit.prepare(step);
        for (let i = 0; i < 10; ++i) step(0, i);
        expect(jit.compile(step)).toBe(jit.enabled);
        for (let phase = 1; phase < 16; ++phase) {
            for (let i = 0; i < 20; ++i) expect(step(phase, i)).toBe(((i ^ phase) + phase) | 0);
            expect(jit.compile(step)).toBe(jit.enabled);
        }
        // NB: Exits forced at random checks may come before the ones at sites that never ran, which then go unrecorded.
        if (!jit.forcesExits) expect(jit.discardCount(step)).toBe(jit.enabled ? 15 : 0);
    });

    test("code that keeps learning is not compiled anymore after many discards", () => {
        const cases = Array.from(
            { length: 80 },
            (_, phase) => `case ${phase}: x = (x ^ ${phase}) + ${phase} | 0; break;`
        );
        const step = new Function("phase", "x", `switch (phase) { ${cases.join(" ")} } return x;`);
        jit.prepare(step);
        for (let i = 0; i < 10; ++i) step(0, i);
        for (let phase = 0; phase < 80; ++phase) {
            for (let i = 0; i < 20; ++i) expect(step(phase, i)).toBe(((i ^ phase) + phase) | 0);
            jit.compile(step);
        }
        if (jit.enabled && !jit.forcesExits) {
            expect(jit.discardCount(step)).toBe(64);
            expect(jit.compile(step)).toBeFalse();
        }
        expect(step(79, 1)).toBe(((1 ^ 79) + 79) | 0);
    });

    test("discards that teach the code something start the count of fruitless discards over", () => {
        const step = new Function(
            "phase",
            "x",
            "switch (phase) { case 0: return x + 1 | 0; case 1: return x ^ 3; } return x;"
        );
        jit.prepare(step);
        for (let i = 0; i < 10; ++i) step(0, i);
        for (let i = 0; i < 7; ++i) {
            expect(jit.compile(step)).toBe(jit.enabled);
            jit.discard(step);
        }
        // The code exits where it reaches the case that never ran, which a recompile learns from.
        expect(jit.compile(step)).toBe(jit.enabled);
        for (let i = 0; i < 20; ++i) expect(step(1, i)).toBe(i ^ 3);
        for (let i = 0; i < 7; ++i) {
            expect(jit.compile(step)).toBe(jit.enabled);
            jit.discard(step);
        }
        expect(jit.compile(step)).toBe(jit.enabled);
    });

    test("code discarded again and again without new exits is not compiled anymore", () => {
        const add = new Function("a", "b", "return a + b;");
        jit.prepare(add);
        for (let i = 0; i < 10; ++i) add(i, 1);
        for (let i = 0; i < 8; ++i) {
            expect(jit.compile(add)).toBe(jit.enabled);
            jit.discard(add);
        }
        expect(jit.compile(add)).toBeFalse();
        expect(add(2, 3)).toBe(5);
    });
});

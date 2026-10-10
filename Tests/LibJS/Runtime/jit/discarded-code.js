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

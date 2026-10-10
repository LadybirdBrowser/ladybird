// JIT code makes the direct and native calls of strict inlined callees without the frames of the inlined calls they
// are in, which stack walks show from the call site, and which the calls' slow paths materialize: when the callee is
// another function, when it has no JIT code or exits to the interpreter, and when it throws. These tests run every
// function many times, so that with a low JIT threshold they are compiled, and check that what the callees see is
// the same throughout.

const iterations = 200;

function stackFunctionNames(error) {
    return error.stack
        .split("\n")
        .map(line => line.trim())
        .filter(line => line.startsWith("at "))
        .map(line => line.slice(3).split(" ")[0]);
}

describe("calls in inlined callees", () => {
    test("Error.stack shows the inlined callees", () => {
        // NB: The callee is too large to be inlined itself.
        function makeError(value) {
            "use strict";
            if (value === -1) {
                value = (value * 3 + 1) | 0;
                value = (value ^ 7) + 7;
                value = (value * 5 + 2) | 0;
                value = (value ^ 11) + 11;
                value = (value * 7 + 3) | 0;
                value = (value ^ 13) + 13;
                value = (value * 9 + 4) | 0;
                value = (value ^ 17) + 17;
            }
            return new Error(`here ${value}`);
        }
        function middle() {
            "use strict";
            return makeError(1);
        }
        function outer() {
            "use strict";
            return middle();
        }
        for (let i = 0; i < iterations; ++i) {
            const names = stackFunctionNames(outer());
            const start = names.indexOf("makeError");
            expect(names.slice(start, start + 3)).toEqual(["makeError", "middle", "outer"]);
        }
    });

    test("Error.stack of an error thrown by a native function called from an inlined callee", () => {
        function parse(text) {
            "use strict";
            return JSON.parse(text);
        }
        function middle(text) {
            "use strict";
            return parse(text) + 1;
        }
        function outer(text) {
            "use strict";
            return middle(text);
        }
        for (let i = 0; i < iterations; ++i) {
            const valid = i % 50 !== 49;
            if (valid) {
                expect(outer(String(i))).toBe(i + 1);
                continue;
            }
            let caught = null;
            try {
                outer("{");
            } catch (error) {
                caught = error;
            }
            expect(caught).toBeInstanceOf(SyntaxError);
            expect(stackFunctionNames(caught)).toContain("middle");
        }
    });

    test("the legacy caller of a function called from a strict inlined callee", () => {
        // NB: The callee is too large to be inlined itself.
        function sloppyCallee(value) {
            if (value === -1) {
                value = (value * 3 + 1) | 0;
                value = (value ^ 7) + 7;
                value = (value * 5 + 2) | 0;
                value = (value ^ 11) + 11;
                value = (value * 7 + 3) | 0;
                value = (value ^ 13) + 13;
                value = (value * 9 + 4) | 0;
                value = (value ^ 17) + 17;
            }
            return sloppyCallee.caller;
        }
        function middle() {
            "use strict";
            return sloppyCallee(1);
        }
        function outer() {
            return middle();
        }
        for (let i = 0; i < iterations; ++i) expect(outer()).toBeNull();
    });

    test("the legacy caller and arguments of a function called from a non-strict inlined callee", () => {
        // NB: The callee is too large to be inlined itself.
        function sloppyCallee(a, b) {
            if (a === -1) {
                a = (a * 3 + b) | 0;
                b = (b ^ a) + 7;
                a = (a * 5 + b) | 0;
                b = (b ^ a) + 11;
                a = (a * 7 + b) | 0;
                b = (b ^ a) + 13;
                a = (a * 9 + b) | 0;
                b = (b ^ a) + 17;
            }
            return [sloppyCallee.caller, Array.from(sloppyCallee.arguments)];
        }
        function middle(x) {
            return sloppyCallee(x, x + 1);
        }
        function outer(x) {
            return middle(x);
        }
        for (let i = 0; i < iterations; ++i) {
            const [caller, args] = outer(i);
            expect(caller).toBe(middle);
            expect(args).toEqual([i, i + 1]);
        }
    });

    test("the legacy caller and arguments of a non-strict inlined callee, from a function it calls", () => {
        // NB: The callee is too large to be inlined itself.
        function inspect(value) {
            if (value === -1) {
                value = (value * 3 + 1) | 0;
                value = (value ^ 7) + 7;
                value = (value * 5 + 2) | 0;
                value = (value ^ 11) + 11;
                value = (value * 7 + 3) | 0;
                value = (value ^ 13) + 13;
                value = (value * 9 + 4) | 0;
                value = (value ^ 17) + 17;
            }
            return [inspect.caller === middle, Array.from(middle.arguments), middle.caller === outer, value];
        }
        // NB: The middle function reads all its arguments, none of which it needs once it calls.
        function middle(a, b, c) {
            if (c === null) return b;
            return inspect(a + 1);
        }
        function outer(a) {
            return middle(a, `b${a}`, { c: a });
        }
        for (let i = 0; i < iterations; ++i) {
            const [callerIsMiddle, middleArguments, middleCallerIsOuter, value] = outer(i);
            expect(callerIsMiddle).toBeTrue();
            expect(middleArguments.length).toBe(3);
            expect(middleArguments[0]).toBe(i);
            expect(middleArguments[1]).toBe(`b${i}`);
            expect(middleArguments[2].c).toBe(i);
            expect(middleCallerIsOuter).toBeTrue();
            expect(value).toBe(i + 1);
        }
    });

    test("the legacy caller of a function called from an inlined closure", () => {
        // NB: The callee is too large to be inlined itself.
        function inspect(value) {
            if (value === -1) {
                value = (value * 3 + 1) | 0;
                value = (value ^ 7) + 7;
                value = (value * 5 + 2) | 0;
                value = (value ^ 11) + 11;
                value = (value * 7 + 3) | 0;
                value = (value ^ 13) + 13;
                value = (value * 9 + 4) | 0;
                value = (value ^ 17) + 17;
            }
            return inspect.caller;
        }
        function makeClosure(offset) {
            return function closure(value) {
                return [inspect(value + offset), closure];
            };
        }
        const closures = [makeClosure(1), makeClosure(2)];
        function outer(closure, value) {
            return closure(value);
        }
        for (let i = 0; i < iterations; ++i) {
            const closure = closures[i % 100 === 99 ? 1 : 0];
            const [caller, self] = outer(closure, i);
            expect(caller).toBe(self);
            expect(caller).toBe(closure);
        }
    });

    test("exceptions from callees caught in the inlined callee and further out", () => {
        function thrower(value) {
            "use strict";
            if (value % 7 === 3) throw new Error(`bad ${value}`);
            return value * 2;
        }
        function catchingMiddle(value) {
            "use strict";
            try {
                return thrower(value);
            } catch (error) {
                return error.message;
            }
        }
        function middle(value) {
            "use strict";
            return thrower(value) + 1;
        }
        function outer(value) {
            "use strict";
            let result;
            try {
                result = middle(value);
            } catch (error) {
                result = `outer ${error.message}`;
            }
            return [result, catchingMiddle(value)];
        }
        for (let i = 0; i < iterations; ++i) {
            const expected = i % 7 === 3 ? [`outer bad ${i}`, `bad ${i}`] : [i * 2 + 1, i * 2];
            expect(outer(i)).toEqual(expected);
        }
    });

    test("callees that exit to the interpreter or have no JIT code", () => {
        // NB: The callee is too large to be inlined itself.
        function callee(value) {
            "use strict";
            if (value === -1) {
                value = (value * 3 + 1) | 0;
                value = (value ^ 7) + 7;
                value = (value * 5 + 2) | 0;
                value = (value ^ 11) + 11;
                value = (value * 7 + 3) | 0;
                value = (value ^ 13) + 13;
                value = (value * 9 + 4) | 0;
                value = (value ^ 17) + 17;
            }
            return value.x + 1;
        }
        function middle(value, other) {
            "use strict";
            const before = other * 3;
            const result = callee(value);
            return [before, result, other];
        }
        function outer(value, other) {
            "use strict";
            return middle(value, other);
        }
        for (let i = 0; i < iterations; ++i) {
            // NB: The callee sees objects of other shapes, and strings, now and then.
            let value = { x: i };
            if (i % 40 === 39) value = { y: 0, x: i };
            if (i % 60 === 59) value = String(i);
            const [before, result, other] = outer(value, i);
            expect(before).toBe(i * 3);
            expect(other).toBe(i);
            if (typeof value === "string") expect(result).toBeNaN();
            else expect(result).toBe(i + 1);
        }
    });

    test("call sites that see another callee", () => {
        // NB: The callees are too large to be inlined themselves.
        function first(value) {
            "use strict";
            if (value === -1) {
                value = (value * 3 + 1) | 0;
                value = (value ^ 7) + 7;
                value = (value * 5 + 2) | 0;
                value = (value ^ 11) + 11;
                value = (value * 7 + 3) | 0;
                value = (value ^ 13) + 13;
                value = (value * 9 + 4) | 0;
                value = (value ^ 17) + 17;
            }
            return value + 1;
        }
        function second(value) {
            "use strict";
            if (value === -1) {
                value = (value * 3 + 1) | 0;
                value = (value ^ 7) + 7;
                value = (value * 5 + 2) | 0;
                value = (value ^ 11) + 11;
                value = (value * 7 + 3) | 0;
                value = (value ^ 13) + 13;
                value = (value * 9 + 4) | 0;
                value = (value ^ 17) + 17;
            }
            return value + 2;
        }
        function middle(callee, value) {
            "use strict";
            const kept = value * 5;
            return [callee(value), kept];
        }
        function outer(callee, value) {
            "use strict";
            return middle(callee, value);
        }
        for (let i = 0; i < iterations; ++i) {
            const callee = i % 25 === 24 ? second : first;
            expect(outer(callee, i)).toEqual([callee === first ? i + 1 : i + 2, i * 5]);
        }
    });

    test("values of the inlined callees survive garbage collection in their callees", () => {
        function collect(value) {
            "use strict";
            if (value % 50 === 0) gc();
            return value;
        }
        function middle(value) {
            "use strict";
            const object = { value, list: [value, value + 1] };
            const text = `text ${value}`;
            collect(value);
            return object.list[1] + object.value + text.length;
        }
        function outer(value) {
            "use strict";
            return middle(value);
        }
        for (let i = 0; i < iterations; ++i) expect(outer(i)).toBe(2 * i + 1 + `text ${i}`.length);
    });

    test("native functions that call back into JavaScript from inlined callees", () => {
        let names = null;
        function reviver(key, value) {
            "use strict";
            if (key === "") names = stackFunctionNames(new Error());
            return value;
        }
        function middle(text) {
            "use strict";
            return JSON.parse(text, reviver);
        }
        function outer(text) {
            "use strict";
            return middle(text);
        }
        for (let i = 0; i < iterations; ++i) {
            names = null;
            expect(outer(String(i))).toBe(i);
            expect(names).toContain("reviver");
            expect(names).toContain("middle");
            expect(names).toContain("outer");
        }
    });
});

// Exceptions thrown in functions the JIT inlines unwind through the frames
// it builds for them. These tests run every caller and callee many times, so
// that with a low JIT threshold the callees are inlined into their callers.

const iterations = 100;

describe("unwinding through inlined frames", () => {
    test("a throw in an inlined callee is caught in the caller", () => {
        function thrower(value) {
            if (value % 3 === 0) throw new Error(`thrown ${value}`);
            return value;
        }
        function middle(value) {
            return thrower(value) + 1;
        }
        function caller(value) {
            let result;
            try {
                result = middle(value);
            } catch (error) {
                result = error.message;
            }
            return result;
        }
        for (let i = 0; i < iterations; ++i) expect(caller(i)).toBe(i % 3 === 0 ? `thrown ${i}` : i + 1);
    });

    test("an uncaught throw leaves the compiled function", () => {
        function thrower(value) {
            if (value > iterations / 2) throw value;
            return value;
        }
        function middle(value) {
            return thrower(value) * 2;
        }
        function caller(value) {
            return middle(value) + 1;
        }
        for (let i = 0; i < iterations; ++i) {
            let result;
            try {
                result = caller(i);
            } catch (error) {
                result = `caught ${error}`;
            }
            expect(result).toBe(i > iterations / 2 ? `caught ${i}` : i * 2 + 1);
        }
    });

    test("a throw from a generic instruction in an inlined callee", () => {
        function check(key, object) {
            return key in object;
        }
        function middle(key, object) {
            return check(key, object) ? 1 : 0;
        }
        function caller(i) {
            const object = i % 4 === 3 ? i : { a: i };
            try {
                return middle("a", object);
            } catch (error) {
                return error.constructor.name;
            }
        }
        for (let i = 0; i < iterations; ++i) expect(caller(i)).toBe(i % 4 === 3 ? "TypeError" : 1);
    });

    test("errors created in inlined callees see every frame", () => {
        function inner(value) {
            return new Error(`error ${value}`).stack;
        }
        function middle(value) {
            return inner(value + 1);
        }
        function outer(value) {
            return middle(value) + "";
        }
        function frameNames(stack) {
            return stack
                .split("\n")
                .slice(1)
                .map(line => line.trim().split(" ")[1])
                .filter(name => name === "inner" || name === "middle" || name === "outer");
        }
        function innerFrame(stack) {
            return stack.split("\n").find(line => line.trim().startsWith("at inner "));
        }
        const firstInnerFrame = innerFrame(outer(0));
        for (let i = 0; i < iterations; ++i) {
            const stack = outer(i);
            expect(stack.startsWith(`Error: error ${i + 1}\n`)).toBeTrue();
            expect(frameNames(stack)).toEqual(["inner", "middle", "outer"]);
            expect(innerFrame(stack)).toBe(firstInnerFrame);
        }
    });

    test("stacks of errors thrown through inlined callees", () => {
        function inner(value) {
            if (value % 2) throw new TypeError(`odd ${value}`);
            return value;
        }
        function middle(value) {
            return inner(value) + 1;
        }
        function outer(value) {
            try {
                return middle(value);
            } catch (error) {
                return error.stack;
            }
        }
        for (let i = 0; i < iterations; ++i) {
            const result = outer(i);
            if (i % 2 === 0) {
                expect(result).toBe(i + 1);
                continue;
            }
            const names = result
                .split("\n")
                .slice(1)
                .map(line => line.trim().split(" ")[1]);
            expect(result.startsWith(`TypeError: odd ${i}\n`)).toBeTrue();
            expect(names.indexOf("inner")).toBeLessThan(names.indexOf("middle"));
            expect(names.indexOf("middle")).toBeLessThan(names.indexOf("outer"));
        }
    });
});

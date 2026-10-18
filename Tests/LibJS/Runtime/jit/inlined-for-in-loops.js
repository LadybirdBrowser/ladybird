// Callees with for-in loops are inlined into their callers. These tests run
// every caller and callee many times, so that with a low JIT threshold the
// callees are inlined, and then make the loops take their slow paths.

const iterations = 100;

describe("for-in loops in inlined callees", () => {
    test("keys of objects of the shape the loop saw, and of others", () => {
        function keys(object) {
            let result = "";
            for (const key in object) result += key;
            return result;
        }
        function caller(object) {
            return keys(object) + "!";
        }
        for (let i = 0; i < iterations; ++i) expect(caller({ a: 1, b: 2 })).toBe("ab!");
        expect(caller({ c: 1 })).toBe("c!");
        expect(caller([5, 6])).toBe("01!");
        expect(caller("xy")).toBe("01!");
        expect(caller(null)).toBe("!");
        for (let i = 0; i < iterations; ++i) expect(caller({ a: 1, b: 2 })).toBe("ab!");
    });

    test("the loop variable keeps the last key once the loop is done", () => {
        function lastKey(object) {
            let key;
            for (key in object);
            return key;
        }
        function caller(object) {
            return lastKey(object);
        }
        for (let i = 0; i < iterations; ++i) expect(caller({ a: 1, b: 2 })).toBe("b");
        expect(caller({})).toBeUndefined();
        expect(caller({ c: 3 })).toBe("c");
    });

    test("properties deleted during the loop are skipped", () => {
        function keysDeletingB(object) {
            const result = [];
            for (const key in object) {
                result.push(key);
                delete object.b;
            }
            return result;
        }
        function caller() {
            return keysDeletingB({ a: 1, b: 2, c: 3 });
        }
        for (let i = 0; i < iterations; ++i) expect(caller()).toEqual(["a", "c"]);
    });

    test("exceptions in the loop body leave the inlined callee", () => {
        function sum(object) {
            let total = 0;
            for (const key in object) total += object[key];
            return total;
        }
        function caller(object) {
            try {
                return sum(object);
            } catch (error) {
                return error.message;
            }
        }
        for (let i = 0; i < iterations; ++i) expect(caller({ a: i, b: 1 })).toBe(i + 1);
        const throwing = {
            a: 1,
            get b() {
                throw new Error("b");
            },
        };
        expect(caller(throwing)).toBe("b");
        for (let i = 0; i < iterations; ++i) expect(caller({ a: i, b: 1 })).toBe(i + 1);
    });

    test("a rarely called callee whose loop runs zero times", () => {
        function rare() {
            for (const key in {}) {
            }
        }
        function lengthSquared(point, i) {
            if (i % 10 === 0) rare();
            return point.x * point.x + point.y * point.y;
        }
        function run(count) {
            let sum = 0;
            for (let i = 0; i < count; ++i) sum += lengthSquared({ x: i, y: 1 }, i);
            return sum;
        }
        for (let i = 0; i < iterations; ++i) expect(run(20)).toBe(2490);
    });
});

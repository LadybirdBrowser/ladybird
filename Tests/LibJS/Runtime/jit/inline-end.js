// JIT code inlines calls of functions that end without a return, whose
// bytecode ends with End. These tests run every function many times, so that
// with a low JIT threshold they are compiled, and then make the inlined code
// exit.

const iterations = 100;

describe("inlined calls of functions without a return", () => {
    test("return undefined and keep their effects", () => {
        function setX(object, value) {
            object.x = value;
        }
        function run(count) {
            const object = { x: 0 };
            let results = 0;
            for (let i = 0; i < count; ++i) {
                if (setX(object, i) === undefined) ++results;
            }
            return [object.x, results];
        }
        for (let i = 0; i < iterations; ++i) expect(run(10)).toEqual([9, 10]);
    });

    test("exits inside them", () => {
        function addTo(object, value) {
            object.total = object.total + value;
        }
        function run(values) {
            const object = { total: 0 };
            for (const value of values) addTo(object, value);
            return object.total;
        }
        for (let i = 0; i < iterations; ++i) expect(run([1, 2, 3])).toBe(6);
        expect(run([1, "2", 3])).toBe("123");
    });
});

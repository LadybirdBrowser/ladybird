// JIT code reads and writes the bindings of closures and global variables
// inline, also in inlined callees, whose environment and realm are those of
// their function. These tests run every function many times, so that with a
// low JIT threshold they are compiled.

const iterations = 100;

var globalVariable = 1;
let globalLexical = 2;

describe("bindings in inlined callees", () => {
    test("captured variables", () => {
        function makeCounter(step) {
            let count = 0;
            function nested() {
                return function add(extra) {
                    count = (count + step + extra) | 0;
                    return count;
                };
            }
            return nested();
        }
        const counter = makeCounter(3);
        function caller(i) {
            return counter(i & 1);
        }
        let expected = 0;
        for (let i = 0; i < iterations; ++i) {
            expected += 3 + (i & 1);
            expect(caller(i)).toBe(expected);
        }
    });

    test("uninitialized and immutable bindings", () => {
        function make() {
            const writeConstant = value => {
                fixed = value;
            };
            const fixed = 1;
            let reads = 0;
            const read = () => (reads < iterations ? ++reads : late);
            let late = 5;
            return { read, writeConstant };
        }
        const { read, writeConstant } = make();
        function caller() {
            return read();
        }
        for (let i = 0; i < iterations; ++i) expect(caller()).toBe(i + 1);
        expect(caller()).toBe(5);

        function assign(value) {
            writeConstant(value);
        }
        for (let i = 0; i < iterations; ++i)
            expect(() => assign(i)).toThrowWithMessage(TypeError, "Invalid assignment to const variable");

        function tdz() {
            let value = 0;
            const get = () => value + (inner ?? 0);
            function run(i) {
                return get() + i;
            }
            for (let i = 0; i < iterations; ++i) {
                try {
                    run(i);
                } catch (error) {
                    value += error instanceof ReferenceError ? 1 : 100;
                }
            }
            let inner = 1;
            return [value, run(1)];
        }
        expect(tdz()).toEqual([iterations, iterations + 2]);
    });

    test("global variables", () => {
        function read() {
            return globalVariable + globalLexical;
        }
        function write(value) {
            globalVariable = value;
            globalLexical = value * 2;
        }
        function caller(i) {
            write(i);
            return read();
        }
        for (let i = 0; i < iterations; ++i) expect(caller(i)).toBe(3 * i);

        // An accessor on the global object takes the slow path.
        let getterCalls = 0;
        Object.defineProperty(globalThis, "globalAccessor", {
            get() {
                ++getterCalls;
                return 7;
            },
            configurable: true,
        });
        function readAccessor() {
            return globalAccessor;
        }
        function accessorCaller() {
            return readAccessor();
        }
        for (let i = 0; i < iterations; ++i) expect(accessorCaller()).toBe(7);
        expect(getterCalls).toBe(iterations);
        delete globalThis.globalAccessor;
        expect(() => accessorCaller()).toThrowWithMessage(ReferenceError, "'globalAccessor' is not defined");
    });
});

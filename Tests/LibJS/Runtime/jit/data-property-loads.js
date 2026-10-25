// JIT code loads and stores properties that the checked shape marks writable
// without checking for an accessor, as accessor properties are never
// writable. Properties the shape does not mark writable are still checked.
// These tests run every function many times, so that with a low JIT
// threshold they are compiled.

const iterations = 100;

describe("loads and stores of data properties", () => {
    test("accessors with the shape of a read-only data property", () => {
        // NB: Either shape may be the first the property lookup cache lists.
        function loadWritableFirst(object) {
            return object.x;
        }
        function loadReadOnlyFirst(object) {
            return object.x;
        }
        const writable = { x: 1 };
        const readOnly = Object.defineProperty({}, "x", { value: 2, enumerable: true, configurable: true });
        loadWritableFirst(readOnly);
        loadReadOnlyFirst(writable);
        for (let i = 0; i < iterations; ++i) {
            for (const load of [loadWritableFirst, loadReadOnlyFirst]) {
                expect(load(writable)).toBe(1);
                expect(load(readOnly)).toBe(2);
            }
        }
        const accessor = Object.defineProperty({}, "x", {
            get() {
                return 3;
            },
            enumerable: true,
            configurable: true,
        });
        for (let i = 0; i < iterations; ++i) {
            expect(loadWritableFirst(accessor)).toBe(3);
            expect(loadReadOnlyFirst(accessor)).toBe(3);
        }
    });

    test("properties redefined as accessors", () => {
        function load(object) {
            return object.x;
        }
        function store(object, value) {
            object.x = value;
        }
        const object = { x: 1 };
        for (let i = 0; i < iterations; ++i) {
            store(object, i);
            expect(load(object)).toBe(i);
        }
        let stored;
        Object.defineProperty(object, "x", {
            get() {
                return "getter";
            },
            set(value) {
                stored = value;
            },
        });
        store(object, "value");
        expect(stored).toBe("value");
        expect(load(object)).toBe("getter");
    });
});

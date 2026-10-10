// JIT code turns keyed accesses whose key is known at compile time, or the
// one key their cache saw, into named accesses. These tests run every
// function many times, so that with a low JIT threshold they are compiled.

const iterations = 100;

describe("keyed accesses by known keys in JIT code", () => {
    test("accessor-style keyed loads and stores", () => {
        class Store {
            constructor() {
                this.attributes = { a: 1, b: 2 };
            }
            get(key) {
                return this.attributes[key];
            }
            set(key, value) {
                this.attributes[key] = value;
            }
            sum() {
                return this.get("a") + this.get("b");
            }
        }
        const store = new Store();
        for (let i = 0; i < iterations; ++i) {
            store.set("a", i);
            expect(store.sum()).toBe(i + 2);
        }
        // Other keys, shapes and missing properties after compilation.
        store.attributes.c = 3;
        expect(store.sum()).toBe(iterations - 1 + 2);
        expect(store.get("c")).toBe(3);
        expect(store.get("missing")).toBeUndefined();
        const key = "b";
        expect(store.get(key)).toBe(2);
        expect(store.get("a".repeat(1))).toBe(iterations - 1);
        store.attributes = { b: 10, a: 20 };
        expect(store.sum()).toBe(30);
        Object.defineProperty(store.attributes, "a", { get: () => 40 });
        expect(store.sum()).toBe(50);
    });

    test("keyed accesses with symbol keys", () => {
        const key = Symbol("key");
        function read(object) {
            return object[key];
        }
        function write(object, value) {
            object[key] = value;
        }
        const object = { [key]: 0 };
        for (let i = 0; i < iterations; ++i) {
            write(object, i);
            expect(read(object)).toBe(i);
        }
        expect(read({ [key]: "other" })).toBe("other");
        expect(read({})).toBeUndefined();
    });
});

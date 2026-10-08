describe("per-site caches of keyed property accesses", () => {
    function read(object, key) {
        return object[key];
    }

    function write(object, key, value) {
        object[key] = value;
    }

    test("string keys with the same contents but different identities", () => {
        const object = { alpha: 1, beta: 2 };
        const prefix = "al";
        for (let i = 0; i < 10; ++i) {
            expect(read(object, "alpha")).toBe(1);
            expect(read(object, prefix + "pha")).toBe(1);
            expect(read(object, "be" + "ta")).toBe(2);
        }
    });

    test("symbol keys", () => {
        const first = Symbol("first");
        const second = Symbol("first");
        const object = { [first]: 1, [second]: 2 };
        for (let i = 0; i < 10; ++i) {
            expect(read(object, first)).toBe(1);
            expect(read(object, second)).toBe(2);
        }
        write(object, first, 3);
        expect(object[first]).toBe(3);
        expect(object[second]).toBe(2);
    });

    test("writes to existing properties and property additions", () => {
        const object = { x: 0 };
        for (let i = 0; i < 10; ++i) write(object, "x", i);
        expect(object.x).toBe(9);

        const objects = [];
        for (let i = 0; i < 10; ++i) {
            const added = {};
            write(added, "a", i);
            write(added, "b", i + 1);
            objects.push(added);
        }
        for (let i = 0; i < 10; ++i) {
            expect(Object.keys(objects[i])).toEqual(["a", "b"]);
            expect(objects[i].b).toBe(i + 1);
        }
    });

    test("accessors, setters and non-writable properties", () => {
        let reads = 0;
        const getter = {
            get value() {
                ++reads;
                return 42;
            },
        };
        for (let i = 0; i < 5; ++i) expect(read(getter, "value")).toBe(42);
        expect(reads).toBe(5);

        const setter = {
            set value(v) {
                this.stored = v * 2;
            },
        };
        for (let i = 0; i < 5; ++i) write(setter, "value", i);
        expect(setter.stored).toBe(8);

        const frozen = Object.freeze({ x: 1 });
        for (let i = 0; i < 5; ++i) write(frozen, "x", i);
        expect(frozen.x).toBe(1);

        const prototype = { set inherited(v) {} };
        const child = Object.create(prototype);
        for (let i = 0; i < 5; ++i) write(child, "inherited", i);
        expect(Object.hasOwn(child, "inherited")).toBeFalse();
    });

    test("shape changes, deletions and prototype changes", () => {
        const object = { x: 1, y: 2 };
        for (let i = 0; i < 5; ++i) expect(read(object, "y")).toBe(2);
        delete object.x;
        expect(read(object, "y")).toBe(2);
        object.y = 3;
        expect(read(object, "y")).toBe(3);
        expect(read(object, "x")).toBeUndefined();

        const prototype = { inherited: 1 };
        const child = Object.create(prototype);
        for (let i = 0; i < 5; ++i) expect(read(child, "inherited")).toBe(1);
        prototype.inherited = 2;
        expect(read(child, "inherited")).toBe(2);
        Object.setPrototypeOf(child, { inherited: 3 });
        expect(read(child, "inherited")).toBe(3);
    });

    test("many shapes and keys at one site", () => {
        const objects = [];
        for (let i = 0; i < 32; ++i) {
            const object = { ["own" + i]: i };
            object.shared = i;
            objects.push(object);
        }
        for (let round = 0; round < 3; ++round) {
            for (let i = 0; i < objects.length; ++i) {
                expect(read(objects[i], "shared")).toBe(i + round);
                expect(read(objects[i], "own" + i)).toBe(i);
                write(objects[i], "shared", i + round + 1);
                expect(objects[i].shared).toBe(i + round + 1);
            }
        }
    });

    test("keys that are not cached", () => {
        const array = [1, 2, 3];
        for (let i = 0; i < 5; ++i) {
            expect(read(array, "1")).toBe(2);
            expect(read(array, "length")).toBe(3);
        }
        write(array, "length", 1);
        expect(array).toEqual([1]);

        const typed = new Uint8Array(2);
        for (let i = 0; i < 5; ++i) {
            expect(read(typed, "-0")).toBeUndefined();
            expect(read(typed, "1")).toBe(0);
        }

        const proxy = new Proxy({}, { get: (target, key) => "proxied " + String(key) });
        for (let i = 0; i < 5; ++i) expect(read(proxy, "x")).toBe("proxied x");
    });
});


test("a keyed site handles replacement string keys after collection", () => {
    const object = {};
    let suffix = 0;
    function read(object, key) {
        return object[key];
    }
    function primeCache() {
        const key = "collected-cache-key-" + suffix;
        object[key] = 11;
        for (let i = 0; i < 8; ++i) expect(read(object, key)).toBe(11);
    }
    primeCache();
    gc();
    gc();
    suffix = 1;
    const replacement = "replacement-cache-key-" + suffix;
    object[replacement] = 22;
    for (let i = 0; i < 8; ++i) expect(read(object, replacement)).toBe(22);
    expect(object["collected-cache-key-0"]).toBe(11);
});

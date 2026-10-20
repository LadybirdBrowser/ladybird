// JIT code at keyed accesses that saw too many (shape, key) pairs for their own caches looks string keys up in the
// VM's keyed caches, like the interpreter: missing properties read undefined, and stores add properties to objects of
// the shapes the store cache remembers additions for. The functions are compiled once their sites have given up on
// their own caches.

const keys = [];
for (let i = 0; i < 600; ++i) keys.push(`key${i}`);

function compiledAfter(parameters, body, warmUp) {
    const fn = new Function(...parameters, `return ${body};`);
    jit.prepare(fn);
    warmUp(fn);
    jit.compile(fn);
    return fn;
}

describe("keyed accesses through the VM's caches in JIT code", () => {
    test("missing and present properties of many shapes", () => {
        // Objects with every even or every odd key, and one key of their own, so that the VM's lookup cache holds
        // entries for missing and for present properties of many shapes, which also share entries.
        const objects = [];
        for (let i = 0; i < 8; ++i) {
            const object = {};
            object[`own${i}`] = i;
            for (let k = i % 2; k < keys.length; k += 2) object[keys[k]] = k;
            objects.push(object);
        }
        const expected = (object, k) => (k % 2 === objects.indexOf(object) % 2 ? k : undefined);
        const get = compiledAfter(["object", "key"], "object[key]", get => {
            for (const object of objects) for (const key of keys) get(object, key);
        });
        for (let round = 0; round < 3; ++round) {
            for (const object of objects) {
                for (let k = 0; k < keys.length; ++k) expect(get(object, keys[k])).toBe(expected(object, k));
            }
        }
    });

    test("properties that the prototype chain gains later", () => {
        const prototype = {};
        const objects = [];
        for (let i = 0; i < 40; ++i) {
            const object = Object.create(prototype);
            object[`own${i}`] = i;
            objects.push(object);
        }
        const get = compiledAfter(["object", "key"], "object[key]", get => {
            for (const object of objects) for (const key of keys) get(object, key);
        });
        for (const object of objects) expect(get(object, "key3")).toBeUndefined();
        prototype.key3 = "inherited";
        for (const object of objects) expect(get(object, "key3")).toBe("inherited");
        Object.prototype.key4 = "very inherited";
        try {
            for (const object of objects) expect(get(object, "key4")).toBe("very inherited");
        } finally {
            delete Object.prototype.key4;
        }
        for (const object of objects) expect(get(object, "key4")).toBeUndefined();
    });

    test("objects that answer for themselves", () => {
        const objects = [];
        for (let i = 0; i < 40; ++i) objects.push({ [`own${i}`]: i });
        const get = compiledAfter(["object", "key"], "object[key]", get => {
            for (const object of objects) for (const key of keys) get(object, key);
        });
        const reads = [];
        const proxy = new Proxy(
            {},
            {
                get(target, key) {
                    reads.push(key);
                    return "trapped";
                },
            }
        );
        expect(get(proxy, "key1")).toBe("trapped");
        expect(reads).toEqual(["key1"]);
        const args = (function () {
            return arguments;
        })(1, 2);
        expect(get(args, "key1")).toBeUndefined();
        expect(get(args, "length")).toBe(2);
    });

    test("additions of many keys", () => {
        const make = () => {
            const object = {};
            for (let k = 0; k < keys.length; k += 3) object[keys[k]] = k;
            return object;
        };
        const put = compiledAfter(["object", "key", "value"], "(object[key] = value)", put => {
            for (let round = 0; round < 3; ++round) {
                const object = make();
                for (const key of keys) put(object, key, key);
            }
        });
        for (let round = 0; round < 3; ++round) {
            const object = make();
            for (let k = 0; k < keys.length; ++k) put(object, keys[k], -k);
            for (let k = 0; k < keys.length; ++k) expect(object[keys[k]]).toBe(-k);
            expect(Object.keys(object).length).toBe(keys.length);
        }
        const sealed = make();
        Object.preventExtensions(sealed);
        put(sealed, "key1", 1);
        expect(Object.hasOwn(sealed, "key1")).toBeFalse();
        const prototype = {};
        const stores = [];
        Object.defineProperty(prototype, "key1", {
            set(value) {
                stores.push(value);
            },
        });
        const inheriting = Object.assign(Object.create(prototype), make());
        put(inheriting, "key1", "set");
        expect(stores).toEqual(["set"]);
        expect(Object.hasOwn(inheriting, "key1")).toBeFalse();
    });
});

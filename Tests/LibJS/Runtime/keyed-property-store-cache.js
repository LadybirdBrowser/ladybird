// Keyed stores whose own caches give up look in the VM's keyed property
// store cache. The store sites here first see so many keys that their own
// caches give up.

function write(object, key, value) {
    object[key] = value;
}

function writeStrict(object, key, value) {
    "use strict";
    object[key] = value;
}

for (let i = 0; i < 3000; ++i) {
    write({}, "warmup" + i, i);
    writeStrict({}, "warmup" + i, i);
}

const keys = ["a", "b", "c", "d", "e", "f", "g", "h"];

function makeObjects() {
    const objects = [];
    for (let shape = 0; shape < keys.length; ++shape) {
        const object = {};
        for (let i = 0; i <= shape; ++i) object[keys[(shape + i) % keys.length]] = 0;
        objects.push(object);
    }
    return objects;
}

describe("keyed property store cache", () => {
    test("own data properties on objects of many shapes", () => {
        const objects = makeObjects();
        for (let round = 0; round < 50; ++round) {
            for (const object of objects) {
                for (const key of Object.keys(object)) write(object, key, round);
            }
        }
        for (const object of objects) {
            for (const key of Object.keys(object)) expect(object[key]).toBe(49);
        }
    });

    test("properties that become accessors or read-only", () => {
        const objects = makeObjects();
        for (let round = 0; round < 50; ++round) {
            for (const object of objects) write(object, "a", round);
        }
        const object = objects[0];
        let stored;
        Object.defineProperty(object, "a", {
            get: () => stored,
            set: value => {
                stored = value * 2;
            },
            configurable: true,
        });
        write(object, "a", 5);
        expect(stored).toBe(10);
        expect(object.a).toBe(10);

        Object.defineProperty(object, "a", { value: 1, writable: false, configurable: true });
        write(object, "a", 7);
        expect(object.a).toBe(1);
        expect(() => writeStrict(object, "a", 7)).toThrow(TypeError);
    });

    test("frozen and sealed objects", () => {
        const objects = makeObjects();
        for (let round = 0; round < 50; ++round) {
            for (const object of objects) write(object, "a", round);
        }
        const frozen = Object.freeze(objects[1]);
        write(frozen, "a", "changed");
        expect(frozen.a).toBe(49);
        expect(() => writeStrict(frozen, "a", "changed")).toThrow(TypeError);
        const sealed = Object.seal(objects[2]);
        write(sealed, "a", "changed");
        expect(sealed.a).toBe("changed");
        write(sealed, "new", 1);
        expect(sealed.new).toBeUndefined();
    });

    test("setters on the prototype chain and additions", () => {
        const objects = makeObjects();
        let setterValue;
        const prototype = {
            set a(value) {
                setterValue = value;
            },
        };
        const child = Object.create(prototype);
        for (let round = 0; round < 50; ++round) {
            for (const object of objects) write(object, "a", round);
            write(child, "a", round);
            expect(setterValue).toBe(round);
            expect(Object.hasOwn(child, "a")).toBeFalse();
        }
        const added = {};
        for (let i = 0; i < 50; ++i) write(added, "k" + (i % 10), i);
        expect(added.k9).toBe(49);
        expect(Object.keys(added).length).toBe(10);
    });

    test("additions to objects of the same shape", () => {
        const objects = makeObjects();
        const prototype = {};
        const make = () => Object.create(prototype);
        for (let round = 0; round < 50; ++round) {
            for (const object of objects) write(object, "a", round);
            const fresh = make();
            write(fresh, "x", round);
            write(fresh, "y", -round);
            expect(fresh.x).toBe(round);
            expect(fresh.y).toBe(-round);
            expect(Object.keys(fresh)).toEqual(["x", "y"]);
        }

        // Objects that cannot be extended do not get the property.
        const sealed = Object.preventExtensions(make());
        write(sealed, "x", 1);
        expect(Object.hasOwn(sealed, "x")).toBeFalse();
        expect(() => writeStrict(Object.preventExtensions(make()), "x", 1)).toThrow(TypeError);

        // A setter added to the prototype chain afterwards takes the put.
        let setterValue;
        Object.defineProperty(prototype, "x", {
            set(value) {
                setterValue = value;
            },
            configurable: true,
        });
        const withSetter = make();
        write(withSetter, "x", 2);
        expect(setterValue).toBe(2);
        expect(Object.hasOwn(withSetter, "x")).toBeFalse();
        delete prototype.x;
        const afterwards = make();
        write(afterwards, "x", 3);
        expect(afterwards.x).toBe(3);

        // A read-only property on the prototype chain stops the addition.
        Object.defineProperty(prototype, "y", { value: "inherited", writable: false, configurable: true });
        const readOnly = make();
        write(readOnly, "x", 4);
        write(readOnly, "y", 4);
        expect(readOnly.x).toBe(4);
        expect(Object.hasOwn(readOnly, "y")).toBeFalse();
        expect(readOnly.y).toBe("inherited");
    });

    test("additions that grow the storage of objects", () => {
        const objects = makeObjects();
        for (let round = 0; round < 20; ++round) {
            for (const object of objects) write(object, "a", round);
            const grown = {};
            for (let i = 0; i < 40; ++i) write(grown, "p" + i, i);
            for (let i = 0; i < 40; ++i) expect(grown["p" + i]).toBe(i);
        }
    });

    test("dictionary objects", () => {
        const dictionary = {};
        for (let i = 0; i < 100; ++i) dictionary["p" + i] = i;
        for (let i = 0; i < 50; ++i) delete dictionary["p" + i];
        const objects = makeObjects();
        for (let round = 0; round < 50; ++round) {
            for (const object of objects) write(object, "a", round);
            write(dictionary, "p60", round);
            write(dictionary, "p70", -round);
        }
        expect(dictionary.p60).toBe(49);
        expect(dictionary.p70).toBe(-49);
        delete dictionary.p60;
        write(dictionary, "p70", "after");
        expect(dictionary.p70).toBe("after");
        expect(dictionary.p60).toBeUndefined();
        Object.defineProperty(dictionary, "p70", { get: () => "getter", set: () => {}, configurable: true });
        write(dictionary, "p70", "ignored");
        expect(dictionary.p70).toBe("getter");
    });

    test("array lengths and symbol keys", () => {
        const objects = makeObjects();
        const symbol = Symbol("key");
        const array = [1, 2, 3, 4];
        for (let round = 0; round < 50; ++round) {
            for (const object of objects) write(object, "a", round);
            write(array, "length", 4);
            write(objects[3], symbol, round);
        }
        write(array, "length", 2);
        expect(array).toEqual([1, 2]);
        expect(objects[3][symbol]).toBe(49);
    });
});

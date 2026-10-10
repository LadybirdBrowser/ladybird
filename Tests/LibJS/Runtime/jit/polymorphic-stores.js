// JIT code stores named properties through every entry of polymorphic and
// megamorphic property caches, not only the most recently used one. These
// tests run every function many times, so that with a low JIT threshold they
// are compiled.

const iterations = 100;

function store(object, value) {
    object.value = value;
}

// Objects of many shapes that all have a `value` data property.
function objectsOfManyShapes(count) {
    const objects = [];
    for (let i = 0; i < count; ++i) {
        const object = {};
        object[`key${i}`] = i;
        object.value = 0;
        objects.push(object);
    }
    return objects;
}

describe("Polymorphic and megamorphic stores in JIT code", () => {
    test("changing existing properties of many shapes", () => {
        const objects = objectsOfManyShapes(40);
        for (let i = 0; i < iterations; ++i) {
            for (const object of objects) store(object, i);
        }
        for (const object of objects) expect(object.value).toBe(iterations - 1);
    });

    test("adding properties to objects of many shapes", () => {
        for (let i = 0; i < iterations; ++i) {
            const objects = [];
            for (let j = 0; j < 20; ++j) {
                const object = {};
                object[`key${j}`] = j;
                store(object, i);
                objects.push(object);
            }
            for (let j = 0; j < 20; ++j) {
                expect(objects[j].value).toBe(i);
                expect(Object.keys(objects[j])).toEqual([`key${j}`, "value"]);
            }
        }
    });

    test("a few shapes, with additions and changes", () => {
        for (let i = 0; i < iterations; ++i) {
            const a = { a: 1 };
            const b = { b: 1 };
            const c = { c: 1, value: 1 };
            store(a, i);
            store(b, i);
            store(c, i);
            store(a, i + 1);
            expect(a.value).toBe(i + 1);
            expect(b.value).toBe(i);
            expect(c.value).toBe(i);
        }
    });

    test("stores that must not go through the cache", () => {
        const objects = objectsOfManyShapes(40);
        for (let i = 0; i < iterations; ++i) {
            for (const object of objects) store(object, i);
        }

        const frozen = Object.freeze(objectsOfManyShapes(1)[0]);
        store(frozen, "frozen");
        expect(frozen.value).toBe(0);

        const nonWritable = objectsOfManyShapes(1)[0];
        Object.defineProperty(nonWritable, "value", { writable: false });
        store(nonWritable, "non-writable");
        expect(nonWritable.value).toBe(0);

        let setterValue;
        const withSetter = objectsOfManyShapes(1)[0];
        Object.defineProperty(withSetter, "value", {
            set(value) {
                setterValue = value;
            },
        });
        store(withSetter, "setter");
        expect(setterValue).toBe("setter");

        const nonExtensible = Object.preventExtensions({ key0: 0 });
        store(nonExtensible, "added");
        expect(Object.hasOwn(nonExtensible, "value")).toBeFalse();

        let prototypeSetterValue;
        const withPrototypeSetter = Object.create({
            set value(value) {
                prototypeSetterValue = value;
            },
        });
        withPrototypeSetter.key0 = 0;
        store(withPrototypeSetter, "prototype setter");
        expect(prototypeSetterValue).toBe("prototype setter");
        expect(Object.hasOwn(withPrototypeSetter, "value")).toBeFalse();
    });
});

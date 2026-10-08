// A GetByValue that cycles through more (shape, key) pairs than its own property lookup cache can learn gives up on
// it, and looks its string keys up in the VM's keyed property lookup cache. These tests use such sites, and then change
// the properties they find there.

const keys = [];
for (let i = 0; i < 1500; ++i) keys.push(`key${i}`);

function makeObject() {
    const object = {};
    for (let i = 0; i < keys.length; ++i) object[keys[i]] = i;
    return object;
}

function get(object, key) {
    return object[key];
}

describe("GetByValue through the VM's keyed property lookup cache", () => {
    test("own data properties under many keys", () => {
        const object = makeObject();
        for (let round = 0; round < 4; ++round) {
            for (let i = 0; i < keys.length; ++i) expect(get(object, keys[i])).toBe(i);
        }
        expect(get(object, "missing")).toBeUndefined();
    });

    test("changed, deleted and accessor properties", () => {
        const object = makeObject();
        for (let round = 0; round < 2; ++round) {
            for (let i = 0; i < keys.length; ++i) expect(get(object, keys[i])).toBe(i);
        }
        object.key3 = "three";
        expect(get(object, "key3")).toBe("three");
        delete object.key5;
        expect(get(object, "key5")).toBeUndefined();
        Object.defineProperty(object, "key7", {
            get() {
                return "seven";
            },
        });
        expect(get(object, "key7")).toBe("seven");
        for (let i = 8; i < keys.length; ++i) expect(get(object, keys[i])).toBe(i);
    });

    test("other objects with the same keys", () => {
        const objects = [makeObject(), makeObject(), Object.assign(Object.create({ inherited: 1 }), makeObject())];
        for (let round = 0; round < 2; ++round) {
            for (const object of objects) {
                for (let i = 0; i < keys.length; ++i) expect(get(object, keys[i])).toBe(i);
                expect(get(object, "inherited")).toBe(object === objects[2] ? 1 : undefined);
            }
        }
    });

    test("keys made at run time", () => {
        const object = makeObject();
        for (let round = 0; round < 2; ++round) {
            for (let i = 0; i < keys.length; ++i) expect(get(object, "key" + i)).toBe(i);
        }
    });
});

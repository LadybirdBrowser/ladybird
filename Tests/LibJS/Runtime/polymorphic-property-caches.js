const readValue = object => object.value;

describe("polymorphic property caches", () => {
    test("own and inherited data properties of objects with different shapes", () => {
        const prototype = { value: "inherited" };
        const objects = [{ value: 1 }, { other: 0, value: 2 }, Object.create(prototype), { a: 0, b: 0, value: 4 }];
        for (let round = 0; round < 3; ++round) {
            expect(objects.map(readValue)).toEqual([1, 2, "inherited", 4]);
        }
        prototype.value = "changed";
        expect(objects.map(readValue)).toEqual([1, 2, "changed", 4]);
        Object.defineProperty(prototype, "value", { get: () => "getter" });
        expect(objects.map(readValue)).toEqual([1, 2, "getter", 4]);
    });

    test("prototype chains that change after being cached", () => {
        const base = { value: "base" };
        const middle = Object.create(base);
        const objects = [{ value: 1 }, { x: 0, value: 2 }, Object.create(middle), { y: 0, value: 4 }];
        for (let round = 0; round < 3; ++round) expect(readValue(objects[2])).toBe("base");
        middle.value = "middle";
        expect(objects.map(readValue)).toEqual([1, 2, "middle", 4]);
        delete middle.value;
        expect(objects.map(readValue)).toEqual([1, 2, "base", 4]);
        Object.setPrototypeOf(middle, { value: "replaced" });
        expect(objects.map(readValue)).toEqual([1, 2, "replaced", 4]);
    });

    test("accessors, missing properties and dictionary objects", () => {
        let calls = 0;
        const withGetter = {
            get value() {
                ++calls;
                return "getter";
            },
        };
        const dictionary = {};
        for (let i = 0; i < 100; ++i) dictionary["p" + i] = i;
        dictionary.value = "dictionary";
        const objects = [{ value: 1 }, withGetter, { missing: true }, dictionary];
        for (let round = 0; round < 3; ++round) {
            expect(objects.map(readValue)).toEqual([1, "getter", undefined, "dictionary"]);
        }
        expect(calls).toBe(3);
        delete dictionary.p50;
        dictionary.value = "changed";
        expect(objects.map(readValue)).toEqual([1, "getter", undefined, "changed"]);
        delete dictionary.value;
        expect(readValue(dictionary)).toBeUndefined();
    });
});

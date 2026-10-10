// JIT code may embed the shapes its property accesses saw. These tests drop
// every object of such a shape, collect garbage and allocate objects with new
// shapes, which must never be mistaken for the old ones.

function makeShapes(count, keys) {
    const objects = [];
    for (let i = 0; i < count; ++i) {
        const object = {};
        for (const key of keys) object[key + i] = i;
        object.a = i;
        objects.push(object);
    }
    return objects;
}

describe("JIT code whose embedded cells lose every other reference", () => {
    test("calling again after a collection", () => {
        function read(object) {
            return object.a;
        }

        let warm = [
            { a: 1, b: 2 },
            { a: 3, b: 4 },
        ];
        for (let i = 0; i < 100; ++i) expect(read(warm[i % 2])).toBe(i % 2 ? 3 : 1);
        warm = null;
        gc();

        const others = makeShapes(200, ["x", "y"]);
        for (let i = 0; i < others.length; ++i) expect(read(others[i])).toBe(i);
    });

    test("a collection while the code runs", () => {
        let warm = { a: 1, b: 2 };
        function readAround(object, callback) {
            const before = object.a;
            const next = callback();
            return before + next.a;
        }

        for (let i = 0; i < 100; ++i) expect(readAround(warm, () => ({ a: 10, b: 20 }))).toBe(11);

        const result = readAround(warm, () => {
            warm = null;
            gc();
            return makeShapes(200, ["p", "q", "r"])[123];
        });
        expect(result).toBe(124);
    });
});

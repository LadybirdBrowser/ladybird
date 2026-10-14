// JIT code may embed the shapes its property accesses saw. These tests drop
// every object of such a shape, collect garbage and allocate objects with new
// shapes, which must never be mistaken for the old ones.
//
// The code holds the cells it embeds weakly, unless a frame runs it: once one
// of them dies, the code is dropped, and the function may be compiled again.

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

    test("the code is dropped once a cell it embeds dies", () => {
        const read = new Function("object", "return object.a;");
        jit.prepare(read);
        // NB: The objects only live in this nested function's frame, so that none lingers in a register of the
        //     test's frame.
        (() => {
            const prototype = { a: 1 };
            const object = Object.create(prototype);
            for (let i = 0; i < 10; ++i) expect(read(object)).toBe(1);
            expect(jit.compile(read)).toBe(jit.enabled);
        })();
        gc();
        expect(jit.isCompiled(read)).toBeFalse();
        expect(jit.discardCount(read)).toBe(0);

        const object = Object.create({ a: 2 });
        for (let i = 0; i < 10; ++i) expect(read(object)).toBe(2);
        expect(jit.compile(read)).toBe(jit.enabled);
        expect(read(object)).toBe(2);
    });
});

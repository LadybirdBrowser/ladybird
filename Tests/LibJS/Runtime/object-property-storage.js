test("named properties survive storage growth and garbage collection", () => {
    const objects = [];
    for (let count of [1, 2, 3, 5, 9, 17, 100, 1100, 2500]) {
        const object = {};
        for (let i = 0; i < count; ++i) object["p" + i] = i * 2;
        objects.push([object, count]);
        gc();
    }
    gc();
    for (const [object, count] of objects) {
        expect(Object.keys(object)).toHaveLength(count);
        for (let i = 0; i < count; ++i) expect(object["p" + i]).toBe(i * 2);
    }
});

test("property values in storage keep objects alive", () => {
    const object = {};
    for (let i = 0; i < 40; ++i) object["p" + i] = { value: i, nested: [i, String(i)] };
    for (let i = 0; i < 1000; ++i) ({ garbage: [i] });
    gc();
    gc();
    for (let i = 0; i < 40; ++i) {
        expect(object["p" + i].value).toBe(i);
        expect(object["p" + i].nested[1]).toBe(String(i));
    }
});

test("cached property additions grow storage in the middle of adding", () => {
    function build(count) {
        const object = {};
        // The same additions every time, so the later objects take them from property lookup caches.
        for (let i = 0; i < count; ++i) object["q" + i] = { i };
        return object;
    }
    const kept = [];
    for (let round = 0; round < 50; ++round) {
        kept.push(build(30));
        if (round % 10 === 0) gc();
    }
    gc();
    for (const object of kept) {
        for (let i = 0; i < 30; ++i) expect(object["q" + i].i).toBe(i);
    }
});

test("array elements survive storage growth and garbage collection", () => {
    const arrays = [];
    for (let length of [1, 4, 5, 13, 100, 1023, 1024, 1025, 5000]) {
        const array = [];
        for (let i = 0; i < length; ++i) array.push({ i });
        arrays.push(array);
        gc();
    }
    gc();
    for (const array of arrays) {
        for (let i = 0; i < array.length; ++i) expect(array[i].i).toBe(i);
    }
});

test("holey arrays and arrays that go through dictionary storage", () => {
    const holey = [];
    holey[10] = "ten";
    holey[3] = "three";
    gc();
    expect(holey.length).toBe(11);
    expect(holey[3]).toBe("three");
    expect(holey[4]).toBeUndefined();
    expect(4 in holey).toBeFalse();

    const sparse = [];
    sparse[100000] = { far: true };
    sparse[0] = { near: true };
    gc();
    expect(sparse[100000].far).toBeTrue();
    expect(sparse[0].near).toBeTrue();

    const object = { length: 0 };
    Object.defineProperty(object, 0, { value: "a", writable: false, configurable: true });
    object[1] = "b";
    delete object[0];
    object[0] = "c";
    gc();
    expect(object[0]).toBe("c");
    expect(object[1]).toBe("b");
});

test("array storage replaced by array operations", () => {
    const array = Array.from({ length: 2000 }, (_, i) => ({ i }));
    gc();
    array.length = 3;
    gc();
    expect(array.map(element => element.i)).toEqual([0, 1, 2]);
    const filled = new Array(1500).fill(null).map((_, i) => [i]);
    gc();
    expect(filled[1499][0]).toBe(1499);
    const args = (function () {
        return arguments;
    })(...filled);
    gc();
    expect(args.length).toBe(1500);
    expect(args[777][0]).toBe(777);
});

test("object literals of every size keep their properties inline and grow beyond them", () => {
    const literals = [];
    for (let round = 0; round < 3; ++round) {
        literals.push({});
        literals.push({ a: 1 });
        literals.push({ a: 1, b: 2, c: 3 });
        literals.push({ a: 1, b: 2, c: 3, d: 4, e: 5 });
        literals.push({ a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7 });
        literals.push({ a: 1, b: 2, c: 3, d: 4, e: 5, f: 6, g: 7, h: 8, i: 9, j: 10, k: 11 });
        literals.push({
            a: 1,
            b: 2,
            c: 3,
            d: 4,
            e: 5,
            f: 6,
            g: 7,
            h: 8,
            i: 9,
            j: 10,
            k: 11,
            l: 12,
            m: 13,
            n: 14,
            o: 15,
        });
        literals.push({
            a: 1,
            b: 2,
            c: 3,
            d: 4,
            e: 5,
            f: 6,
            g: 7,
            h: 8,
            i: 9,
            j: 10,
            k: 11,
            l: 12,
            m: 13,
            n: 14,
            o: 15,
            p: 16,
            q: 17,
            r: 18,
            s: 19,
            t: 20,
        });
    }
    gc();
    for (const literal of literals) {
        const keys = Object.keys(literal);
        keys.forEach((key, index) => expect(literal[key]).toBe(index + 1));
        for (let i = 0; i < 20; ++i) literal["added" + i] = { i };
    }
    gc();
    for (const literal of literals) {
        for (let i = 0; i < 20; ++i) expect(literal["added" + i].i).toBe(i);
        expect(literal.a === undefined || literal.a === 1).toBeTrue();
    }
});

test("strict arguments objects", () => {
    function capture() {
        "use strict";
        return arguments;
    }
    const kept = [];
    for (let i = 0; i < 100; ++i) kept.push(capture({ i }, String(i)));
    gc();
    kept.forEach((args, i) => {
        expect(args.length).toBe(2);
        expect(args[0].i).toBe(i);
        expect(args[1]).toBe(String(i));
        args.extra = i;
        expect(args.extra).toBe(i);
    });
});

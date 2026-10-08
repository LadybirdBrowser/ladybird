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

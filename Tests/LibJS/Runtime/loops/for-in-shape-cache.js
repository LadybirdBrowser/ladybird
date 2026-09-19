function keysOf(object) {
    const keys = [];
    for (const key in object) {
        keys.push(key);
    }
    return keys;
}

function keysOfAgain(object) {
    const keys = [];
    for (const key in object) {
        keys.push(key);
    }
    return keys;
}

test("objects of one shape enumerated at different sites", () => {
    const a = { x: 1, y: 2 };
    const b = { x: 3, y: 4 };
    expect(keysOf(a)).toEqual(["x", "y"]);
    expect(keysOfAgain(b)).toEqual(["x", "y"]);
    expect(keysOfAgain(a)).toEqual(["x", "y"]);
});

test("adding, deleting and reconfiguring properties between enumerations", () => {
    const a = { x: 1, y: 2 };
    const b = { x: 3, y: 4 };
    expect(keysOf(a)).toEqual(["x", "y"]);
    b.z = 5;
    expect(keysOf(b)).toEqual(["x", "y", "z"]);
    expect(keysOfAgain(a)).toEqual(["x", "y"]);
    delete a.x;
    expect(keysOf(a)).toEqual(["y"]);
    Object.defineProperty(b, "y", { enumerable: false });
    expect(keysOfAgain(b)).toEqual(["x", "z"]);
    Object.defineProperty(b, "y", { enumerable: true });
    expect(keysOf(b)).toEqual(["x", "y", "z"]);
});

test("prototype changes between enumerations", () => {
    const prototype = { p: 1 };
    const c = Object.create(prototype);
    c.q = 1;
    const d = Object.create(prototype);
    d.q = 2;
    expect(keysOf(c)).toEqual(["q", "p"]);
    prototype.r = 2;
    expect(keysOfAgain(d)).toEqual(["q", "p", "r"]);
    delete prototype.p;
    expect(keysOf(c)).toEqual(["q", "r"]);
    Object.setPrototypeOf(d, { s: 3 });
    expect(keysOfAgain(d)).toEqual(["q", "s"]);
    expect(keysOf(c)).toEqual(["q", "r"]);
});

test("arrays of one shape with different lengths", () => {
    expect(keysOf([1, 2])).toEqual(["0", "1"]);
    expect(keysOfAgain([1, 2, 3])).toEqual(["0", "1", "2"]);
    expect(keysOf([1])).toEqual(["0"]);
    const array = [1, 2];
    array.named = true;
    expect(keysOfAgain(array)).toEqual(["0", "1", "named"]);
    expect(keysOf([1, 2])).toEqual(["0", "1"]);
});

test("dictionary objects enumerated repeatedly", () => {
    const object = {};
    for (let i = 0; i < 40; ++i) {
        object["key" + i] = i;
    }
    delete object.key0;
    const expected = keysOf(object);
    expect(expected.length).toBe(39);
    expect(keysOfAgain(object)).toEqual(expected);
    object.key0 = 0;
    expect(keysOf(object)).toEqual([...expected, "key0"]);
    delete object.key5;
    expect(keysOfAgain(object)).toEqual(keysOf(object));
    expect(keysOf(object)).not.toContain("key5");
});

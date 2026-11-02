// Plain objects keep their first named property where indexed storage would go until they get an indexed property.

function expectProperties(object, a, b) {
    expect(object.a).toBe(a);
    expect(object.b).toBe(b);
}

test("named properties survive the first indexed property", () => {
    const object = { a: 1, b: 2 };
    object[0] = "zero";
    expectProperties(object, 1, 2);
    expect(object[0]).toBe("zero");
    expect(Object.keys(object)).toEqual(["0", "a", "b"]);
});

test("a full inline storage still takes more named properties afterwards", () => {
    const object = { a: 1, b: 2 };
    object[5] = "five";
    for (let i = 0; i < 40; ++i) object[`p${i}`] = i;
    expectProperties(object, 1, 2);
    expect(object[5]).toBe("five");
    for (let i = 0; i < 40; ++i) expect(object[`p${i}`]).toBe(i);
});

test("indexed properties after named storage outgrew the cell", () => {
    const object = {};
    for (let i = 0; i < 40; ++i) object[`p${i}`] = i;
    expect(object[0]).toBeUndefined();
    object[0] = "zero";
    object[1] = "one";
    expect(object[0]).toBe("zero");
    expect(object[1]).toBe("one");
    for (let i = 0; i < 40; ++i) expect(object[`p${i}`]).toBe(i);
});

test("reading indexed properties of an object without them", () => {
    const object = { a: 1.5, b: "string" };
    for (let i = 0; i < 3; ++i) expect(object[i]).toBeUndefined();
    expect(0 in object).toBeFalse();
    expect(Object.keys(object)).toEqual(["a", "b"]);
    expect(Object.getOwnPropertyDescriptor(object, "0")).toBeUndefined();
    expect(delete object[0]).toBeTrue();
    expectProperties(object, 1.5, "string");
});

test("array methods on plain objects", () => {
    const pushed = { a: 1, b: 2, length: 0 };
    Array.prototype.push.call(pushed, "x", "y");
    expect(pushed.length).toBe(2);
    expect(pushed[1]).toBe("y");
    expect(pushed.a).toBe(1);

    const sliced = Array.prototype.slice.call({ a: 1, 0: "x", 1: "y", length: 2 });
    expect(sliced).toEqual(["x", "y"]);
});

test("sparse and non-default indexed properties", () => {
    const sparse = { a: 1, b: 2 };
    sparse[1000000] = "far";
    expectProperties(sparse, 1, 2);
    expect(sparse[1000000]).toBe("far");

    const accessor = { a: 1, b: 2 };
    Object.defineProperty(accessor, 0, { get: () => "got", enumerable: false });
    expectProperties(accessor, 1, 2);
    expect(accessor[0]).toBe("got");
    accessor[1] = "one";
    expect(accessor[1]).toBe("one");

    const frozen = { a: 1, b: 2, 0: "zero" };
    Object.freeze(frozen);
    expectProperties(frozen, 1, 2);
    expect(frozen[0]).toBe("zero");
});

test("deleting the elements and adding them back", () => {
    const object = { a: 1, b: 2 };
    object[0] = "zero";
    delete object[0];
    expect(object[0]).toBeUndefined();
    expectProperties(object, 1, 2);
    object[0] = "again";
    expect(object[0]).toBe("again");
    expectProperties(object, 1, 2);
});

test("prototype and dictionary objects", () => {
    const prototype = { a: 1 };
    const child = Object.create(prototype);
    prototype[3] = "three";
    expect(child[3]).toBe("three");
    expect(child.a).toBe(1);

    const dictionary = {};
    for (let i = 0; i < 200; ++i) dictionary[`key${i}`] = i;
    for (let i = 0; i < 190; ++i) delete dictionary[`key${i}`];
    dictionary[0] = "zero";
    expect(dictionary[0]).toBe("zero");
    for (let i = 190; i < 200; ++i) expect(dictionary[`key${i}`]).toBe(i);
});

// The rest runs often enough for compiled code to allocate the objects and add their properties.
const ITERATIONS = 20000;

test("object literals with computed keys in between", () => {
    function literal(key, value) {
        return { a: value, [key]: "computed", b: value + 1, c: value + 2 };
    }
    for (let i = 0; i < ITERATIONS; ++i) {
        const object = literal(i % 16 === 0 ? 0 : "other", i);
        if (object.a !== i || object.b !== i + 1 || object.c !== i + 2) expect().fail();
    }
    const object = literal(0, 7);
    expect(object[0]).toBe("computed");
    expect(object.c).toBe(9);
});

test("constructors that add named properties after indexed ones", () => {
    function Constructed(indexed, value) {
        if (indexed) this[0] = "zero";
        this.a = value;
        this.b = value + 1;
        this.c = value + 2;
    }
    for (let i = 0; i < ITERATIONS; ++i) {
        const object = new Constructed(i % 16 === 0, i);
        if (object.a !== i || object.b !== i + 1 || object.c !== i + 2) expect().fail();
        if (i % 16 === 0 && object[0] !== "zero") expect().fail();
    }
});

test("named property additions that grow the storage", () => {
    function grow(object, value) {
        object.a = value;
        object.b = value;
        object.c = value;
        object.d = value;
        object.e = value;
        return object;
    }
    for (let i = 0; i < ITERATIONS; ++i) {
        const object = grow({}, i);
        if (i % 16 === 0) object[0] = i;
        if (object.a !== i || object.e !== i) expect().fail();
        if (i % 16 === 0 && object[0] !== i) expect().fail();
    }
});

test("indexed accesses in loops on objects of varying kinds", () => {
    function sum(object, count) {
        let result = 0;
        for (let i = 0; i < count; ++i) result += object[0] === undefined ? 1 : object[0];
        return result;
    }
    const packed = [2];
    const plain = { a: 1.5, b: 2.5 };
    const callable = function () {};
    for (let i = 0; i < 2000; ++i) sum(packed, 20);
    expect(sum(plain, 20)).toBe(20);
    expect(sum(callable, 20)).toBe(20);
    for (let i = 0; i < 2000; ++i) {
        sum(packed, 20);
        sum(plain, 20);
        sum(callable, 20);
    }
    expect(sum(packed, 20)).toBe(40);
    expect(plain.a).toBe(1.5);

    function store(object, count) {
        for (let i = 0; i < count; ++i) object[i % 2] = i;
    }
    for (let i = 0; i < 2000; ++i) store([0, 0], 20);
    const stored = { a: 1, b: 2 };
    store(stored, 20);
    expect(stored[1]).toBe(19);
    expectProperties(stored, 1, 2);
});

test("objects survive a garbage collection with either storage", () => {
    const objects = [];
    for (let i = 0; i < 1000; ++i) {
        const object = { a: { value: i }, b: `string${i}` };
        if (i % 2 === 0) object[0] = { value: i };
        objects.push(object);
    }
    gc();
    for (let i = 0; i < 1000; ++i) {
        expect(objects[i].a.value).toBe(i);
        expect(objects[i].b).toBe(`string${i}`);
        if (i % 2 === 0) expect(objects[i][0].value).toBe(i);
    }
});

test("strict arguments keep their properties and elements as they grow", () => {
    "use strict";

    function make() {
        return arguments;
    }

    const args = make(1, 2, 3);
    expect(args.length).toBe(3);
    expect(args[0]).toBe(1);
    expect(args[2]).toBe(3);
    expect(typeof args[Symbol.iterator]).toBe("function");
    expect([...args]).toEqual([1, 2, 3]);

    for (let i = 0; i < 40; ++i) args["named" + i] = i;
    args[10] = "sparse";
    expect(args.length).toBe(3);
    expect(args[2]).toBe(3);
    expect(args[10]).toBe("sparse");
    expect(args.named39).toBe(39);
    expect(typeof args[Symbol.iterator]).toBe("function");

    const empty = make();
    expect(empty.length).toBe(0);
    empty[0] = "first";
    empty.extra = "named";
    expect(empty[0]).toBe("first");
    expect(empty.extra).toBe("named");
    expect(empty.length).toBe(0);
});

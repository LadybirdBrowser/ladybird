// A miss on one object must not answer for an indexed property of the next object with the same shape, since indexed
// properties live outside the shape.
test("object pattern with a numeric key", () => {
    function second(o) {
        const { 1: y } = o;
        return y;
    }
    expect(second({ a: 1 })).toBeUndefined();
    expect(second({ a: 2, 1: "y" })).toBe("y");
});

test("object pattern with a string key that is an array index", () => {
    function second(o) {
        // prettier-ignore
        const { "1": y } = o;
        return y;
    }
    expect(second({ a: 1 })).toBeUndefined();
    expect(second({ a: 2, 1: "y" })).toBe("y");
});

test("destructuring assignment with a numeric key", () => {
    function second(o) {
        let y;
        ({ 1: y } = o);
        return y;
    }
    expect(second({ a: 1 })).toBeUndefined();
    expect(second({ a: 2, 1: "y" })).toBe("y");
});

test("a hit after a miss", () => {
    function second(o) {
        const { 1: y } = o;
        return y;
    }
    expect(second({ a: 2, 1: "y" })).toBe("y");
    expect(second({ a: 1 })).toBeUndefined();
    expect(second({ a: 2, 1: "y" })).toBe("y");
});

test("an indexed property added to the prototype after a miss", () => {
    function second(o) {
        const { 1: y } = o;
        return y;
    }
    const prototype = {};
    const object = Object.create(prototype);
    expect(second(object)).toBeUndefined();
    prototype[1] = "y";
    expect(second(object)).toBe("y");
});

test("function objects", () => {
    function second(o) {
        const { 1: y } = o;
        return y;
    }
    function f() {}
    function g() {}
    Object.setPrototypeOf(f, null);
    Object.setPrototypeOf(g, null);
    g[1] = "y";
    expect(second(f)).toBeUndefined();
    expect(second(g)).toBe("y");
});

test("setting an index through a deep prototype chain", () => {
    let object = {};
    for (let i = 0; i < 100_000; ++i) object = Object.create(object);
    object[1] = "y";
    expect(object[1]).toBe("y");
});

test("rest parameter with no arguments", () => {
    function foo(...a) {
        expect(a).toBeInstanceOf(Array);
        expect(a).toHaveLength(0);
    }
    foo();
});

test("rest parameter with arguments", () => {
    function foo(...a) {
        expect(a).toEqual(["foo", 123, undefined, { foo: "bar" }]);
    }
    foo("foo", 123, undefined, { foo: "bar" });
});

test("rest parameter after normal parameters with no arguments", () => {
    function foo(a, b, ...c) {
        expect(a).toBe("foo");
        expect(b).toBe(123);
        expect(c).toEqual([]);
    }
    foo("foo", 123);
});

test("rest parameter after normal parameters with arguments", () => {
    function foo(a, b, ...c) {
        expect(a).toBe("foo");
        expect(b).toBe(123);
        expect(c).toEqual([undefined, { foo: "bar" }]);
    }
    foo("foo", 123, undefined, { foo: "bar" });
});

test("basic arrow function rest parameters", () => {
    let foo = (...a) => {
        expect(a).toBeInstanceOf(Array);
        expect(a).toHaveLength(0);
    };
    foo();

    foo = (a, b, ...c) => {
        expect(a).toBe("foo");
        expect(b).toBe(123);
        expect(c).toEqual([undefined, { foo: "bar" }]);
    };
    foo("foo", 123, undefined, { foo: "bar" });
});

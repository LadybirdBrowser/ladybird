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

test("rest parameter as only parameter with arguments object access", () => {
    function foo(...args) {
        expect(args).toEqual([1, 2, 3]);
        expect(arguments.length).toBe(3);
        expect(arguments[0]).toBe(1);
    }
    foo(1, 2, 3);
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

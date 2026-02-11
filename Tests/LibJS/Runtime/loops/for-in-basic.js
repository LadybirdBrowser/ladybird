test("iterate through empty string", () => {
    const a = [];
    for (const property in "") {
        a.push(property);
    }
    expect(a).toEqual([]);
});

test("iterate through number", () => {
    const a = [];
    for (const property in 123) {
        a.push(property);
    }
    expect(a).toEqual([]);
});

test("iterate through empty object", () => {
    const a = [];
    for (const property in {}) {
        a.push(property);
    }
    expect(a).toEqual([]);
});

test("iterate through string", () => {
    const a = [];
    for (const property in "hello") {
        a.push(property);
    }
    expect(a).toEqual(["0", "1", "2", "3", "4"]);
});

test("iterate through object", () => {
    const a = [];
    for (const property in { a: 1, b: 2, c: 2 }) {
        a.push(property);
    }
    expect(a).toEqual(["a", "b", "c"]);
});

test("iterate through undefined", () => {
    for (const property in undefined) {
        expect.fail();
    }
});

test("use already-declared variable", () => {
    var property;
    for (property in "abc");
    expect(property).toBe("2");
});

test("allow binding patterns", () => {
    const expected = [
        ["1", "3", []],
        ["s", undefined, []],
        ["l", "n", ["g", "N", "a", "m", "e"]],
    ];
    let counter = 0;

    for (let [a, , b, ...c] in { 123: 1, sm: 2, longName: 3 }) {
        expect(a).toBe(expected[counter][0]);
        expect(b).toBe(expected[counter][1]);
        expect(c).toEqual(expected[counter][2]);
        counter++;
    }
    expect(counter).toBe(3);
});

describe("special left hand sides", () => {
    test("allow member expression as variable", () => {
        const f = {};
        for (f.a in "abc");
        expect(f.a).toBe("2");
    });

    test("allow member expression of function call", () => {
        const b = {};
        function f() {
            return b;
        }

        for (f().a in "abc");

        expect(f().a).toBe("2");
        expect(b.a).toBe("2");
    });

    test("call function is allowed in parsing but fails in runtime", () => {
        var fCalled = false;
        function f() {
            fCalled = true;
            return {};
        }

        // Does not fail since it does not iterate (no keys in [])
        expect("for (f() in []);").toEvalTo(undefined);

        // f() is evaluated as the LHS, then ReferenceError is thrown
        // because the result of a call is not a valid assignment target.
        expect(() => {
            eval("for (f() in { a: 1 }) {}");
        }).toThrowWithMessage(ReferenceError, "Invalid left-hand side in assignment");
        expect(fCalled).toBeTrue();
    });

    test("Cannot change constant declaration in body", () => {
        const vals = [];
        for (const v in [1, 2]) {
            expect(() => v++).toThrowWithMessage(TypeError, "Invalid assignment to const variable");
            vals.push(v);
        }

        expect(vals).toEqual(["0", "1"]);
    });
});

test("remove properties while iterating", () => {
    const from = [1, 2, 3];
    const to = [];
    for (const prop in from) {
        to.push(prop);
        from.pop();
    }
    expect(to).toEqual(["0", "1"]);
});

test("duplicated properties in prototype", () => {
    const object = { a: 1 };
    const proto = { a: 2 };
    Object.setPrototypeOf(object, proto);
    const a = [];
    for (const prop in object) {
        a.push(prop);
    }
    expect(a).toEqual(["a"]);
});

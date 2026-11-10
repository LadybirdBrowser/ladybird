test("nested computed object pattern preserves the assignment result", () => {
    let value;
    const key = "length";
    const source = { x: "abc" };
    const result = ({
        x: { [key]: value },
    } = source);
    expect(value).toBe(3);
    expect(result).toBe(source);
});

test("nested computed object pattern preserves the callee, receiver, and earlier arguments", () => {
    let value;
    const key = "length";
    const source = { x: "abc" };
    const receiver = {
        check(first, result, last) {
            expect(this).toBe(receiver);
            expect(first).toBe("before");
            expect(result).toBe(source);
            expect(last).toBe(3);
            return "called";
        },
    };
    expect(
        receiver.check(
            "before",
            ({
                x: { [key]: value },
            } = source),
            value
        )
    ).toBe("called");
});

test("computed object pattern in an array preserves the assignment result", () => {
    let value;
    const key = "length";
    const source = ["abc"];
    const result = ([{ [key]: value }] = source);
    expect(value).toBe(3);
    expect(result).toBe(source);
});

test("computed object pattern in an array rest element preserves the assignment result", () => {
    let first, length;
    const key = "length";
    const source = [1, 2, 3];
    const result = ([first, ...{ [key]: length }] = source);
    expect(first).toBe(1);
    expect(length).toBe(2);
    expect(result).toBe(source);
});

test("nested computed object pattern evaluates keys, defaults, and rest in order", () => {
    let value, rest;
    const events = [];
    const source = {
        get x() {
            events.push("outer getter");
            return {
                get a() {
                    events.push("value getter");
                    return undefined;
                },
                b: 2,
            };
        },
    };
    const key = {
        toString() {
            events.push("key conversion");
            return "a";
        },
    };
    const result = ({
        x: { [(events.push("key expression"), key)]: value = (events.push("default"), 7), ...rest },
    } = source);
    expect(result).toBe(source);
    expect(value).toBe(7);
    expect(rest).toEqual({ b: 2 });
    expect(events).toEqual(["outer getter", "key expression", "key conversion", "value getter", "default"]);
});

test("object literals in computed keys and nested defaults still create objects", () => {
    let value;
    const source = {};
    const result = ({ x: { [{ key: "a" }.key]: value } = { a: 9 } } = source);
    expect(result).toBe(source);
    expect(value).toBe(9);
});

test("computed object patterns in arrow parameters preserve their defaults", () => {
    const key = "length";
    const read = ({ x: { [key]: value } } = { x: "abc" }) => value;
    expect(read()).toBe(3);
    expect(read({ x: "hello" })).toBe(5);
});

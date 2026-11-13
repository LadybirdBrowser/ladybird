test("computed destructuring keys survive their targets and defaults", () => {
    const events = [];
    const key = name => {
        events.push(`key ${name}`);
        return {
            toString() {
                events.push(`convert ${name}`);
                return name;
            },
        };
    };
    const target = {};
    const destination = name => {
        events.push(`target ${name}`);
        return target;
    };
    const defaultValue = value => {
        events.push(`default ${value}`);
        return { value };
    };
    const source = {
        get a() {
            events.push("get a");
        },
        get b() {
            events.push("get b");
        },
    };
    const result = ({
        [key("a")]: destination("first")[key("first")] = defaultValue(1),
        [key("b")]: destination("second")[key("second")] = defaultValue(2),
    } = source);
    expect(result).toBe(source);
    expect(target).toEqual({ first: { value: 1 }, second: { value: 2 } });
    expect(events).toEqual([
        "key a",
        "convert a",
        "target first",
        "key first",
        "get a",
        "default 1",
        "convert first",
        "key b",
        "convert b",
        "target second",
        "key second",
        "get b",
        "default 2",
        "convert second",
    ]);
});

test("object rest keeps earlier computed keys across nested patterns and defaults", () => {
    const symbol = Symbol("key");
    const key = name => ({ toString: () => name });
    const source = { a: undefined, b: 2, [symbol]: 3, remaining: 4 };
    const {
        [key("a")]: { [key("value")]: first } = { value: 1 },
        [key("b")]: second,
        [symbol]: third,
        ...rest
    } = source;
    expect([first, second, third]).toEqual([1, 2, 3]);
    expect(rest).toEqual({ remaining: 4 });
});

test("anonymous functions take their names from the string keys of object literals", () => {
    function make() {
        return { "with space": () => 1, "with-dash": function () {}, "1x": class {} };
    }
    const object = make();
    expect(object["with space"].name).toBe("with space");
    expect(object["with-dash"].name).toBe("with-dash");
    expect(object["1x"].name).toBe("1x");
});

test("anonymous classes take their names from computed keys, in parentheses too", () => {
    function make(key) {
        // prettier-ignore
        return { [key]: class {}, [key + "2"]: (class {}), [key + "3"]: class { static name = 1; } };
    }
    const object = make("key");
    expect(object.key.name).toBe("key");
    expect(object.key2.name).toBe("key2");
    expect(object.key3.name).toBe(1);
});

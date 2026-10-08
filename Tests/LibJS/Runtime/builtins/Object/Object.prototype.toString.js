test("length", () => {
    expect(Object.prototype.toString).toHaveLength(0);
});

test("result for various object types", () => {
    const arrayProxy = new Proxy([], {});
    const customToStringTag = {
        [Symbol.toStringTag]: "Foo",
    };
    const arguments = (function () {
        return arguments;
    })();

    expect(Object.prototype.toString.call(undefined)).toBe("[object Undefined]");
    expect(Object.prototype.toString.call(null)).toBe("[object Null]");
    expect(Object.prototype.toString.call([])).toBe("[object Array]");
    expect(Object.prototype.toString.call(arguments)).toBe("[object Arguments]");
    expect(Object.prototype.toString.call(function () {})).toBe("[object Function]");
    expect(Object.prototype.toString.call(new Error())).toBe("[object Error]");
    expect(Object.prototype.toString.call(new TypeError())).toBe("[object Error]");
    expect(Object.prototype.toString.call(new AggregateError([]))).toBe("[object Error]");
    expect(Object.prototype.toString.call(new Boolean())).toBe("[object Boolean]");
    expect(Object.prototype.toString.call(new Number())).toBe("[object Number]");
    expect(Object.prototype.toString.call(new Date())).toBe("[object Date]");
    expect(Object.prototype.toString.call(new RegExp())).toBe("[object RegExp]");
    expect(Object.prototype.toString.call({})).toBe("[object Object]");
    expect(Object.prototype.toString.call(arrayProxy)).toBe("[object Array]");
    expect(Object.prototype.toString.call(customToStringTag)).toBe("[object Foo]");

    expect(globalThis.toString()).toBe("[object Object]");
});

test("tags found on the prototype chain after earlier calls", () => {
    const toString = Object.prototype.toString;
    const array = [1, 2];
    expect(toString.call(array)).toBe("[object Array]");
    expect(toString.call({})).toBe("[object Object]");

    Array.prototype[Symbol.toStringTag] = "Listed";
    try {
        expect(toString.call(array)).toBe("[object Listed]");
    } finally {
        delete Array.prototype[Symbol.toStringTag];
    }
    expect(toString.call(array)).toBe("[object Array]");

    class Tagged {
        get [Symbol.toStringTag]() {
            return "Tagged from a getter";
        }
    }
    expect(toString.call(new Tagged())).toBe("[object Tagged from a getter]");

    const longTag = { [Symbol.toStringTag]: "A tag that is longer than thirty-two characters" };
    expect(toString.call(longTag)).toBe("[object A tag that is longer than thirty-two characters]");
    expect(toString.call({ [Symbol.toStringTag]: "Ünïcode" })).toBe("[object Ünïcode]");
    expect(toString.call({ [Symbol.toStringTag]: 42 })).toBe("[object Object]");

    const proxy = new Proxy([], {
        get(target, key) {
            return key === Symbol.toStringTag ? "Proxied" : target[key];
        },
    });
    expect(toString.call(proxy)).toBe("[object Proxied]");
    expect(toString.call(function () {})).toBe("[object Function]");
    expect(`${[{}, {}]}`).toBe("[object Object],[object Object]");
});

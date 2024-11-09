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

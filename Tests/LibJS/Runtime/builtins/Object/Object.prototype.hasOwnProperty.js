test("basic functionality", () => {
    var o = {};

    o.foo = 1;
    expect(o.hasOwnProperty("foo")).toBeTrue();
    expect(o.hasOwnProperty("bar")).toBeFalse();
    expect(o.hasOwnProperty()).toBeFalse();
    expect(o.hasOwnProperty(undefined)).toBeFalse();

    o.undefined = 2;
    expect(o.hasOwnProperty()).toBeTrue();
    expect(o.hasOwnProperty(undefined)).toBeTrue();

    var testSymbol = Symbol("real");
    o[testSymbol] = 3;
    expect(o.hasOwnProperty(testSymbol)).toBeTrue();
    expect(o.hasOwnProperty(Symbol("fake"))).toBeFalse();
});

test("objects with exotic own properties", () => {
    expect([1, 2].hasOwnProperty("length")).toBeTrue();
    expect([1, 2].hasOwnProperty("1")).toBeTrue();
    expect(new String("ab").hasOwnProperty("1")).toBeTrue();
    expect(new Uint8Array(2).hasOwnProperty("1")).toBeTrue();
    expect(function () {}.hasOwnProperty("prototype")).toBeTrue();
    expect(
        (function () {
            return arguments.hasOwnProperty("length");
        })()
    ).toBeTrue();
    expect(
        new Proxy({}, { getOwnPropertyDescriptor: () => ({ value: 1, configurable: true }) }).hasOwnProperty("x")
    ).toBeTrue();
    const object = { a: 1 };
    delete object.a;
    expect(object.hasOwnProperty("a")).toBeFalse();
});

test("objects with their own [[GetOwnProperty]]", () => {
    const array = [1, , 3];
    expect(array.hasOwnProperty(0)).toBeTrue();
    expect(array.hasOwnProperty(1)).toBeFalse();
    expect(array.hasOwnProperty("length")).toBeTrue();

    const string = new String("ab");
    expect(string.hasOwnProperty(1)).toBeTrue();
    expect(string.hasOwnProperty(2)).toBeFalse();
    expect(string.hasOwnProperty("length")).toBeTrue();

    const proxy = new Proxy({}, { getOwnPropertyDescriptor: () => ({ value: 1, configurable: true }) });
    expect(proxy.hasOwnProperty("anything")).toBeTrue();

    function f(a) {
        return arguments;
    }
    expect(f(1).hasOwnProperty(0)).toBeTrue();
    expect(f(1).hasOwnProperty(1)).toBeFalse();
    expect(f.hasOwnProperty("prototype")).toBeTrue();
    expect(Math.hasOwnProperty("abs")).toBeTrue();
    expect(new Uint8Array(2).hasOwnProperty(1)).toBeTrue();
    expect(new Uint8Array(2).hasOwnProperty(2)).toBeFalse();
});

test("accessor and indexed properties of ordinary objects", () => {
    const o = { 3: "x" };
    Object.defineProperty(o, "getter", { get: () => 1 });
    expect(o.hasOwnProperty("getter")).toBeTrue();
    expect(o.hasOwnProperty(3)).toBeTrue();
    expect(o.hasOwnProperty("3")).toBeTrue();
    expect(o.hasOwnProperty(4)).toBeFalse();
    delete o[3];
    expect(o.hasOwnProperty(3)).toBeFalse();
});

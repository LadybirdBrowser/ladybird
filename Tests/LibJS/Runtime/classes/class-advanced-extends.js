test("extending function", () => {
    class A extends function () {
        this.foo = 10;
    } {}

    expect(new A().foo).toBe(10);
});

test("extending null", () => {
    class A extends null {}

    expect(Object.getPrototypeOf(A.prototype)).toBeNull();

    expect(() => {
        new A();
    }).toThrowWithMessage(TypeError, "Super constructor is not a constructor");
});

test("extending String", () => {
    class MyString extends String {}

    const ms = new MyString("abc");
    expect(ms).toBeInstanceOf(MyString);
    expect(ms).toBeInstanceOf(String);
    expect(ms.charAt(1)).toBe("b");

    class MyString2 extends MyString {
        charAt(i) {
            return `#${super.charAt(i)}`;
        }
    }

    const ms2 = new MyString2("abc");
    expect(ms2.charAt(1)).toBe("#b");
});

test("class extends value is invalid", () => {
    expect(() => {
        class A extends 123 {}
    }).toThrowWithMessage(TypeError, "Class extends value 123 is not a constructor or null");
});

test("class extends value has invalid prototype", () => {
    function f() {}
    f.prototype = 123;
    expect(() => {
        class A extends f {}
    }).toThrowWithMessage(TypeError, "Class extends value has an invalid prototype 123");
});

test("class heritage rejects unparenthesized arrows", () => {
    expect(() => eval("class A extends () => {} {}")).toThrow(SyntaxError);
    expect(() => eval("class A extends async () => {} {}")).toThrow(SyntaxError);
    expect(() => eval("const A = class extends () => {} {};")).toThrow(SyntaxError);
    expect(() => eval("const A = class extends async () => {} {};")).toThrow(SyntaxError);

    expect(() => eval("class A extends (() => {}) {}")).toThrow(TypeError);

    function mixin(callback) {
        expect(callback()).toBe(1);
        return class {};
    }
    class A extends mixin(() => 1) {}
    expect(new A()).toBeInstanceOf(A);
});

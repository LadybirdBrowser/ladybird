test("deleting object properties", () => {
    const o = {};
    o.x = 1;
    o.y = 2;
    o.z = 3;
    expect(Object.getOwnPropertyNames(o)).toHaveLength(3);

    expect(delete o.x).toBeTrue();
    expect(o.hasOwnProperty("x")).toBeFalse();
    expect(o.hasOwnProperty("y")).toBeTrue();
    expect(o.hasOwnProperty("z")).toBeTrue();
    expect(Object.getOwnPropertyNames(o)).toHaveLength(2);

    expect(delete o.y).toBeTrue();
    expect(o.hasOwnProperty("x")).toBeFalse();
    expect(o.hasOwnProperty("y")).toBeFalse();
    expect(o.hasOwnProperty("z")).toBeTrue();
    expect(Object.getOwnPropertyNames(o)).toHaveLength(1);

    expect(delete o.z).toBeTrue();
    expect(o.hasOwnProperty("x")).toBeFalse();
    expect(o.hasOwnProperty("y")).toBeFalse();
    expect(o.hasOwnProperty("z")).toBeFalse();
    expect(Object.getOwnPropertyNames(o)).toHaveLength(0);
});

test("deleting array indices", () => {
    const a = [3, 5, 7];

    expect(Object.getOwnPropertyNames(a)).toHaveLength(4);

    expect(delete a[0]).toBeTrue();
    expect(a.hasOwnProperty(0)).toBeFalse();
    expect(a.hasOwnProperty(1)).toBeTrue();
    expect(a.hasOwnProperty(2)).toBeTrue();
    expect(Object.getOwnPropertyNames(a)).toHaveLength(3);

    expect(delete a[1]).toBeTrue();
    expect(a.hasOwnProperty(0)).toBeFalse();
    expect(a.hasOwnProperty(1)).toBeFalse();
    expect(a.hasOwnProperty(2)).toBeTrue();
    expect(Object.getOwnPropertyNames(a)).toHaveLength(2);

    expect(delete a[2]).toBeTrue();
    expect(a.hasOwnProperty(0)).toBeFalse();
    expect(a.hasOwnProperty(1)).toBeFalse();
    expect(a.hasOwnProperty(2)).toBeFalse();
    expect(Object.getOwnPropertyNames(a)).toHaveLength(1);

    expect(delete a["42"]).toBeTrue();
    expect(Object.getOwnPropertyNames(a)).toHaveLength(1);
});

test("deleting non-configurable property", () => {
    const q = {};
    Object.defineProperty(q, "foo", { value: 1, writable: false, enumerable: false });
    expect(q.foo).toBe(1);

    expect(delete q.foo).toBeFalse();
    expect(q.hasOwnProperty("foo")).toBeTrue();
});

test("deleting non-configurable property throws in strict mode", () => {
    "use strict";
    const q = {};
    Object.defineProperty(q, "foo", { value: 1, writable: false, enumerable: false });
    expect(q.foo).toBe(1);

    expect(() => {
        delete q.foo;
    }).toThrowWithMessage(TypeError, "Cannot delete property 'foo' of [object Object]");
    expect(q.hasOwnProperty("foo")).toBeTrue();
});

test("deleting super property", () => {
    class A {
        foo() {}
    }

    class B extends A {
        bar() {
            delete super.foo;
        }

        baz() {
            delete super["foo"];
        }
    }

    class C {
        static foo() {
            delete super.bar;
        }
    }

    class D {
        static foo() {
            const deleter = () => delete super.foo;
            deleter();
        }
    }

    const obj = new B();
    expect(() => {
        obj.bar();
    }).toThrowWithMessage(ReferenceError, "Can't delete a property on 'super'");

    expect(() => {
        obj.baz();
    }).toThrowWithMessage(ReferenceError, "Can't delete a property on 'super'");

    Object.setPrototypeOf(C, null);
    expect(() => {
        C.foo();
    }).toThrowWithMessage(ReferenceError, "Can't delete a property on 'super'");

    expect(() => {
        D.foo();
    }).toThrowWithMessage(ReferenceError, "Can't delete a property on 'super'");
});

test("deleting an object computed property coerces the object to a property key", () => {
    let called = false;
    const obj = { prop1: 1, 2: 2 };

    function createToPrimitiveFunction(object, valueToReturn) {
        return function (hint) {
            called = true;
            console.log(this, object);
            expect(this).toBe(object);
            expect(hint).toBe("string");
            return valueToReturn;
        };
    }

    const a = {
        [Symbol.toPrimitive]: function (hint) {
            called = true;
            expect(this).toBe(a);
            expect(hint).toBe("string");
            return "prop1";
        },
    };

    const b = {
        [Symbol.toPrimitive]: function (hint) {
            called = true;
            expect(this).toBe(b);
            expect(hint).toBe("string");
            return 2;
        },
    };

    const c = {
        [Symbol.toPrimitive]: function (hint) {
            called = true;
            expect(this).toBe(c);
            expect(hint).toBe("string");
            return {};
        },
    };

    expect(Object.hasOwn(obj, "prop1")).toBeTrue();
    expect(Object.hasOwn(obj, 2)).toBeTrue();

    expect(delete obj[a]).toBeTrue();
    expect(called).toBeTrue();
    expect(Object.hasOwn(obj, "prop1")).toBeFalse();
    expect(Object.hasOwn(obj, 2)).toBeTrue();
    expect(obj.prop1).toBeUndefined();
    expect(obj[2]).toBe(2);

    called = false;
    expect(delete obj[b]).toBeTrue();
    expect(called).toBeTrue();
    expect(Object.hasOwn(obj, "prop1")).toBeFalse();
    expect(Object.hasOwn(obj, 2)).toBeFalse();
    expect(obj.prop1).toBeUndefined();
    expect(obj[2]).toBeUndefined();

    called = false;
    expect(() => {
        delete obj[c];
    }).toThrowWithMessage(
        TypeError,
        `Can't convert [object Object] to primitive with hint "string", its @@toPrimitive method returned an object`
    );
    expect(called).toBeTrue();
});

test("deleting a symbol returned by @@toPrimitive", () => {
    let called = false;
    const obj = { [Symbol.toStringTag]: "hello world" };

    const a = {
        [Symbol.toPrimitive]: function (hint) {
            called = true;
            expect(this).toBe(a);
            expect(hint).toBe("string");
            return Symbol.toStringTag;
        },
    };

    expect(Object.hasOwn(obj, Symbol.toStringTag)).toBeTrue();
    expect(delete obj[a]).toBeTrue();
    expect(called).toBeTrue();
    expect(Object.hasOwn(obj, Symbol.toStringTag)).toBeFalse();
    expect(obj[Symbol.toStringTag]).toBeUndefined();
});

test("delete always evaluates the lhs", () => {
    const obj = { prop: 1 };
    let called = false;
    function a() {
        called = true;
        return obj;
    }
    expect(delete a()).toBeTrue();
    expect(called).toBeTrue();
    expect(obj).toBeDefined();
    expect(Object.hasOwn(obj, "prop")).toBeTrue();
    expect(obj.prop).toBe(1);

    called = false;
    expect(delete a().prop).toBeTrue();
    expect(called).toBeTrue();
    expect(obj).toBeDefined();
    expect(Object.hasOwn(obj, "prop")).toBeFalse();
    expect(obj.prop).toBeUndefined();

    let b = 1;
    expect(delete ++b).toBeTrue();
    expect(b).toBe(2);

    expect(delete b++).toBeTrue();
    expect(b).toBe(3);

    let c = { d: 1 };
    expect(delete (b = c)).toBeTrue();
    expect(b).toBeDefined();
    expect(c).toBeDefined();
    expect(b).toBe(c);

    function d() {
        throw new Error("called");
    }

    expect(() => {
        delete d();
    }).toThrowWithMessage(Error, "called");

    expect(() => {
        delete d().stack;
    }).toThrowWithMessage(Error, "called");

    expect(() => {
        delete ~d();
    }).toThrowWithMessage(Error, "called");

    expect(() => {
        delete new d();
    }).toThrowWithMessage(Error, "called");
});

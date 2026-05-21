test("object literal concise methods are not constructors", () => {
    const object_method = { m() {} }.m;
    expect(() => {
        new object_method();
    }).toThrow(TypeError);
});

test("class methods are not constructors", () => {
    class C {
        m() {}
        static s() {}
    }

    expect(() => {
        new C.prototype.m();
    }).toThrow(TypeError);
    expect(() => {
        new C.s();
    }).toThrow(TypeError);
});

test("normal methods do not have a prototype property", () => {
    const object = {
        method() {},
        get getter() {},
        set setter(value) {},
    };

    class C {
        method() {}
        static staticMethod() {}
        get getter() {}
        set setter(value) {}
        static get staticGetter() {}
        static set staticSetter(value) {}
    }

    expect(object.method.hasOwnProperty("prototype")).toBeFalse();
    expect(Object.getOwnPropertyDescriptor(object, "getter").get.hasOwnProperty("prototype")).toBeFalse();
    expect(Object.getOwnPropertyDescriptor(object, "setter").set.hasOwnProperty("prototype")).toBeFalse();
    expect(C.prototype.method.hasOwnProperty("prototype")).toBeFalse();
    expect(C.staticMethod.hasOwnProperty("prototype")).toBeFalse();
    expect(Object.getOwnPropertyDescriptor(C.prototype, "getter").get.hasOwnProperty("prototype")).toBeFalse();
    expect(Object.getOwnPropertyDescriptor(C.prototype, "setter").set.hasOwnProperty("prototype")).toBeFalse();
    expect(Object.getOwnPropertyDescriptor(C, "staticGetter").get.hasOwnProperty("prototype")).toBeFalse();
    expect(Object.getOwnPropertyDescriptor(C, "staticSetter").set.hasOwnProperty("prototype")).toBeFalse();
});

test("generator methods keep their prototype property", () => {
    const object = {
        *method() {},
    };

    class C {
        *method() {}
        static *staticMethod() {}
    }

    expect(object.method.hasOwnProperty("prototype")).toBeTrue();
    expect(C.prototype.method.hasOwnProperty("prototype")).toBeTrue();
    expect(C.staticMethod.hasOwnProperty("prototype")).toBeTrue();
});

test("non-method functions remain constructible", () => {
    const plain_function = function () {};
    const property_function = { m: function () {} }.m;

    expect(new plain_function()).toBeInstanceOf(plain_function);
    expect(new property_function()).toBeInstanceOf(property_function);
});

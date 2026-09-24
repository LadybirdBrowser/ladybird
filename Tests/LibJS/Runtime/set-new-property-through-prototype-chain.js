describe("assigning a property the receiver does not have", () => {
    test("creates an own data property", () => {
        const o = {};
        o.foo = 1;
        o[3] = 2;
        expect(Object.getOwnPropertyDescriptor(o, "foo")).toEqual({
            value: 1,
            writable: true,
            enumerable: true,
            configurable: true,
        });
        expect(o[3]).toBe(2);
        expect(Object.keys(o)).toEqual(["3", "foo"]);
    });

    test("calls an inherited setter with the receiver", () => {
        let receiver;
        const p = {
            set foo(value) {
                receiver = this;
                this._foo = value;
            },
        };
        const o = Object.create(Object.create(p));
        o.foo = 1;
        expect(receiver).toBe(o);
        expect(o._foo).toBe(1);
        expect(Object.hasOwn(o, "foo")).toBeFalse();
    });

    test("respects an inherited read-only property", () => {
        const p = Object.defineProperty({}, "foo", { value: 1, writable: false });
        const o = Object.create(p);
        expect(() => {
            "use strict";
            o.foo = 2;
        }).toThrow(TypeError);
        o.foo = 2;
        expect(o.foo).toBe(1);
        expect(Object.hasOwn(o, "foo")).toBeFalse();
    });

    test("respects an inherited accessor without a setter", () => {
        const p = {
            get foo() {
                return 1;
            },
        };
        const o = Object.create(p);
        expect(Reflect.set(o, "foo", 2)).toBeFalse();
        expect(Object.hasOwn(o, "foo")).toBeFalse();
    });

    test("does not add to a non-extensible receiver", () => {
        const o = Object.preventExtensions(Object.create({ bar: 1 }));
        expect(Reflect.set(o, "foo", 1)).toBeFalse();
        expect(Object.hasOwn(o, "foo")).toBeFalse();
    });

    test("goes through a proxy prototype's set trap", () => {
        let trapReceiver;
        const proxy = new Proxy(
            {},
            {
                set(target, key, value, receiver) {
                    trapReceiver = receiver;
                    return Reflect.set(target, key, value, receiver);
                },
            }
        );
        const o = Object.create(Object.create(proxy));
        o.foo = 1;
        expect(trapReceiver).toBe(o);
        expect(Object.hasOwn(o, "foo")).toBeTrue();
        expect(Object.hasOwn(proxy, "foo")).toBeFalse();
    });

    test("forwards through a proxy prototype without a set trap", () => {
        const proxy = new Proxy(
            {
                set bar(value) {
                    this._bar = value;
                },
            },
            {}
        );
        const o = Object.create(proxy);
        o.foo = 1;
        o.bar = 2;
        expect(Object.hasOwn(o, "foo")).toBeTrue();
        expect(o._bar).toBe(2);
    });

    test("goes through a typed array prototype's [[Set]]", () => {
        const o = Object.create(new Uint8Array(2));
        o[5] = 1;
        expect(Object.hasOwn(o, "5")).toBeFalse();
        o[1] = 1;
        expect(Object.hasOwn(o, "1")).toBeTrue();
        expect(Object.getPrototypeOf(o)[1]).toBe(0);
    });

    test("respects a string object prototype's read-only indices", () => {
        const o = Object.create(new String("ab"));
        expect(Reflect.set(o, "1", "x")).toBeFalse();
        expect(Reflect.set(o, "2", "x")).toBeTrue();
        expect(Object.hasOwn(o, "2")).toBeTrue();
    });

    test("respects a function prototype's lazily created properties", () => {
        function f() {}
        const o = Object.create(f);
        o.prototype = 1;
        expect(Object.hasOwn(o, "prototype")).toBeTrue();
        expect(f.prototype).not.toBe(1);
    });

    test("adds to an arguments object", () => {
        function f(a) {
            arguments.foo = 1;
            arguments[3] = 2;
            return arguments;
        }
        const args = f(0);
        expect(args.foo).toBe(1);
        expect(args[3]).toBe(2);
        expect(args.length).toBe(1);
    });

    test("Object.assign calls inherited setters on the target", () => {
        let setterValue;
        const target = Object.create({
            set foo(value) {
                setterValue = value;
            },
        });
        Object.assign(target, { foo: 1, bar: 2 });
        expect(setterValue).toBe(1);
        expect(Object.hasOwn(target, "foo")).toBeFalse();
        expect(target.bar).toBe(2);
    });

    test("Object.assign throws on an inherited read-only property", () => {
        const target = Object.create(Object.defineProperty({}, "foo", { value: 1, writable: false }));
        expect(() => Object.assign(target, { foo: 2 })).toThrow(TypeError);
    });
});

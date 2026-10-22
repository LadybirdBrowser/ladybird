// https://tc39.es/ecma262/#sec-putvalue
// https://tc39.es/ecma262/#sec-ordinarysetwithowndescriptor
describe("setting a super property with a non-object this value", () => {
    test("throws a TypeError for a data property", () => {
        class A {
            setNamed() {
                super.x = 1;
            }

            setComputed(key) {
                super[key] = 1;
            }
        }

        for (const thisValue of [null, undefined, 1, "foo", Symbol(), 1n]) {
            expect(() => A.prototype.setNamed.call(thisValue)).toThrowWithMessage(
                TypeError,
                "Cannot set property 'x' of"
            );
            expect(() => A.prototype.setComputed.call(thisValue, "y")).toThrowWithMessage(
                TypeError,
                "Cannot set property 'y' of"
            );
        }
    });

    test("throws a TypeError after the caches have seen an object this value", () => {
        class A {
            set() {
                super.x = 1;
            }
        }

        for (let i = 0; i < 10; ++i) {
            const object = new A();
            A.prototype.set.call(object);
            expect(object.x).toBe(1);
        }

        expect(() => A.prototype.set.call(null)).toThrowWithMessage(TypeError, "Cannot set property 'x' of");
        expect(Object.hasOwn(A.prototype, "x")).toBeFalse();
    });

    test("calls a setter with the non-object this value", () => {
        const receivers = [];

        class A {
            set x(value) {
                receivers.push(this);
            }
        }

        class B extends A {
            set() {
                super.x = 1;
            }
        }

        for (let i = 0; i < 10; ++i) {
            B.prototype.set.call(new B());
        }
        receivers.length = 0;

        B.prototype.set.call(null);
        B.prototype.set.call(undefined);
        B.prototype.set.call(42);

        expect(receivers).toEqual([null, undefined, 42]);
    });
});

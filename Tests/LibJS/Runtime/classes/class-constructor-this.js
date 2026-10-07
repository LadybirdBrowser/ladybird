// Base class constructors bind `this` like other functions, while derived
// ones bind it when super() returns.

test("this in base class constructors", () => {
    class Base {
        constructor(value) {
            this.value = value;
            this.self = this;
        }
    }
    const object = new Base(1);
    expect(object.value).toBe(1);
    expect(object.self).toBe(object);
    expect(object).toBeInstanceOf(Base);
});

test("arrow functions and eval in base class constructors see its this", () => {
    class Arrow {
        constructor() {
            this.getThis = () => this;
        }
    }
    class Eval {
        constructor() {
            this.evaluated = eval("this");
        }
    }
    const arrow = new Arrow();
    expect(arrow.getThis()).toBe(arrow);
    const evaluated = new Eval();
    expect(evaluated.evaluated).toBe(evaluated);
});

test("new.target and super properties in base class constructors", () => {
    class Base {
        constructor() {
            this.target = new.target;
            this.description = super.toString.call(this);
        }
    }
    class Derived extends Base {}
    expect(new Base().target).toBe(Base);
    expect(new Derived().target).toBe(Derived);
    expect(new Base().description).toBe("[object Object]");
});

test("this in derived class constructors", () => {
    class Base {
        constructor() {
            this.base = true;
        }
    }
    class Derived extends Base {
        constructor() {
            expect(() => this).toThrowWithMessage(ReferenceError, "|this| has not been initialized");
            super();
            this.derived = true;
        }
    }
    const object = new Derived();
    expect(object.base).toBeTrue();
    expect(object.derived).toBeTrue();
});

test("base class constructors returning objects", () => {
    class Replaced {
        constructor(replacement) {
            this.ignored = true;
            if (replacement) return replacement;
        }
    }
    const replacement = { replaced: true };
    expect(new Replaced(replacement)).toBe(replacement);
    expect(new Replaced(null).ignored).toBeTrue();
});

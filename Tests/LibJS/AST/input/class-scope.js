// Class name is visible inside the class body but not outside (for class expressions).
var C = class MyClass {
    method() {
        return MyClass;
    }
};

// Class declaration name IS visible in the enclosing scope.
function uses_class() {
    class Foo {
        method() {
            return new Foo();
        }
    }
    return new Foo();
}

// Computed property keys are evaluated in the class scope.
function computed_key() {
    let key = "hello";
    class Bar {
        [key]() {
            return 1;
        }
    }
    return new Bar();
}

// Static methods and fields.
function static_members() {
    class Baz {
        static x = 1;
        static method() {
            return Baz.x;
        }
    }
    return Baz.method();
}

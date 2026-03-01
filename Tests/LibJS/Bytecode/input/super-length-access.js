// Test that super.length uses the GetLengthWithThis optimization.

class A {
    get length() { return 42; }
}

class B extends A {
    m() {
        return super.length;
    }
}

new B().m();

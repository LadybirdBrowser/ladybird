class A {
    foo() {
        delete super.bar;
    }
    baz() {
        delete super["bar"];
    }
}
try {
    new A().foo();
} catch (e) {}
try {
    new A().baz();
} catch (e) {}

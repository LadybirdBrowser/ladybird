class Base {
    method() {}
}
class Foo extends Base {
    method() {
        return super.method?.();
    }
}
new Foo().method();

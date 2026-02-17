class Base {
    get name() {
        return "base";
    }
}
class Derived extends Base {
    method(x = super.name) {
        return x;
    }
}

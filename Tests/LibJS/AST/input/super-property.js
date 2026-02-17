class Base {
    getValue() {
        return 42;
    }
}

class Derived extends Base {
    getValue() {
        return super.getValue();
    }
    getComputed(key) {
        return super[key];
    }
}

let obj = {
    foo() {
        return super.toString();
    },
};

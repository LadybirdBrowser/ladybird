class Base {}
class Derived extends Base {
    constructor() {
        super();
    }
    test() {
        return super[0];
    }
}
new Derived().test();

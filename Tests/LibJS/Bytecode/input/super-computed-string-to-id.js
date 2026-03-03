class C extends Object {
    constructor() {
        super();
        super["foo"] = 1;
    }
}
new C();

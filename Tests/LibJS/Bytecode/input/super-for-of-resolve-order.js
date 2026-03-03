class C {
    constructor() {
        for (super["prop"] of [1, 2]) {}
    }
}
new C();

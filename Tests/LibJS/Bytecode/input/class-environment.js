function classWithName() {
    let C = class Foo {
        method() {
            return Foo;
        }
    };
    return new C().method() === C;
}
console.log(classWithName());

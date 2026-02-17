class A extends Object {
    read() {
        return super["x"];
    }
    write() {
        super["x"] = 42;
    }
    update() {
        super.x++;
    }
}
new A().read();
new A().write();
new A().update();

class C {
    #x = 0;
    f() { this.#x &&= 1; }
}
new C().f();

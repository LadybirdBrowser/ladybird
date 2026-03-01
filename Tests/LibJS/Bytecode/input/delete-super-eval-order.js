// Test that delete super[key] evaluates the key expression
// before calling ResolveSuperBase, per spec.

class A {
    m(key) {
        delete super[key];
    }
}

try { new A().m("x"); } catch {}

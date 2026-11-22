[9, "x"].forEach(function(key) {
  test(function() {
    var element = document.createElement("div");
    var dataset = element.dataset;

    var value = "value for " + this.name;

    assert_equals(dataset[key], undefined);

    element.setAttribute("data-" + key, value);
    assert_equals(element.getAttribute("data-" + key), value);
    assert_equals(dataset[key], value);

    var propdesc = Object.getOwnPropertyDescriptor(dataset, key);
    assert_not_equals(propdesc, undefined);
    assert_equals(propdesc.value, value);
    assert_true(propdesc.writable);
    assert_true(propdesc.enumerable);
    assert_true(propdesc.configurable);
  }, "Getting property descriptor for key " + key);

  test(function() {
    var element = document.createElement("div");
    var dataset = element.dataset;

    var proto = "proto getter for " + this.name;
    var calledSetter = [];
    Object.defineProperty(DOMStringMap.prototype, key, {
      "get": function() { return proto; },
      "set": this.unreached_func("Should not call [[Set]] on prototype"),
      "configurable": true,
    });
    this.add_cleanup(function() {
      delete DOMStringMap.prototype[key];
    });

    var value = "value for " + this.name;

    assert_equals(dataset[key], proto);
    assert_equals(element.getAttribute("data-" + key), null);
    assert_equals(dataset[key] = value, value);
    assert_equals(dataset[key], value);
    assert_equals(element.getAttribute("data-" + key), value);
  }, "Setting property for key " + key + " with accessor property on prototype");
});

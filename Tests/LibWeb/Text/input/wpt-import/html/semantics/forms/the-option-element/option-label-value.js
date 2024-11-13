function test_option(member) {
  test(function() {
    var option = document.createElement("option");
    assert_equals(option[member], "");
  }, "No children, no " + member);

  test(function() {
    var option = document.createElement("option");
    option.setAttribute(member, "")
    assert_equals(option[member], "");
  }, "No children, empty " + member);

  test(function() {
    var option = document.createElement("option");
    option.setAttribute(member, member)
    assert_equals(option[member], member);
  }, "No children, " + member);

  test(function() {
    var option = document.createElement("option");
    option.setAttributeNS("http://www.example.com/", member, member)
    assert_equals(option[member], "");
  }, "No children, namespaced " + member);

  test(function() {
    var option = document.createElement("option");
    option.appendChild(document.createTextNode(" child "));
    assert_equals(option[member], "child");
  }, "Single child, no " + member);

  test(function() {
    var option = document.createElement("option");
    option.appendChild(document.createTextNode(" child "));
    option.setAttribute(member, "")
    assert_equals(option[member], "");
  }, "Single child, empty " + member);

  test(function() {
    var option = document.createElement("option");
    option.appendChild(document.createTextNode(" child "));
    option.setAttribute(member, member)
    assert_equals(option[member], member);
  }, "Single child, " + member);

  test(function() {
    var option = document.createElement("option");
    option.appendChild(document.createTextNode(" child "));
    option.setAttributeNS("http://www.example.com/", member, member)
    assert_equals(option[member], "child");
  }, "Single child, namespaced " + member);

  test(function() {
    var option = document.createElement("option");
    option.appendChild(document.createTextNode(" child "));
    option.appendChild(document.createTextNode(" node "));
    assert_equals(option[member], "child node");
  }, "Two children, no " + member);

  test(function() {
    var option = document.createElement("option");
    option.appendChild(document.createTextNode(" child "));
    option.appendChild(document.createTextNode(" node "));
    option.setAttribute(member, "")
    assert_equals(option[member], "");
  }, "Two children, empty " + member);

  test(function() {
    var option = document.createElement("option");
    option.appendChild(document.createTextNode(" child "));
    option.appendChild(document.createTextNode(" node "));
    option.setAttribute(member, member)
    assert_equals(option[member], member);
  }, "Two children, " + member);

  test(function() {
    var option = document.createElement("option");
    option.appendChild(document.createTextNode(" child "));
    option.appendChild(document.createTextNode(" node "));
    option.setAttributeNS("http://www.example.com/", member, member)
    assert_equals(option[member], "child node");
  }, "Two children, namespaced " + member);
}

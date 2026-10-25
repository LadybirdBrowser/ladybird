setup({ allow_uncaught_exception: true });

test(function() {
  var events = [];
  window.onerror = function() {
    events.push("error");
  };

  var div = document.createElement("div");
  div.addEventListener("click", function (e) { events.push("click 1") });
  div.setAttribute("onclick", "}");
  div.addEventListener("click", function (e) { events.push("click 2") });
  div.dispatchEvent(new Event("click"));
  assert_equals(div.onclick, null);
  assert_array_equals(events, ["click 1", "error", "click 2"]);
}, "Invalid uncompiled raw handlers should only be compiled when about to call them");

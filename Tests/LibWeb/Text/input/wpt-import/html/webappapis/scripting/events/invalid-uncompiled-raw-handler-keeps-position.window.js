setup({ allow_uncaught_exception: true });

test(function() {
  var events = [];
  window.onerror = function() {
    events.push("error");
  };

  var div = document.createElement("div");
  div.addEventListener("click", function (e) { events.push("click 1"); });
  div.setAttribute("onclick", "}");
  div.addEventListener("click", function (e) { events.push("click 3"); });
  assert_equals(div.onclick, null);
  assert_array_equals(events, ["error"]);

  events = [];
  div.onclick = function (e) { events.push("click 2"); };
  div.dispatchEvent(new Event("click"));
  assert_array_equals(events, ["click 1", "click 2", "click 3"]);
}, "Compiling invalid uncompiled raw handlers should keep the position in event listener list");

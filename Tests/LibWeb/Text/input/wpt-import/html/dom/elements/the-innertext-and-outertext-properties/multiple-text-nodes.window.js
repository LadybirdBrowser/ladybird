async_test(t => {
  const div = document.body.appendChild(document.createElement("div"));
  t.add_cleanup(() => div.remove());
  const t1 = div.appendChild(new Text(""));
  div.appendChild(new Text(""));
  const t2 = div.appendChild(new Text(""));
  const t3 = div.appendChild(new Text(""));
  t.step_timeout(() => {
    t1.data = "X";
    t2.data = " ";
    t3.data = "Y";
    assert_equals(div.innerText, "X Y", "innerText");
    assert_equals(div.outerText, "X Y", "outerText");
    t.done();
  }, 100);
}, "Ensure multiple text nodes get rendered properly");

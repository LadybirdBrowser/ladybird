test(() => {
  const select = document.createElement("select");
  select.innerHTML = "<optgroup><option>1</optgroup><optgroup><option>2";
  assert_equals(select.value, "1");
  select.querySelector("optgroup").remove();
  assert_equals(select.value, "2");
}, "<select> needs to be updated when <optgroup> is removed");

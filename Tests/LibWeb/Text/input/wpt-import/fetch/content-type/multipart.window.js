// META: title=Ensure capital letters can be used in the boundary value.
setup({ single_test: true });
(async () => {
  const form_string =
    "--Boundary_with_capital_letters\r\n" +
    "Content-Type: application/json\r\n" +
    'Content-Disposition: form-data; name="does_this_work"\r\n' +
    "\r\n" +
    'YES\r\n' +
    "--Boundary_with_capital_letters--\r\n";

  const r = new Response(new Blob([form_string]), {
    headers: [
      [
        "Content-Type",
        "multipart/form-data; boundary=Boundary_with_capital_letters",
      ],
    ],
  });

  var s = "";
  try {
    const fd = await r.formData();
    for (const [key, value] of fd.entries()) {
      s += (`${key} = ${value}`);
    }
  } catch (ex) {
    s = ex;
  }

  assert_equals(s, "does_this_work = YES");
  done();
})();

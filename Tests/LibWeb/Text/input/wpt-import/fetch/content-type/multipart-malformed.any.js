// This is a repro for Chromium issue https://crbug.com/1412007.
promise_test(t => {
  const form_string =
    "--Boundary_with_capital_letters\r\n" +
    "Content-Type: application/json\r\n" +
    'Content-Disposition: form-data; name="does_this_work"\r\n' +
    "\r\n" +
    'YES\r\n' +
    "--Boundary_with_capital_letters-Random junk";

  const r = new Response(new Blob([form_string]), {
    headers: [
      [
        "Content-Type",
        "multipart/form-data; boundary=Boundary_with_capital_letters",
      ],
    ],
  });

  return promise_rejects_js(t, TypeError, r.formData(),
                            "form data should fail to parse");
}, "Invalid form data should not crash the browser");

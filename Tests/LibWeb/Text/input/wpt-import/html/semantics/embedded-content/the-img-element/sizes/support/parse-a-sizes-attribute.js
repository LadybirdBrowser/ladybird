setup({explicit_done:true});

function check(p, iframe) {
  var current = p.firstElementChild;
  var ref_sizes = current.getAttribute('sizes');
  var expect = current.currentSrc;
  if (expect) {
    expect = expect.split('?')[0];
  }
  while (current = current.nextElementSibling) {
    test(function() {
      if (expect === '' || expect === null || expect === undefined) {
        assert_unreached('ref currentSrc was ' + format_value(expect));
      }
      var got = current.currentSrc;
      assert_greater_than(got.indexOf('?'), -1, 'expected a "?" in currentSrc');
      got = got.split('?')[0];
      assert_equals(got, expect);
    }, current.outerHTML + ' ref sizes=' + format_value(ref_sizes) + ' (' + iframe.getAttribute('data-desc') + ')');
  }
}

function run_test() {
  var iframe = document.querySelector('iframe');
  [].forEach.call(iframe.contentDocument.querySelectorAll('p'), function(p) {
    check(p, iframe);
  });
  done();
}

onload = function() {
  var iframe = document.querySelector('iframe');
  iframe.onload = run_test;
  iframe.srcdoc = make_sizes_iframed_srcdoc(iframe.dataset.doctype, iframe.dataset.style);
}

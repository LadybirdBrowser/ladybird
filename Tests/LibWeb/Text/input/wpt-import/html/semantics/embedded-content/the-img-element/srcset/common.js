setup({explicit_done:true});

function check(img) {
  var name = format_value(img.getAttribute('srcset'));
  if (img.hasAttribute('sizes')) {
    name += ' sizes=' + format_value(img.getAttribute('sizes'));
  }
  if (img.hasAttribute('data-desc')) {
    name += ' (' + img.getAttribute('data-desc') + ')';
  }
  test(function() {
    var expect = img.dataset.expect;
    if ('resolve' in img.dataset) {
      var a = document.createElement('a');
      a.href = expect;
      expect = a.href;
    }
    assert_equals(img.currentSrc, expect);
  }, name);
}

onload = function() {
  [].forEach.call(document.images, check);
  done();
};

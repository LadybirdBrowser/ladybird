setup({ explicit_done: true });

function t(desc, func, expect) {
  async_test(function() {
    let img = document.querySelector('[data-desc="' + desc + '"]');
    let oldComplete = img.complete;
    img.onload = img.onerror = this.unreached_func('update the image data was run');
    if (expect == 'timeout') {
      setTimeout(this.step_func_done(), 1000);
    } else {
      img['on' + expect] = this.step_func_done();
      setTimeout(this.unreached_func('update the image data didn\'t run'), 1000);
    }
    func.call(this, img);
    if (expect == 'timeout') {
      assert_equals(img.complete, oldComplete, `complete shouldn't have changed`);
    }
  }, desc);
}

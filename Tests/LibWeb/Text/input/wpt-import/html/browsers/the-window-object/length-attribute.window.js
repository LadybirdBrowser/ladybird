async_test(t => {
  const frame = document.createElement("iframe");
  frame.srcdoc = "<iframe name=x srcdoc='<iframe name=z></iframe>'></iframe><iframe name=y></iframe>";
  frame.onload = t.step_func_done(() => {
    const frameW = frame.contentWindow;
    assert_equals(frameW.length, 2);
    assert_not_equals(frameW.x, undefined);
    assert_not_equals(frameW.y, undefined);
    assert_equals(frameW.z, undefined);
    assert_equals(frameW.x, frameW[0]);
    assert_equals(frameW.y, frameW[1]);
    const xFrameW = frameW.x;
    assert_equals(xFrameW.length, 1);
    assert_not_equals(xFrameW.z, undefined);
    assert_equals(xFrameW.z, xFrameW[0]);
    frame.remove();
    assert_equals(frameW.length, 0);
    assert_equals(frameW.x, undefined);
    assert_equals(frameW[0], undefined);
    assert_equals(xFrameW.length, 0);
    assert_equals(xFrameW.z, undefined);
  });
  document.body.append(frame);
}, "Window object's length IDL attribute (and named access)");

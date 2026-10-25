function booleanTests(feature) {
  const windowURL = 'resources/close-self.html';
  // Tests for how windows features are tokenized into 'name', 'value'
  // window features separators are ASCII whitespace, '=' and  ','

  const featureUpper = feature.toUpperCase(),
        featureSplitBegin = feature.slice(0, 2),
        featureSplitEnd = feature.slice(2),
        featureMixedCase = featureSplitBegin.toUpperCase() + featureSplitEnd;
        featureMixedCase2 = featureSplitBegin + featureSplitEnd.toUpperCase();

  test (t => {
    // Tokenizing `name`: initial window features separators are ignored
    // Each of these variants should tokenize to (`${feature}`, '')
    [
      ` ${feature}`,
      `=${feature}`,
      `,,${feature}`,
      `,=, ${feature}`,
      `\n=${feature}=`,
      `\t${feature}`,
      `\r,,,=${feature}`,
      `\u000C${feature}`
    ].forEach(variant => {
      const win = window.open(windowURL, "", variant);
      assert_equals(win, null, `"${variant}" should activate feature "${feature}"`);
    });
  }, `Tokenization of "${feature}" should skip window features separators before feature`);

  test (t => {
    // Tokenizing `name`: lowercase conversion
    // Each of these variants should tokenize as feature (`${feature}`, '')
    // except where indicated
    // Note also that `value` is lowercased during tokenization
    [
      `${featureUpper}`,
      `${featureMixedCase}`,
      `  ${featureMixedCase2}`,
      `=${featureUpper}`,
      `${featureUpper}=1`,
      `${featureUpper}=1`,
      `${featureUpper}=yes`,
      `${feature}=YES`,
    ].forEach(variant => {
      const win = window.open(windowURL, '', variant);
      assert_equals(win, null, `"${variant}" should activate feature "${feature}"`);
    });
  }, `Feature "${feature}" should be converted to ASCII lowercase`);

  test (t => {
    // After `name` has been collected, ignore any window features separators until '='
    // except ',' OR a non-window-features-separator — break in those cases
    // i.e. ignore whitespace until '=' unless a ',' is encountered first
    // Each of these variants should tokenize as feature ('noopener', '')
    [
      `${feature}`,
      ` ${feature}\r`,
      `${feature}\n =`,
      `${feature},`,
      `${feature}  =,`,
      `, ${feature}   =`,
      `${feature},=`,
      `${feature} foo`,
      `foo ${feature}=1`,
      `foo=\u000Cbar\u000C${feature}`
    ].forEach(variant => {
      const win = window.open(windowURL, '', variant);
      assert_equals(win, null, `"${variant}" should activate feature "${feature}"`);
    });
  }, `After "${feature}", tokenization should skip window features separators that are not "=" or ","`);

  test (t => {
    // After initial '=', tokenizing should ignore all separators except ','
    // before collecting `value`
    // Each of these variants should tokenize as feature ('noopener', '')
    // Except where indicated
    [
      `${feature}=  yes`,
      `${feature}==,`,
      `${feature}=\n ,`,
      `${feature} = \t ,`,
      `${feature}\n=\r 1,`,
      `${feature}=,yes`,
      `${feature}= yes=,`,
      `${feature} = \u000Cyes`
    ].forEach(variant => {
      const win = window.open(windowURL, '', variant);
      assert_equals(win, null, `"${variant}" should activate feature "${feature}"`);
    });
  }, `Tokenizing "${feature}" should ignore window feature separators except "," after initial "=" and before value`);

  test (t => {
    // Tokenizing `value` should collect any non-separator code points until first separator
    [
      `${feature}=1`,
      `${feature}=yes`,
      `${feature} = yes ,`,
      `${feature}=\nyes  ,`,
      `${feature}=yes yes`,
      `${feature}=yes\ts`,
      `${feature}==`,
      `${feature}=1\n,`,
      `==${feature}===`,
      `${feature}==\u000C`
    ].forEach(variant => {
      const win = window.open(windowURL, '', variant);
      assert_equals(win, null, `"${variant}" should set "${feature}"`);
    });
  }, `Tokenizing "${feature}" should read characters until first window feature separator as \`value\``);

  test (t => {
    [
      `${feature}=1`,
      `${feature}=2`,
      `${feature}=12345`,
      `${feature}=1.5`,
      `${feature}=-1`,
    ].forEach(variant => {
      const win = window.open(windowURL, '', variant);
      assert_equals(win, null, `"${variant}" should activate feature "${feature}"`);
    });
  }, 'Integer values other than 0 should activate the feature');

  test (t => {
    [
      `${feature}=0`,
      `${feature}=0.5`,
      `${feature}=error`,
    ].forEach(variant => {
      const win = window.open(windowURL, '', variant);
      assert_not_equals(win, null, `"${variant}" should NOT activate feature "${feature}"`);
    });
  }, `Integer value of 0 should not activate "${feature}"`);

  test (t => {
    [
      `-${feature}`,
      `${featureUpper}RRR`,
      `${featureMixedCase}R`,
      `${featureSplitBegin}_${featureSplitEnd}`,
      ` ${featureSplitBegin} ${featureSplitEnd}`,
      `${featureSplitBegin}\n${featureSplitEnd}`,
      `${featureSplitBegin},${featureSplitEnd}`,
      `\0${feature}`,
      `\u000B${feature}`,
      `${feature}\u0000=yes`,
      `foo=\u000C${feature}`
    ].forEach(variant => {
      const win = window.open(windowURL, '', variant);
      assert_not_equals(win, null, `"${variant}" should NOT activate feature "${feature}"`);
    });
  }, `Invalid feature names should not tokenize as "${feature}"`);
}

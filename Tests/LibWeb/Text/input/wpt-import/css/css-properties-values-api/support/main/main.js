function reg_url(name, inherits) {
    CSS.registerProperty({
        name: name,
        syntax: '<url> | none',
        inherits: inherits,
        initialValue: 'none'
    });
}

reg_url('--reg-non-inherited-url', false);
reg_url('--reg-non-inherited-func', false);

reg_url('--reg-inherited-url', true);
reg_url('--reg-inherited-func', true);

reg_url('--reg-ref-to-unreg-url', false);
reg_url('--reg-ref-to-unreg-func', false);

reg_url('--reg-ref-to-reg-url', false);
reg_url('--reg-ref-to-reg-func', false);

reg_url('--reg-merged-func', false);

reg_url('--reg-utf16be-url', false);
reg_url('--reg-utf16be-func', false);

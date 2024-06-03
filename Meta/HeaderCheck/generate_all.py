#!/usr/bin/env python3

import os
import sys
import subprocess

TEST_FILE_TEMPLATE = '''\
#include <{filename}>
// Check idempotency:
#include <{filename}>
'''


def get_headers_here():
    result = subprocess.run(
        ['git', 'ls-files', 'AK/*.h', 'Userland/Libraries/*.h'],
        check=True, capture_output=True, text=True)
    assert result.stderr == ''
    output = result.stdout.split('\n')
    assert output[-1] == ''  # Trailing newline
    assert len(output) > 500, 'There should be well over a thousand headers, not only {}?!'.format(len(output))
    return output[:-1]


def as_filename(header_path):
    return header_path.replace('/', '__') + '__test.cpp'


def verbosely_write(path, new_content):
    print(path)
    # FIXME: Ensure directory exists
    if os.path.exists(path):
        with open(path, 'r') as fp:
            old_data = fp.read()
        if old_data == new_content:
            # Fast path! Don't trigger ninja
            return
    with open(path, 'w') as fp:
        fp.write(new_content)


def generate_part(header):
    content = TEST_FILE_TEMPLATE.format(filename=header)
    if header.startswith('Kernel/'):
        content += '#define KERNEL\n'
    verbosely_write(as_filename(header), content)


def run(root_path, arch):
    os.chdir(root_path)
    headers_list = get_headers_here()

    generated_files_path = os.path.join(root_path, 'Build', arch, 'Meta', 'HeaderCheck')
    if not os.path.exists(generated_files_path):
        os.mkdir(generated_files_path)
    os.chdir(generated_files_path)
    for header in headers_list:
        generate_part(header)


if __name__ == '__main__':
    if 'LADYBIRD_SOURCE_DIR' not in os.environ:
        print('Must set LADYBIRD_SOURCE_DIR first!', file=sys.stderr)
        exit(1)
    if len(sys.argv) == 2:
        run(os.environ['LADYBIRD_SOURCE_DIR'], sys.argv[1])
    else:
        print('Usage: LADYBIRD_SOURCE_DIR=/path/to/serenity {} SERENITY_BUILD_ARCH'
              .format(sys.argv[0]), file=sys.stderr)
        exit(1)

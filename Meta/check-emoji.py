#!/usr/bin/env python3

import os
import re
import sys

RE_INVALID_CHAR = re.compile('[^A-FU0-9+_]')
RE_MISSING_UNDERSCORE = re.compile('[^_]U')
RE_MISSING_LETTER_U = re.compile('_(?!U)')
RE_MISSING_SIGN_PLUS = re.compile('U(?!\\+)')
RE_MULTIPLE_UNDERSCORES = re.compile('__+')


def any_problems_here():
    found_invalid_filenames = False
    for filename in os.listdir():
        if not filename.endswith('.png'):
            print(f'Non-png file {filename} does not belong in the emoji directory')
            found_invalid_filenames = True
            break
        filename = filename[:-len('.png')]
        if RE_INVALID_CHAR.search(filename):
            print(f'Filename {filename}.png contains invalid characters in its filename. Only uppercase letters'
                  ' A-F and U, numbers, +, and _ should be used.')
            found_invalid_filenames = True
            break
        if 'U+0' in filename:
            print(f'Filename {filename}.png contains codepoint(s) with leading zeros. Leading zeros should be'
                  ' removed from codepoint(s).')
            found_invalid_filenames = True
            break
        if '+U' in filename:
            print(f'Filename {filename}.png is incorrectly named. "_" should be used as a separator between'
                  ' codepoints, not "+".')
            found_invalid_filenames = True
            break
        if RE_MISSING_UNDERSCORE.search(filename):
            print(f'Filename {filename}.png is missing an underscore "_" between codepoints.')
            found_invalid_filenames = True
            break
        if RE_MISSING_LETTER_U.search(filename):
            print(f'Filename {filename}.png is either missing a "U" to indicate the start of a codepoint,'
                  ' or has a spurious underscore ("_").')
            found_invalid_filenames = True
            break
        if RE_MISSING_SIGN_PLUS.search(filename):
            print(f'Filename {filename}.png is either missing a "+" after a "U", or has a spurious "U".')
            found_invalid_filenames = True
            break
        if 'U+FE0F' in filename:
            print(f'Filename {filename}.png should not include any emoji presentation selectors. U+FE0F codepoints'
                  ' should be removed from the filename.')
            found_invalid_filenames = True
            break
        if RE_MULTIPLE_UNDERSCORES.search(filename):
            print(f'Filename {filename}.png contains consecutive underscores "__", reduce to only one underscore between codepoints.')
            found_invalid_filenames = True
            break
        if filename.endswith('_'):
            print(f'Filename {filename}.png ends with an underscore "_", revise filename.')
            found_invalid_filenames = True
            break


        code_points = [int(code_point[len('U+'):], 16) for code_point in filename.split('_')]

        if any(code_point > 0x10ffff for code_point in code_points):
            print(f'Filename {filename}.png contains a code point exceeding U+10FFFF')
            found_invalid_filenames = True
            break

    return found_invalid_filenames


if __name__ == '__main__':
    os.chdir(os.path.dirname(__file__) + "/../Base/res/emoji/")
    if any_problems_here():
        sys.exit(1)

#!/usr/bin/env bash

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

echo -e "\033[33;1mWARNING\033[0m: ladybird.sh is deprecated and will be removed soon. Please use ladybird.py instead."
"${DIR}/ladybird.py" "$@"

#!/usr/bin/env bash

# the file containing the commit message is passed as the first argument
commit_file="$1"
commit_message=$(cat "$commit_file")

error() {
  echo -e "\033[0;31m$1:\033[0m"
  echo "$commit_message"
  exit 1
}

# fail if the commit message contains windows style line breaks (carriage returns)
if grep -q -U $'\x0D' "$commit_file"; then
  error "Commit message contains CRLF line breaks (only unix-style LF linebreaks are allowed)"
fi

line_number=0
while read -r line; do
  # break on git cut line, used by git commit --verbose
  if [[ "$line" == "# ------------------------ >8 ------------------------" ]]; then
    break
  fi

  # ignore comment lines
  [[ "$line" =~ ^#.* ]] && continue
  # ignore overlong 'fixup!' commit descriptions
  [[ "$line" =~ ^fixup!\ .* ]] && continue

  ((line_number += 1))
  line_length=${#line}

  if [[ $line_number -eq 2 ]] && [[ $line_length -ne 0 ]]; then
    error "Empty line between commit title and body is missing"
  fi

  merge_commit_pattern="^Merge branch"
  if [[ $line_number -eq 1 ]] && (echo "$line" | grep -E -q "$merge_commit_pattern"); then
    error "Commit is a git merge commit, use the rebase command instead"
  fi

  category_pattern='^(Revert "|\S+: )'
  if [[ $line_number -eq 1 ]] && (echo "$line" | grep -E -v -q "$category_pattern"); then
    error "Missing category in commit title (if this is a fix up of a previous commit, it should be squashed)"
  fi

  title_case_pattern="^\S.*?: [A-Z0-9]"
  if [[ $line_number -eq 1 ]] && (echo "$line" | grep -E -v -q "$title_case_pattern"); then
    error "First word of commit after the subsystem is not capitalized"
  fi

  if [[ $line_number -eq 1 ]] && [[ "$line" =~ \.$ ]]; then
    error "Commit title ends in a period"
  fi

  url_pattern="([a-z]+:\/\/)?(([a-zA-Z0-9_]|-)+\.)+[a-z]{2,}(:\d+)?([a-zA-Z_0-9@:%\+.~\?&\/=]|-)+"
  if [[ $line_length -gt 72 ]] && (echo "$line" | grep -E -v -q "$url_pattern"); then
    error "Commit message lines are too long (maximum allowed is 72 characters)"
  fi

  if [[ "$line" == "Signed-off-by: "* ]]; then
    error "Commit body contains a Signed-off-by tag"
  fi

done <"$commit_file"
exit 0

#!/bin/sh
set -e

if [ -z "$1" ]; then
    echo "usage: $0 \"commit message\"" >&2
    exit 1
fi

cd "$(dirname "$0")"

make

git add -A
if git diff --cached --quiet; then
    echo "checkin: nothing to commit" >&2
    exit 1
fi
git commit -m "$1"
git push

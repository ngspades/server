#!/bin/sh
# Push site/ to the gh-pages branch of the repository (force: that branch only).
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
repo=$(git -C "$here" remote get-url origin)
[ -f "$here/site/index.html" ] || { echo "site/ is not built: run make site pdf" >&2; exit 1; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cp -R "$here/site/." "$tmp/"
cd "$tmp"
git init -q -b gh-pages
git add -A
git -c user.name="ngspades docs" -c user.email="docs@ngspades.invalid" commit -q -m "Publish the manual"
git push -q --force "$repo" gh-pages:gh-pages
echo "pushed gh-pages"

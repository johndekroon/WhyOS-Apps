#!/bin/sh
# stage.sh DIR: the library as MicroPython imports it, in DIR/meshcore and
# DIR/cayennelpp: upstream/ with patches/*.patch applied and port/ on top
# (port/meshcore/__init__.py replaces upstream's). sdapps/build.sh freezes
# DIR into MicroPython; the tests import it from DIR.
set -e
here=$(cd "${0%/*}" && pwd)
d=$1
[ -n "$d" ] || { echo "usage: stage.sh DIR" >&2; exit 2; }
rm -rf "$d/meshcore" "$d/cayennelpp"
mkdir -p "$d"
cp -R "$here/upstream/meshcore" "$here/upstream/cayennelpp" "$d/"
for p in "$here"/patches/*.patch; do
	patch -d "$d" -p1 -s < "$p"
done
cp -R "$here/port/meshcore/." "$d/meshcore/"

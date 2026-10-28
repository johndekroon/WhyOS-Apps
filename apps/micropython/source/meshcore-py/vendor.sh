#!/bin/sh
# Refreshes upstream/: meshcore_py's package and pycayennelpp (which it
# imports for sensor data), unmodified, from the pinned commits. Our
# changes are patches/*.patch and compat/, applied when MicroPython is
# built (sdapps/build.sh).
set -e
MESHCORE_PY_REF=105f407b85b19709ffff8616d3bf1e83b79c0b9a	# v2.3.17, 2026-10-16
LPP_REF=28f2ba4fba602527d3369c9cfbce16b783916933		# v2.4.0, 2021-10-12
here=$(cd "${0%/*}" && pwd)
t=$(mktemp -d)
trap 'rm -rf "$t"' EXIT
fetch() {	# repo ref dir
	git -C "$t" init -q "$3"
	git -C "$t/$3" fetch -q --depth 1 "$1" "$2"
	git -C "$t/$3" checkout -q FETCH_HEAD
}
fetch https://github.com/meshcore-dev/meshcore_py.git $MESHCORE_PY_REF mc
fetch https://github.com/smlng/pycayennelpp.git $LPP_REF lpp
rm -rf "$here/upstream"
mkdir -p "$here/upstream"
cp -R "$t/mc/src/meshcore" "$t/lpp/cayennelpp" "$here/upstream/"
rm -rf "$here/upstream/cayennelpp/tests"
find "$here/upstream" -name __pycache__ -prune -exec rm -rf {} +
cp "$t/mc/LICENSE" "$here/upstream/meshcore/LICENSE"
cp "$t/lpp/LICENSE" "$here/upstream/cayennelpp/LICENSE"
printf 'meshcore_py %s\npycayennelpp %s\n' $MESHCORE_PY_REF $LPP_REF > "$here/VENDORED"

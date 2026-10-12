#!/bin/sh
# A local test source (docs/client.md §6): publish the apps into <dir>/v1
# for https://<host>:<port>/v1/, with a test CA and a server certificate
# for <host>, and serve it.
#
#   tools/local-source.sh <dir> <host> [port]       default port 8443
#
# The store trusts the test CA through a source file with CAFILE=, e.g.
#   ID=whyos  URL=https://<host>:<port>/v1/  CAFILE=<dir>/ca.pem
# Needs openssl and python3 (tools/serve.py).
set -eu
dir=$1 host=$2 port=${3:-8443}
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$dir"
if [ ! -f "$dir/ca.pem" ]; then
	openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 3650 \
		-subj "/CN=WhyOS-Apps test CA" -keyout "$dir/ca.key" -out "$dir/ca.pem" 2>/dev/null
fi
case $host in
	*[!0-9.]*) san="DNS:$host" ;;
	*) san="IP:$host" ;;
esac
openssl req -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -subj "/CN=$host" \
	-keyout "$dir/server.key" -out "$dir/server.csr" 2>/dev/null
printf 'subjectAltName=%s\nbasicConstraints=CA:FALSE\n' "$san" > "$dir/server.ext"
openssl x509 -req -in "$dir/server.csr" -CA "$dir/ca.pem" -CAkey "$dir/ca.key" -CAcreateserial \
	-days 825 -extfile "$dir/server.ext" -out "$dir/server.pem" 2>/dev/null
python3 "$here/publish.py" --base-url "https://$host:$port/v1/" --out "$dir/v1"
exec python3 "$here/serve.py" "$dir" "$dir/server.pem" "$dir/server.key" --port "$port"

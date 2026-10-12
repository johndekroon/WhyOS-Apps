#!/usr/bin/env python3
"""Pick the CA roots the store needs out of Mozilla's list (curl's cacert.pem).

    client/mkroots.py <cacert.pem> > client/roots.pem

The store talks to GitHub only (raw.githubusercontent.com, github.com and
their download hosts), so it carries those roots and their likely
successors, not the whole list (docs/client.md §3).
"""
import re
import sys

NAMES = [
    "ISRG Root X1", "ISRG Root X2",                       # Let's Encrypt: raw.githubusercontent.com
    "USERTrust RSA Certification Authority",              # Sectigo: github.com
    "USERTrust ECC Certification Authority",
    "Sectigo Public Server Authentication Root E46",
    "Sectigo Public Server Authentication Root R46",
    "DigiCert Global Root G2",                            # GitHub's earlier and other hosts
    "DigiCert Global Root G3", "DigiCert TLS ECC P384 Root G5", "DigiCert TLS RSA4096 Root G5",
    "GlobalSign Root CA - R3", "GlobalSign ECC Root CA - R5",   # Fastly
]


def main():
    text = open(sys.argv[1]).read()
    blocks = re.findall(r"\n([^\n]+)\n=+\n(-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----)", text, re.S)
    have = {name.strip(): pem for name, pem in blocks}
    missing = [n for n in NAMES if n not in have]
    if missing:
        sys.exit("not in the list: %s" % ", ".join(missing))
    print("# CA roots of the WhyOS store client (client/mkroots.py), from Mozilla's list")
    for n in NAMES:
        print("# " + n)
        print(have[n])


if __name__ == "__main__":
    main()

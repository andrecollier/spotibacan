"""Generate platform3ds/ca_bundle.c: the root certificates Spotify's hosts need.

    python3 client3ds/platform3ds/gen_ca_bundle.py

Which roots are needed is discovered by walking the real chains: for every
host, the topmost certificate the server sends names its issuer, and that
issuer is looked up as a subject in a root store. Two stores are consulted --
the Mac's own (/etc/ssl/cert.pem) and Mozilla's CCADB export, cached under
deps/ because Apple's store is missing a few roots Spotify's CDNs use.

Re-run it when a handshake starts failing with "unknown CA". The 3DS has no
certificate store of its own to fall back on.
"""
import re
import socket
import ssl
import urllib.request
import subprocess
import sys
from pathlib import Path

OUT = Path(__file__).resolve().parent / "ca_bundle.c"
OUT_PEM = Path(__file__).resolve().parent / "ca_bundle.pem"
APPLE_STORE = Path("/etc/ssl/cert.pem")
CCADB_STORE = Path(__file__).resolve().parents[1] / "deps/ccadb-roots.pem"
CCADB_URL = ("https://ccadb.my.salesforce-sites.com/mozilla/IncludedRootsPEMTxt"
             "?TrustBitsInclude=Websites")

# Every host the client talks to. The audio CDN is whatever storage-resolve
# hands back, so the known CDN domains are probed too.
HOSTS = [
    "login5.spotify.com",            # token exchange
    "accounts.spotify.com",          # device-code pairing
    "apresolve.spotify.com",         # which access point to dial
    "spclient.wg.spotify.com",       # metadata, playlists, storage-resolve
    "api.spotify.com",               # /v1/me: the profile picture
    "i.scdn.co",                     # cover art
    "lrclib.net",                    # synced lyrics
    "audio-ak-spotify-com.akamaized.net",
    "audio-fa.scdn.co",
]

# A CDN answers from whichever edge is closest, and those edges do not all
# chain to the same root -- i.scdn.co alone has been seen under GlobalSign and
# under DigiCert within an hour. These are carried unconditionally so a POP we
# never probed from here does not fail on the console.
ALWAYS = [
    "DigiCert Global Root CA",
    "DigiCert Global Root G2",
    "DigiCert Global Root G3",
    "GlobalSign Root CA - R3",
    "GlobalSign Root R46",
    "GTS Root R1",
    "ISRG Root X1",
    "ISRG Root X2",                  # Let's Encrypt's ECDSA chain (lrclib)
    "Certainly Root R1",
    "Starfield Root Certificate Authority - G2",
    "Amazon Root CA 1",
    "Baltimore CyberTrust Root",
]


def openssl(args, stdin=None):
    return subprocess.run(["openssl", *args], input=stdin, capture_output=True,
                          text=True, check=True).stdout


def subject_of(pem):
    return openssl(["x509", "-noout", "-subject"], pem).strip()


def issuer_of(pem):
    return openssl(["x509", "-noout", "-issuer"], pem).strip()


def ccadb_store():
    """Mozilla's included-roots export, fetched once and kept under deps/."""
    if not CCADB_STORE.exists():
        print("Fetching the CCADB root export...")
        CCADB_STORE.parent.mkdir(parents=True, exist_ok=True)
        ctx = ssl.create_default_context(cafile=str(APPLE_STORE))
        with urllib.request.urlopen(CCADB_URL, timeout=120, context=ctx) as r:
            CCADB_STORE.write_bytes(r.read())
    return CCADB_STORE


def store_roots(*stores):
    """Subject line -> PEM, for every certificate in the given stores.

    Earlier stores win, so Apple's copy of a root is preferred and CCADB only
    fills the gaps.
    """
    roots = {}
    for store in stores:
        blocks = re.findall(r"-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----",
                            store.read_text(), re.S)
        for b in blocks:
            pem = b + "\n"
            try:
                name = subject_of(pem).removeprefix("subject=").strip()
            except subprocess.CalledProcessError:
                continue
            roots.setdefault(name, pem)
    return roots


def chain_of(host):
    ctx = ssl.create_default_context(cafile=str(APPLE_STORE))
    with socket.create_connection((host, 443), timeout=10) as s:
        with ctx.wrap_socket(s, server_hostname=host) as t:
            # Older builds hand back cryptography objects, newer ones raw DER.
            return [ssl.DER_cert_to_PEM_cert(
                        c if isinstance(c, bytes) else c.public_bytes("DER"))
                    for c in t.get_unverified_chain()]


def verify():
    """Check the generated bundle really validates every host, over TLS 1.2."""
    bad = 0
    for host in HOSTS:
        ctx = ssl.create_default_context(cafile=str(OUT_PEM))
        ctx.minimum_version = ctx.maximum_version = ssl.TLSVersion.TLSv1_2
        try:
            with socket.create_connection((host, 443), timeout=10) as s:
                with ctx.wrap_socket(s, server_hostname=host) as t:
                    print(f"  ok   {host:40} {t.version()} {t.cipher()[0]}")
        except Exception as e:
            print(f"  FAIL {host:40} {type(e).__name__}: {e}")
            bad += 1
    return bad


def main():
    if not APPLE_STORE.exists():
        raise SystemExit(f"No certificate store at {APPLE_STORE}")
    roots = store_roots(APPLE_STORE, ccadb_store())
    wanted, failed = {}, []

    for host in HOSTS:
        try:
            chain = chain_of(host)
        except Exception as e:
            failed.append(f"{host}: {type(e).__name__}: {e}")
            continue
        # The server stops one short of the root, so the last certificate it
        # sends names the anchor we have to carry ourselves.
        name = issuer_of(chain[-1]).removeprefix("issuer=").strip()
        pem = roots.get(name)
        if pem is None:
            failed.append(f"{host}: no root for {name!r} in either store")
            continue
        cn = re.search(r"CN\s*=\s*([^,/]+)", name)
        wanted.setdefault(name, (cn.group(1).strip() if cn else name, pem, []))
        wanted[name][2].append(host)

    if not wanted:
        raise SystemExit("Found no roots at all:\n  " + "\n  ".join(failed))

    for cn in ALWAYS:
        hit = [(n, pem) for n, pem in roots.items()
               if re.search(rf"(?:CN|OU)\s*=\s*{re.escape(cn)}(?:,|$)", n)]
        if not hit:
            failed.append(f"ALWAYS root {cn!r} is in neither store")
            continue
        name, pem = hit[0]
        if name in wanted:
            continue
        wanted[name] = (cn, pem, ["spare"])

    # The same anchors as a plain .pem, so the bundle can be checked against
    # the live hosts with any ordinary TLS client (see --verify).
    OUT_PEM.write_text("".join(
        f"# {cn} -- {', '.join(hosts)}\n{pem.strip()}\n"
        for cn, pem, hosts in sorted(wanted.values(), key=lambda v: v[0])))

    body = []
    for name, (cn, pem, hosts) in sorted(wanted.items(), key=lambda kv: kv[1][0]):
        body.append(f"\t/* {cn} -- {', '.join(hosts)} */\n")
        for line in pem.strip().splitlines():
            body.append(f'\t"{line}\\n"\n')
        body.append("\n")

    OUT.write_text(
        "/* Generated by gen_ba_bundle.py -- do not edit.\n"
        " *\n"
        " * The trust anchors for Spotify's hosts, as PEM. mbedTLS parses this\n"
        " * once at startup; there is no certificate store on the console.\n"
        " */\n"
        '#include "ca_bundle.h"\n\n'
        "const char spotify_ca_bundle[] =\n" + "".join(body).rstrip() + ";\n\n"
        "const size_t spotify_ca_bundle_len = sizeof(spotify_ca_bundle);\n"
        .replace("gen_ba_bundle.py", "gen_ca_bundle.py"))

    print(f"Wrote {OUT} and {OUT_PEM.name} with {len(wanted)} root(s):")
    for name, (cn, _, hosts) in sorted(wanted.items(), key=lambda kv: kv[1][0]):
        print(f"  {cn:42} {', '.join(hosts)}")
    for f in failed:
        print(f"  ! {f}", file=sys.stderr)


if __name__ == "__main__":
    if "--verify" in sys.argv:
        raise SystemExit(1 if verify() else 0)
    main()

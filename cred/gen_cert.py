#!/usr/bin/env python3
"""
Generates a local CA and a server certificate for goodtogo.local signed by it.

Works the same on Windows, macOS and Linux. It only needs Python 3 and the
`cryptography` package, which the ESP-IDF Python environment already has
(esptool depends on it). No OpenSSL/LibreSSL install is needed.

Outputs (in the directory of this script):
  ca.key, ca.pem      - The CA. Import ca.pem into your browser/OS as a trusted
                        root. Keep ca.key private; it's reused on later runs so
                        the CA only has to be trusted once.
  prvtkey.pem         - Server private key, embedded into the firmware.
  servercert.pem      - Server certificate, embedded into the firmware.

Usage: python gen_cert.py [--force-ca]
  --force-ca  Regenerate the CA even if one already exists.
"""

import argparse
import datetime
import os
import sys
from pathlib import Path

try:
    from cryptography import x509
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID
except ImportError:
    sys.exit(
        "error: the Python package 'cryptography' is missing.\n"
        "Run this script from an ESP-IDF terminal (export.sh / export.ps1),\n"
        "or install it with: python -m pip install cryptography"
    )

HOSTNAME = "goodtogo.local"
CA_NAME = "Good To Go Local CA"
CA_DAYS = 3650
SERVER_DAYS = 825  # Max validity accepted by Apple platforms

OUT_DIR = Path(__file__).resolve().parent
CA_KEY = OUT_DIR / "ca.key"
CA_CERT = OUT_DIR / "ca.pem"
SERVER_KEY = OUT_DIR / "prvtkey.pem"
SERVER_CERT = OUT_DIR / "servercert.pem"


def write_private(path: Path, data: bytes) -> None:
    """Writes a private key, readable only by the owner where the OS supports it."""
    if path.exists():
        path.unlink()
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(data)


def write_public(path: Path, data: bytes) -> None:
    path.write_bytes(data)


def new_key() -> ec.EllipticCurvePrivateKey:
    return ec.generate_private_key(ec.SECP256R1())  # prime256v1


def key_pem(key: ec.EllipticCurvePrivateKey) -> bytes:
    # Same "BEGIN EC PRIVATE KEY" format as `openssl ecparam -genkey`.
    return key.private_bytes(
        serialization.Encoding.PEM,
        serialization.PrivateFormat.TraditionalOpenSSL,
        serialization.NoEncryption(),
    )


def validity(days: int):
    now = datetime.datetime.now(datetime.timezone.utc)
    return now, now + datetime.timedelta(days=days)


def gen_ca():
    print("Generating CA...")
    key = new_key()
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, CA_NAME)])
    not_before, not_after = validity(CA_DAYS)

    cert = (
        x509.CertificateBuilder()
        .subject_name(name)
        .issuer_name(name)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(not_before)
        .not_valid_after(not_after)
        .add_extension(x509.BasicConstraints(ca=True, path_length=0), critical=True)
        .add_extension(
            x509.KeyUsage(
                digital_signature=False,
                content_commitment=False,
                key_encipherment=False,
                data_encipherment=False,
                key_agreement=False,
                key_cert_sign=True,
                crl_sign=True,
                encipher_only=False,
                decipher_only=False,
            ),
            critical=True,
        )
        .add_extension(
            x509.SubjectKeyIdentifier.from_public_key(key.public_key()), critical=False
        )
        .sign(key, hashes.SHA256())
    )

    write_private(CA_KEY, key_pem(key))
    write_public(CA_CERT, cert.public_bytes(serialization.Encoding.PEM))
    return key, cert


def load_ca():
    key = serialization.load_pem_private_key(CA_KEY.read_bytes(), password=None)
    cert = x509.load_pem_x509_certificate(CA_CERT.read_bytes())
    return key, cert


def gen_server_cert(ca_key, ca_cert) -> x509.Certificate:
    print(f"Generating server certificate for {HOSTNAME}...")
    key = new_key()
    not_before, not_after = validity(SERVER_DAYS)

    cert = (
        x509.CertificateBuilder()
        .subject_name(x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, HOSTNAME)]))
        .issuer_name(ca_cert.subject)
        .public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(not_before)
        .not_valid_after(not_after)
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
        .add_extension(
            x509.KeyUsage(
                digital_signature=True,
                content_commitment=False,
                key_encipherment=False,
                data_encipherment=False,
                key_agreement=False,
                key_cert_sign=False,
                crl_sign=False,
                encipher_only=False,
                decipher_only=False,
            ),
            critical=True,
        )
        .add_extension(
            x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]), critical=False
        )
        .add_extension(
            x509.SubjectAlternativeName([x509.DNSName(HOSTNAME)]), critical=False
        )
        .add_extension(
            x509.SubjectKeyIdentifier.from_public_key(key.public_key()), critical=False
        )
        .add_extension(
            x509.AuthorityKeyIdentifier.from_issuer_public_key(ca_key.public_key()),
            critical=False,
        )
        .sign(ca_key, hashes.SHA256())
    )

    write_private(SERVER_KEY, key_pem(key))
    write_public(SERVER_CERT, cert.public_bytes(serialization.Encoding.PEM))
    return cert


def verify(cert: x509.Certificate, ca_cert: x509.Certificate) -> None:
    try:
        cert.verify_directly_issued_by(ca_cert)
    except (ValueError, TypeError, InvalidSignature) as e:
        sys.exit(f"error: {SERVER_CERT} does not verify against {CA_CERT}: {e}")
    print(f"{SERVER_CERT}: OK")


def main() -> None:
    parser = argparse.ArgumentParser(
        description=f"Generate a local CA and a server certificate for {HOSTNAME}."
    )
    parser.add_argument(
        "--force-ca",
        action="store_true",
        help="Regenerate the CA even if one already exists.",
    )
    args = parser.parse_args()

    if args.force_ca or not CA_KEY.is_file() or not CA_CERT.is_file():
        ca_key, ca_cert = gen_ca()
    else:
        print(f"Reusing existing CA at {CA_CERT}")
        ca_key, ca_cert = load_ca()

    server_cert = gen_server_cert(ca_key, ca_cert)
    verify(server_cert, ca_cert)
    print(f"Done. Trust {CA_CERT} on your client devices to access https://{HOSTNAME}")


if __name__ == "__main__":
    main()

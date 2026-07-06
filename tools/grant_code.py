#!/usr/bin/env python3
"""Generate short offline grant codes for pctltcp-sysmodule."""

from __future__ import annotations

import argparse
import datetime as dt
import hashlib
import hmac
import secrets
import sys


ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"
VERSION = 1
ACTION_ADD_TODAY = 0


def date_key(day: dt.date) -> int:
    if day.year < 2020 or day.year > 2147:
        raise ValueError("date year must be in 2020..2147")
    return ((day.year - 2020) << 9) | (day.month << 5) | day.day


def encode_crockford(data: bytes) -> str:
    bits = int.from_bytes(data, "big")
    chars = []
    for shift in range(75, -1, -5):
        chars.append(ALPHABET[(bits >> shift) & 0x1F])
    text = "".join(chars)
    return "-".join(text[i : i + 4] for i in range(0, len(text), 4))


def make_code(minutes: int, device: str, secret: str, day: dt.date, nonce: int) -> str:
    if minutes <= 0 or minutes > 1023:
        raise ValueError("minutes must be in 1..1023")
    if nonce < 0 or nonce > 0xFFFF:
        raise ValueError("nonce must be in 0..65535")

    payload = (
        (VERSION & 0x0F) << 44
        | (ACTION_ADD_TODAY & 0x03) << 42
        | (minutes & 0x03FF) << 32
        | (date_key(day) & 0xFFFF) << 16
        | (nonce & 0xFFFF)
    )
    payload_bytes = payload.to_bytes(6, "big")
    digest = hmac.new(
        secret.encode("utf-8"),
        device.encode("utf-8") + b"\x00" + payload_bytes,
        hashlib.sha256,
    ).digest()
    token = payload_bytes + digest[:4]
    return encode_crockford(token)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Generate a Switch play-time grant code.")
    parser.add_argument("--minutes", type=int, required=True, help="minutes to add to today's limit")
    parser.add_argument("--device", required=True, help="device_id from grant.conf")
    parser.add_argument("--secret", required=True, help="grant_secret from grant.conf")
    parser.add_argument("--date", help="local grant date, YYYY-MM-DD; defaults to today")
    parser.add_argument("--nonce", type=lambda s: int(s, 0), help="nonce for deterministic tests")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    day = dt.date.fromisoformat(args.date) if args.date else dt.date.today()
    nonce = args.nonce if args.nonce is not None else secrets.randbelow(0x10000)
    code = make_code(args.minutes, args.device, args.secret, day, nonce)

    print(code)
    return 0


if __name__ == "__main__":
    sys.exit(main())

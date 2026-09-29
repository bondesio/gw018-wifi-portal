#!/usr/bin/env python3
"""Apply the reviewed GW018 portal integration to the pinned SDK checkout."""
import argparse
import re
import shutil
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PIN = "f144bf1b19dc869730ef09fc2515760245a26a97"
PORTAL_REL = Path("component/common/example/ota_http")


def run(*args, cwd=None):
    subprocess.run(args, cwd=cwd, check=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("sdk", type=Path, help="clean checkout created by prepare-sdk.sh")
    parser.add_argument("--ota-host", required=True,
                        help="your gateway-reachable OTA server hostname or IPv4 address")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9.-]{0,251}[A-Za-z0-9])?", args.ota_host):
        parser.error("--ota-host must be a hostname or IPv4 address (no URL, port, or shell characters)")

    sdk = args.sdk.resolve()
    if not (sdk / ".git").exists():
        parser.error("SDK path must be a git checkout")
    head = subprocess.check_output(["git", "-C", str(sdk), "rev-parse", "HEAD"], text=True).strip()
    if head != PIN:
        parser.error(f"SDK revision must be {PIN}; found {head}")
    dirty = subprocess.check_output(["git", "-C", str(sdk), "status", "--porcelain"], text=True)
    if dirty:
        parser.error("SDK checkout is not clean; use a fresh checkout (no files were changed)")

    patch = ROOT / "patches/gw018-integration.patch"
    run("git", "-C", str(sdk), "apply", "--check", str(patch))
    run("git", "-C", str(sdk), "apply", str(patch))
    dest = sdk / PORTAL_REL
    src = ROOT / "component/common/example/ota_http"
    shutil.copy2(src / "gw018_portal.c", dest / "gw018_portal.c")
    shutil.copy2(src / "gw018_portal.h", dest / "gw018_portal.h")

    ota_example = dest / "example_ota_http.c"
    content = ota_example.read_text()
    host_line = re.compile(r'(?m)^#define HOST[ \t]+"[^"]*".*$')
    content, count = host_line.subn(
        f'#define HOST\t"{args.ota_host}" /* Configured by apply-overlay.py. */', content
    )
    if count != 1:
        raise SystemExit("Expected exactly one OTA HOST definition; refusing to edit unexpected source")
    ota_example.write_text(content)
    print("Overlay applied. Configure the OTA server to serve OTA_All.bin on TCP port 8080.")


if __name__ == "__main__":
    main()

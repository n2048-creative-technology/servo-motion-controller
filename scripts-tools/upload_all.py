#!/usr/bin/env python3
"""Flash the latest firmware + LittleFS web UI to every connected board that
matches one of firmware/platformio.ini's configured environments.

"Matches the configuration" is determined from platformio.ini itself, not a
hardcoded chip list: each `[env:X]` section names a PlatformIO board, whose
installed board definition (~/.platformio/platforms/espressif32/boards/X.json)
says which MCU family it targets (`build.mcu`, e.g. "esp32c3"). Every
connected serial port is probed with esptool to find out what chip is
actually there, then matched against that mapping — so a stray unrelated
serial device (or a board type this project doesn't build for) is skipped
with a clear reason instead of a failed/garbled flash attempt, and adding a
third environment to platformio.ini later needs no change here.

This mirrors the exact build-once-then-flash-in-parallel workflow used by
hand throughout this project's development: building every matched
environment serially first avoids a real, previously-hit failure mode where
concurrent `pio run` invocations against the *same* environment's build
directory corrupt each other's SCons dependency cache — only the actual
upload (fast, board-specific, safe to parallelize) fans out across boards.

Before touching anything, it also checks that FIRMWARE_VERSION
(firmware/include/Config.h) and UI_VERSION (firmware/data/app.js) — two
independently-hardcoded strings the web UI compares at runtime to show a
"Firmware / web UI mismatch" banner — actually agree, and refuses to flash
if they don't. Firmware and filesystem ship as two separate uploads, so
this class of mismatch is otherwise easy to *cause* even with a fully
successful flash, if the two versions were never kept in sync at the
source in the first place — flashing anyway would just reproduce the same
mismatch banner on every board this run touches.

Usage:
    python3 upload_all.py              # build + flash firmware and filesystem
    python3 upload_all.py --dry-run    # show what would be flashed, do nothing
    python3 upload_all.py --fw-only    # skip the filesystem image
    python3 upload_all.py --fs-only    # skip the firmware binary
    python3 upload_all.py --jobs 2     # limit how many boards flash at once
"""

import argparse
import concurrent.futures
import glob
import json
import re
import subprocess
import sys
import time
from pathlib import Path

import serial

REPO_ROOT = Path(__file__).resolve().parent.parent
FIRMWARE_DIR = REPO_ROOT / "firmware"
PLATFORMIO_INI = FIRMWARE_DIR / "platformio.ini"
PLATFORMIO_BOARDS_DIR = Path.home() / ".platformio" / "platforms" / "espressif32" / "boards"
CONFIG_H = FIRMWARE_DIR / "include" / "Config.h"
APP_JS = FIRMWARE_DIR / "data" / "app.js"

ENV_SECTION_RE = re.compile(r"^\[env:([^\]]+)\]", re.MULTILINE)
CHIP_ID_LINE_RE = re.compile(r"Chip is (ESP32-\S+)")
FIRMWARE_VERSION_RE = re.compile(r'FIRMWARE_VERSION\s*=\s*"([^"]+)"')
UI_VERSION_RE = re.compile(r'UI_VERSION\s*=\s*"([^"]+)"')
BOOT_VERSION_LINE_RE = re.compile(r"firmware version=(\S+)")


def find_esptool():
    """Locate the esptool.py PlatformIO already downloaded — this project's
    Python environment doesn't have esptool importable as a module, only
    this vendored copy (matches how it's been invoked by hand all along)."""
    candidates = sorted((Path.home() / ".platformio" / "packages").glob("tool-esptoolpy*"))
    if not candidates:
        sys.exit("Could not find tool-esptoolpy under ~/.platformio/packages — "
                 "build this project with PlatformIO at least once first.")
    return candidates[0] / "esptool.py"


def find_ports():
    patterns = [
        "/dev/ttyACM*", "/dev/ttyUSB*",       # Linux
        "/dev/cu.usbmodem*", "/dev/cu.SLAB_USBtoUART*", "/dev/cu.usbserial*",  # macOS
    ]
    ports = set()
    for pattern in patterns:
        ports.update(glob.glob(pattern))
    return sorted(ports)


def check_version_consistency():
    """FIRMWARE_VERSION (Config.h) and UI_VERSION (app.js) are two separate
    hardcoded strings — nothing enforces they're bumped together, and
    firmware/filesystem ship as two independent uploads, so the two *on a
    board* can end up mismatched even when a flash mostly "worked" (e.g. one
    upload target failed, or a board was flashed at a different time than
    another). The web UI compares them at runtime and shows a "Firmware /
    web UI mismatch" banner when they disagree — but that only catches it
    after the fact, on a phone screen, once you've already gone looking.
    Catching a *source-level* mismatch here, before any board is even
    touched, is strictly more useful: a mismatch here would go on to
    mismatch on every board this run flashes, no matter how completely."""
    fw_match = FIRMWARE_VERSION_RE.search(CONFIG_H.read_text())
    ui_match = UI_VERSION_RE.search(APP_JS.read_text())
    if not fw_match:
        sys.exit(f"Could not find FIRMWARE_VERSION in {CONFIG_H}")
    if not ui_match:
        sys.exit(f"Could not find UI_VERSION in {APP_JS}")
    fw_version, ui_version = fw_match.group(1), ui_match.group(1)
    if fw_version != ui_version:
        sys.exit(
            f"FIRMWARE_VERSION ({fw_version} in {CONFIG_H.relative_to(REPO_ROOT)}) and "
            f"UI_VERSION ({ui_version} in {APP_JS.relative_to(REPO_ROOT)}) don't match. "
            "Flashing now would just reproduce the web UI's own mismatch banner on every "
            "board — bump whichever one is behind so they agree, then rerun."
        )
    print(f"-- firmware/web UI version check OK ({fw_version}) --")
    return fw_version


def project_environments():
    """[(env_name, mcu), ...] for every [env:X] section in platformio.ini,
    read from that environment's installed board definition."""
    ini_text = PLATFORMIO_INI.read_text()
    envs = []
    for env_name in ENV_SECTION_RE.findall(ini_text):
        board_match = re.search(rf"\[env:{re.escape(env_name)}\][^\[]*?^board\s*=\s*(\S+)",
                                 ini_text, re.MULTILINE | re.DOTALL)
        if not board_match:
            print(f"-- warning: [env:{env_name}] has no 'board = ...' line, skipping --")
            continue
        board_name = board_match.group(1)
        board_json = PLATFORMIO_BOARDS_DIR / f"{board_name}.json"
        if not board_json.exists():
            print(f"-- warning: board definition not found for {board_name} "
                  f"(env {env_name}), skipping --")
            continue
        mcu = json.loads(board_json.read_text())["build"]["mcu"]  # e.g. "esp32c3"
        envs.append((env_name, mcu))
    return envs


def detect_chip(esptool_path, port):
    """Returns esptool's normalized chip family (e.g. "esp32c3"), or None if
    the port didn't answer as an ESP32 within the timeout (not one of our
    boards, mid-reset, or busy — same as any transient USB hiccup seen
    throughout this project's own testing)."""
    try:
        result = subprocess.run(
            [sys.executable, str(esptool_path), "--port", port, "chip_id"],
            capture_output=True, text=True, timeout=15,
        )
    except (subprocess.TimeoutExpired, OSError):
        return None
    match = CHIP_ID_LINE_RE.search(result.stdout)
    if not match:
        return None
    # "ESP32-C3" / "ESP32-S3" / "ESP32" -> "esp32c3" / "esp32s3" / "esp32"
    return match.group(1).lower().replace("-", "")


def build_once(env_names):
    print(f"-- building {', '.join(env_names)} --")
    cmd = ["pio", "run", "-d", str(FIRMWARE_DIR)]
    for env in env_names:
        cmd += ["-e", env]
    if subprocess.run(cmd).returncode != 0:
        sys.exit("\nBuild failed (see PlatformIO output above) — stopping before touching any board.")


def buildfs_once(env_names):
    for env in env_names:
        print(f"-- building filesystem image for {env} --")
        if subprocess.run(["pio", "run", "-d", str(FIRMWARE_DIR), "-e", env, "-t", "buildfs"]).returncode != 0:
            sys.exit(f"\nFilesystem build failed for {env} (see PlatformIO output above) — "
                     "stopping before touching any board.")


def verify_boot_version(port, timeout=8.0):
    """Open the just-flashed board's own serial port and watch its boot banner
    for the "[SELFTEST] firmware version=..." line for confirmation that the
    *running* firmware actually reports the version we just built — not just
    that esptool's own write-then-verify passed. That only proves the bytes
    landed on flash; it doesn't prove the board came up running them (e.g. a
    single failed target inside a combined `-t upload -t uploadfs` pio
    invocation used to still let the *other* target run and the process could
    look done, silently leaving stale firmware paired with a freshly-written
    filesystem — the exact "runs firmware 2.0.0 but web UI files are 2.2.0"
    banner this project hit). esptool resets the board after its own
    operation, so the boot banner is already in flight by the time this opens
    the port — a short read-loop is enough to catch it.
    Returns the reported version string, or None if no boot banner with a
    version line showed up within `timeout` seconds (board still booting
    slowly, port busy, or genuinely didn't come back up)."""
    deadline = time.monotonic() + timeout
    try:
        with serial.Serial(port, 115200, timeout=0.5) as ser:
            buf = b""
            while time.monotonic() < deadline:
                buf += ser.read(4096)
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    match = BOOT_VERSION_LINE_RE.search(line.decode("utf-8", "replace"))
                    if match:
                        return match.group(1)
    except (serial.SerialException, OSError):
        return None
    return None


def erase_flash(esptool_path, port):
    """Wipes the whole chip (app partitions *and* NVS) via esptool before any
    new firmware is written. This is the only reliable way to force a board
    back to firmware-default SSID/password/network mode/node id: NVS
    survives an ordinary `-t upload`/`-t uploadfs` pair untouched (that's the
    point of SettingsStore's network-identity split — see Config.h's
    NET_IDENTITY_NVS_KEY comment), so a board can end up on a password
    nobody remembers (a custom one set once, then "preserved" through every
    update since) with no way to reach its web UI and reset it from there.
    Opt-in via --factory-reset, never automatic, since routine updates
    should keep a board's identity — this is only for "I don't know what's
    on this board any more, wipe it"."""
    result = subprocess.run(
        [sys.executable, str(esptool_path), "--port", port, "erase_flash"],
        capture_output=True, text=True, timeout=60,
    )
    return result.returncode == 0, result.stdout + result.stderr


def flash_one(env_name, port, fw, fs, factory_reset=False, esptool_path=None):
    """Flashes firmware and/or filesystem as *separate* pio invocations
    (rather than one `-t upload -t uploadfs` call) so a failure in the first
    can never be masked by the second still running — see
    verify_boot_version's docstring for the exact failure mode this caused."""
    steps = []  # [(target, ok, output)]
    if factory_reset:
        ok, output = erase_flash(esptool_path, port)
        steps.append(("erase_flash", ok, output))
        if not ok:
            # Don't proceed to flash new firmware onto a board whose erase
            # may have only partially completed — that's a worse, harder to
            # diagnose state than just stopping and reporting the failure.
            return port, env_name, False, steps, None
    if fw:
        result = subprocess.run(
            ["pio", "run", "-d", str(FIRMWARE_DIR), "-e", env_name, "--upload-port", port, "-t", "upload"],
            capture_output=True, text=True,
        )
        steps.append(("upload", result.returncode == 0, result.stdout + result.stderr))
    fw_ok = steps[0][1] if (fw and steps) else True
    if fs:
        if fw and not fw_ok:
            steps.append(("uploadfs", False,
                          "skipped: firmware upload failed first — flashing the filesystem onto "
                          "a board whose firmware didn't take would only reproduce the exact "
                          "firmware/web-UI mismatch this script exists to prevent"))
        else:
            result = subprocess.run(
                ["pio", "run", "-d", str(FIRMWARE_DIR), "-e", env_name, "--upload-port", port, "-t", "uploadfs"],
                capture_output=True, text=True,
            )
            steps.append(("uploadfs", result.returncode == 0, result.stdout + result.stderr))
    ok = all(step_ok for _, step_ok, _ in steps)
    booted_version = verify_boot_version(port) if ok else None
    return port, env_name, ok, steps, booted_version


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--dry-run", action="store_true", help="show what would be flashed, don't touch any board")
    parser.add_argument("--fw-only", action="store_true", help="flash firmware only, skip the filesystem image")
    parser.add_argument("--fs-only", action="store_true", help="flash the filesystem image only, skip firmware")
    parser.add_argument("--jobs", type=int, default=8, help="max boards to flash concurrently (default: 8)")
    parser.add_argument("--factory-reset", action="store_true",
                         help="erase each matched board's whole flash (app + NVS) before flashing — use when "
                              "a board's SSID/password/network mode/node id is unknown or unreachable; normal "
                              "runs preserve that identity across updates, so this is opt-in, not automatic")
    args = parser.parse_args()
    do_fw = not args.fs_only
    do_fs = not args.fw_only

    fw_version = check_version_consistency()

    envs = project_environments()
    if not envs:
        sys.exit(f"No usable [env:...] sections found in {PLATFORMIO_INI}")
    print("-- configured environments --")
    for env_name, mcu in envs:
        print(f"  {env_name}  (chip: {mcu})")

    ports = find_ports()
    if not ports:
        sys.exit("No serial ports found (checked /dev/ttyACM*, /dev/ttyUSB*, /dev/cu.*).")
    print(f"\n-- probing {len(ports)} serial port(s) --")

    esptool_path = find_esptool()
    mcu_to_env = {mcu: env_name for env_name, mcu in envs}  # last one wins if ever ambiguous

    matched = []   # [(port, env_name, chip)]
    unmatched = [] # [(port, chip_or_None)]
    for port in ports:
        chip = detect_chip(esptool_path, port)
        if chip is None:
            print(f"  {port}: no response (not an ESP32, or busy) — skipping")
            unmatched.append((port, None))
            continue
        env_name = mcu_to_env.get(chip)
        if env_name is None:
            print(f"  {port}: chip={chip}, no configured environment targets it — skipping")
            unmatched.append((port, chip))
            continue
        print(f"  {port}: chip={chip} -> {env_name}")
        matched.append((port, env_name, chip))

    if not matched:
        sys.exit("\nNo connected boards matched any configured environment. Nothing to flash.")

    print(f"\n-- {len(matched)} board(s) matched, {len(unmatched)} skipped --")
    if args.dry_run:
        print("-- dry run: stopping before build/flash --")
        return

    build_envs = sorted({env for _, env, _ in matched})
    if do_fw:
        build_once(build_envs)
    if do_fs:
        buildfs_once(build_envs)

    if args.factory_reset:
        print("\n-- --factory-reset requested: each matched board's whole flash (app + NVS) "
              "will be erased first --")

    print(f"\n-- flashing {len(matched)} board(s) (up to {args.jobs} at once) --")
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(flash_one, env, port, do_fw, do_fs, args.factory_reset, esptool_path)
                   for port, env, _ in matched]
        for future in concurrent.futures.as_completed(futures):
            port, env_name, ok, steps, booted_version = future.result()
            status = "OK" if ok else "FAILED"
            print(f"  {port} ({env_name}): {status}")
            for target, step_ok, output in steps:
                if not step_ok:
                    print(f"    [{target}] FAILED:")
                    print("      " + "\n      ".join(output.strip().splitlines()[-15:]))
            version_ok = do_fw and ok and booted_version == fw_version
            if do_fw and ok:
                if booted_version is None:
                    print(f"    [boot check] no version reported within timeout — "
                          f"couldn't confirm the board actually came up on {fw_version}")
                elif booted_version != fw_version:
                    print(f"    [boot check] MISMATCH — board booted reporting "
                          f"firmware version={booted_version}, expected {fw_version}. "
                          "The write likely didn't take even though pio/esptool reported success; "
                          "try reflashing this board alone.")
                else:
                    print(f"    [boot check] confirmed running firmware version={booted_version}")
            results.append((port, env_name, ok, booted_version, version_ok))

    print("\n-- summary --")
    failed = [r for r in results if not r[2]]
    version_mismatches = [r for r in results if do_fw and r[2] and not r[4]]
    for port, env_name, ok, booted_version, version_ok in sorted(results):
        tag = "OK  " if ok else "FAIL"
        version_note = ""
        if do_fw and ok:
            version_note = f"  (running {booted_version})" if version_ok else "  (VERSION UNCONFIRMED)"
        print(f"  {tag}  {port}  {env_name}{version_note}")
    if unmatched:
        print(f"  skipped {len(unmatched)} port(s) matching no configured environment:")
        for port, chip in unmatched:
            print(f"    {port}  (chip={chip or 'no response'})")

    if failed:
        sys.exit(f"\n{len(failed)} of {len(results)} board(s) failed to flash.")
    if version_mismatches:
        ports = ", ".join(r[0] for r in version_mismatches)
        sys.exit(f"\npio/esptool reported success, but {len(version_mismatches)} board(s) didn't boot "
                  f"back up confirming firmware version={fw_version}: {ports}. Don't trust these boards "
                  "as flashed — reflash them individually and re-check.")
    print(f"\nAll {len(results)} board(s) flashed successfully and confirmed running firmware version={fw_version}."
          if do_fw else f"\nAll {len(results)} board(s) flashed successfully.")


if __name__ == "__main__":
    main()

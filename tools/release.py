#!/usr/bin/env python3
"""Packs a built firmware for the flashing page and the online update check.
  docs/firmware/GateMaster-<v>-full.bin    whole flash image (bootloader + partitions + app) for new installs
  docs/firmware/GateMaster-<v>-devkit.bin  the same for a plain ESP32 DevKit (starts in Simulator mode)
  docs/firmware/GateMaster-<v>-update.bin  the app alone, for OTA updates and manual upload
  docs/manifest.json, manifest-devkit.json ESP Web Tools manifests (flashing page)
  docs/version.json                        what the boards' "Check for update" reads
Usage: tools/release.py "release notes" [--esptool PATH]"""
import hashlib, json, os, pathlib, re, shutil, subprocess, sys, datetime, glob

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
DOCS = ROOT / "docs"
version = re.search(r'#define FW_VERSION "([^"]+)"', (ROOT / "firmware/GateMaster/config.h").read_text()).group(1)
notes = next((a for a in sys.argv[1:] if not a.startswith("--")), "")
esptool = None
if "--esptool" in sys.argv:
    esptool = sys.argv[sys.argv.index("--esptool") + 1]
else:
    found = sorted(glob.glob(os.path.expanduser("~/.arduino15/packages/esp32/tools/esptool_py/*/esptool.py")))
    esptool = found[-1] if found else "esptool.py"
boot_app0 = sorted(glob.glob(os.path.expanduser("~/.arduino15/packages/esp32/hardware/esp32/*/tools/partitions/boot_app0.bin")))[-1]

fw = DOCS / "firmware"
fw.mkdir(parents=True, exist_ok=True)
for old in fw.glob("GateMaster-*.bin"):
    old.unlink()
full = fw / f"GateMaster-{version}-full.bin"
devkit = fw / f"GateMaster-{version}-devkit.bin"
upd = fw / f"GateMaster-{version}-update.bin"
def merge(build, out):
    subprocess.run([sys.executable, esptool, "--chip", "esp32", "merge_bin", "-o", str(out),
                    "--flash_mode", "dio", "--flash_freq", "80m", "--flash_size", "4MB",
                    "0x1000", str(build / "GateMaster.ino.bootloader.bin"),
                    "0x8000", str(build / "GateMaster.ino.partitions.bin"),
                    "0xe000", boot_app0,
                    "0x10000", str(build / "GateMaster.ino.bin")], check=True, stdout=subprocess.DEVNULL)
merge(BUILD, full)
merge(ROOT / "build-devkit", devkit)
shutil.copyfile(BUILD / "GateMaster.ino.bin", upd)
data = upd.read_bytes()
sha = hashlib.sha256(data).hexdigest()
today = datetime.date.today().isoformat()

for name, img in (("manifest.json", full), ("manifest-devkit.json", devkit)):
    (DOCS / name).write_text(json.dumps({
        "name": "GateMaster" + (" (DevKit simulator)" if img == devkit else ""), "version": version,
        "new_install_prompt_erase": True, "new_install_improv_wait_time": 20,
        "builds": [{"chipFamily": "ESP32", "parts": [{"path": f"firmware/{img.name}", "offset": 0}]}]}, indent=2) + "\n")
(DOCS / "version.json").write_text(json.dumps({
    "version": version, "date": today, "notes": notes,
    "url": f"firmware/{upd.name}", "sha256": sha, "size": len(data)}, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
print(f"GateMaster {version}: {full.name}, {devkit.name}, {upd.name} ({len(data)} bytes, sha256 {sha[:16]}...)")

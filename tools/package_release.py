"""Construit l'archive de release dans dist/ : binaires Release, README, licence et mentions
des bibliothèques tierces (copyright des paquets vcpkg liés statiquement).

Usage : python tools/package_release.py   (après cmake --build build --config Release)
"""
import os
import re
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build", "Release")
SHARE = os.path.join(ROOT, "build", "vcpkg_installed", "x64-windows-static", "share")
BINARIES = ["warband_vr_injector.exe", "warband_vr.dll", "warband_vr_config.exe"]
DOCS = ["README.md", "README.fr.md", "LICENSE"]
# Bibliothèques liées dans les binaires : (paquet vcpkg, nom affiché, site)
THIRD_PARTY = [
    ("minhook", "MinHook", "https://github.com/TsudaKageyu/minhook"),
    ("openxr-loader", "OpenXR SDK (loader)", "https://github.com/KhronosGroup/OpenXR-SDK"),
    ("jsoncpp", "JsonCpp (used by the OpenXR loader)", "https://github.com/open-source-parsers/jsoncpp"),
]


def version():
    cmake = open(os.path.join(ROOT, "CMakeLists.txt"), encoding="utf-8").read()
    m = re.search(r"project\(WarbandVR VERSION ([0-9.]+)", cmake)
    if not m:
        sys.exit("version introuvable dans CMakeLists.txt")
    return m.group(1)


def notices():
    parts = ["WarbandVR includes the following third-party software.\n"]
    for pkg, name, url in THIRD_PARTY:
        path = os.path.join(SHARE, pkg, "copyright")
        if not os.path.exists(path):
            sys.exit(f"licence introuvable : {path}")
        text = open(path, encoding="utf-8-sig", errors="replace").read().strip()
        parts.append(f"\n{'=' * 78}\n{name}\n{url}\n{'=' * 78}\n\n{text}\n")
    return "".join(parts)


def main():
    v = version()
    for f in BINARIES:
        if not os.path.exists(os.path.join(BUILD, f)):
            sys.exit(f"binaire manquant : {f} (compiler en Release d'abord)")
    text = notices()
    with open(os.path.join(ROOT, "THIRD_PARTY_NOTICES.txt"), "w", encoding="utf-8", newline="\r\n") as out:
        out.write(text)
    dist = os.path.join(ROOT, "dist")
    os.makedirs(dist, exist_ok=True)
    name = f"WarbandVR-v{v}"
    archive = os.path.join(dist, name + ".zip")
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED) as z:
        for f in BINARIES:
            z.write(os.path.join(BUILD, f), f"{name}/{f}")
        for f in DOCS + ["THIRD_PARTY_NOTICES.txt"]:
            z.write(os.path.join(ROOT, f), f"{name}/{f}")
    print(archive)
    for info in zipfile.ZipFile(archive).infolist():
        print(f"  {info.filename}  {info.file_size} octets")


if __name__ == "__main__":
    main()

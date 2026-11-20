#!/usr/bin/env python3
"""Package mGBA builds for macOS, Switch, or both as a WoPlay module ZIP."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
from zipfile import ZIP_DEFLATED, ZipFile


def output(*args):
    return subprocess.check_output(args, text=True)


def bundle_macos(module, build):
    cache = (build / "CMakeCache.txt").read_text()
    molten = next(line.split("=", 1)[1] for line in cache.splitlines()
                  if line.startswith("MOLTENVK:FILEPATH="))
    files = [module / "woplay-mgba", module / "libMoltenVK.dylib"]
    shutil.copy2(molten, files[1])
    for prefix in (Path("/opt/homebrew"), Path("/usr/local")):
        sdl3 = prefix / "opt/sdl3/lib/libSDL3.dylib"
        if sdl3.is_file():
            files.append(Path(shutil.copy2(sdl3, module / sdl3.name)))
            break
    for binary in files:
        dependencies = output("otool", "-L", str(binary)).splitlines()[1:]
        for entry in dependencies:
            library = entry.strip().split(" (", 1)[0]
            if not library.startswith(("/opt/homebrew/", "/usr/local/")):
                continue
            source = Path(library)
            target = module / source.name
            if target == binary:
                continue
            if not target.exists():
                shutil.copy2(source, target)
                files.append(target)
            subprocess.run(["install_name_tool", "-change", library,
                            "@loader_path/" + source.name, binary], check=True)
        if binary.suffix == ".dylib":
            subprocess.run(["install_name_tool", "-id", "@loader_path/" + binary.name, binary], check=True)
        subprocess.run(["codesign", "--force", "--sign", "-", binary], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--macos", type=Path)
    parser.add_argument("--switch", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    builds = {platform: build for platform, build in (("macos", args.macos), ("switch", args.switch)) if build}
    if not builds:
        parser.error("provide --macos or --switch")
    destination = args.output.resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="woplay-mgba-", dir=destination.parent) as staging:
        root = Path(staging)
        module = root / "mgba"
        for build in builds.values():
            subprocess.run(["cmake", "--install", build.resolve(), "--prefix", module], check=True)
        manifest = module / "module.json"
        descriptor = json.loads(manifest.read_text(encoding="utf-8"))
        if args.macos:
            if not (module / descriptor["executables"]["macos"]).is_file():
                raise RuntimeError("macOS build did not install the mGBA executable")
            bundle_macos(module, args.macos)
        if args.switch and not (module / descriptor["nro"]).is_file():
            raise RuntimeError("Switch build did not install the mGBA NRO")
        if len(builds) == 1:
            platform = next(iter(builds))
            descriptor["packagePlatform"] = platform
            descriptor.pop("nro" if platform == "macos" else "executables")
            manifest.write_text(json.dumps(descriptor, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        shutil.copy2(Path(__file__).resolve().parents[1] / "LICENSE", module / "LICENSE-mGBA.txt")
        archive = root / destination.name
        with ZipFile(archive, "w", ZIP_DEFLATED) as result:
            for path in sorted(module.rglob("*")):
                if path.is_file():
                    result.write(path, path.relative_to(root))
        archive.replace(destination)
    print(destination)


if __name__ == "__main__":
    main()

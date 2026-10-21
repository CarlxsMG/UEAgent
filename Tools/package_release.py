"""
package_release.py -- build the release archives of UEAgent from the commit checked out (Windows).

Usage:
  python Tools/package_release.py --engine "C:/Program Files/Epic Games/UE_5.8" --out D:/UEAgentRel
  python Tools/package_release.py --engine <UE root> --out <dir> --test-project <some.uproject>

Steps (RELEASING.md tells when to run it):
  1. the source is HEAD, exported with `git archive`: a release is built from a commit, so
     uncommitted changes to tracked files stop the run;
  2. RunUAT BuildPlugin builds and packages it for Win64;
  3. the debug symbols (.pdb) move to an archive of their own;
  4. with --test-project, the packaged plugin replaces <project>/Plugins/UEAgent and the contract
     self-tests run in a headless editor (use a Blueprint-only project: it proves that the
     precompiled plugin loads without a compiler);
  5. UEAgent-<version>-UE<major.minor>-Win64.zip, its -symbols.zip and SHA256SUMS.txt are written
     to --out.

With --fab it builds what Fab takes instead (python Tools/package_release.py --engine <UE root>
--out <dir> --fab --fab-url <the listing's FabURL>): the source, for Epic to compile, one archive
per engine version. From the same export it
  1. keeps only the plugin's own files (FAB_KEEP), not Tools/, docs/, .github/, LICENSE or README.md;
  2. adds to the descriptor EngineVersion (<major.minor>.0) and FabURL, which Fab requires;
  3. checks what Fab's technical requirements refuse and a file listing shows: a source file
     without the copyright notice, a module without a platform list, a path over 170 characters,
     an .exe or .msi, Binaries/ or Intermediate/;
  4. builds it with BuildPlugin -Rocket, the test Fab's requirements name, and lists the compiler
     warnings in UEAgent's code (Fab refuses consequential ones);
  5. writes UEAgent-<version>-UE<major.minor>-Fab.zip to --out.

BuildPlugin fails on paths longer than Windows' 260 characters, so keep --out short.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import re
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_copyright import missing_notice  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
VECTORS = ("refuse_vectors", "registry_vectors", "readback_vectors")

# What a Fab archive carries: the plugin's own folders and CHANGELOG.md. Not docs/, whose pages the
# listing links on the documentation site, nor LICENSE and README.md: on Fab the plugin is licensed
# under Fab's Standard License (Distribution Agreement 3(f)(v)), and the MIT file, with the README's
# MIT badge and License section, would say otherwise.
FAB_KEEP = {"UEAgent.uplugin", "Config", "Resources", "Source", "CHANGELOG.md"}
FAB_URL_PREFIX = "com.epicgames.launcher://ue/Fab/product/"
FAB_MAX_PATH = 170  # characters, counted from the plugin folder (UEAgent/...)
FAB_REFUSED_DIRS = {"Binaries", "Intermediate", "Saved", "DerivedDataCache"}
FAB_NAME = re.compile(r"^[A-Za-z0-9_.]+$")  # "English alphanumeric characters and underscores", plus the extension
# A compiler or UnrealBuildTool warning: "<file>(<line>,<col>): warning C4996: ..." or "<file>: warning: ...".
# Against an installed engine BuildPlugin compiles UEAgent's modules only, so each one comes from building
# UEAgent, even when it names an engine header (a monolithic header UEAgent includes, say).
COMPILER_WARNING = re.compile(r"^\s*(\S.*?)(?:\(\d+(?:,\d+)*\))?: warning\b:?\s*(.*)$", re.IGNORECASE)


def fail(message: str) -> None:
    print(f"FAIL: {message}")
    sys.exit(1)


def git(*args: str) -> bytes:
    return subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True, check=True).stdout


def export_head(destination: Path) -> str:
    """Writes HEAD's tree to destination and returns the commit."""
    if git("status", "--porcelain", "--untracked-files=no").strip():
        fail("uncommitted changes: commit them first (a release is built from a commit)")
    commit = git("rev-parse", "HEAD").decode().strip()
    with tarfile.open(fileobj=io.BytesIO(git("archive", "--format=tar", "HEAD"))) as archive:
        try:
            archive.extractall(destination, filter="data")
        except TypeError:  # Python without extraction filters
            archive.extractall(destination)
    return commit


def build_plugin(engine: Path, source: Path, package: Path, log: Path) -> None:
    run_uat = engine / "Engine" / "Build" / "BatchFiles" / "RunUAT.bat"
    command = [str(run_uat), "BuildPlugin", f"-Plugin={source / 'UEAgent.uplugin'}", f"-Package={package}",
               "-TargetPlatforms=Win64", "-WaitForUATMutex"]  # wait for another AutomationTool rather than fail
    if (engine / "Engine" / "Build" / "InstalledBuild.txt").exists():
        command.append("-Rocket")  # an installed engine (Epic Games Launcher), not a source build
    print(f"BuildPlugin ... (log: {log})")
    with open(log, "wb") as out:
        result = subprocess.run(command, stdout=out, stderr=subprocess.STDOUT)
    if result.returncode != 0:
        tail = log.read_text(encoding="utf-8", errors="replace").splitlines()[-15:]
        print("\n".join(tail))
        fail(f"BuildPlugin exited with {result.returncode}")


def split_symbols(package: Path, dist: Path, symbols: Path) -> None:
    shutil.copytree(package, dist)
    for pdb in sorted(dist.rglob("*.pdb")):
        target = symbols / pdb.relative_to(dist)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.move(str(pdb), target)


def run_self_tests(engine: Path, project: Path, dist: Path, out: Path) -> None:
    plugin = project.parent / "Plugins" / "UEAgent"
    if plugin.exists():
        if not (plugin / "UEAgent.uplugin").exists():
            fail(f"{plugin} exists and is not UEAgent: refusing to replace it")
        shutil.rmtree(plugin)
    shutil.copytree(dist, plugin)
    tests = plugin / "Resources" / "ContractTests"
    if " " in str(tests):
        fail(f"{tests} has a space: the console command that runs the self-tests cannot take it")
    commands = ", ".join(f"mcp.contract.selftest {(tests / name).as_posix()}.json" for name in VECTORS) + " quit"
    editor = engine / "Engine" / "Binaries" / "Win64" / "UnrealEditor-Cmd.exe"
    log_name = "UEAgentRelease.log"
    # A string, not a list: the editor reads -ExecCmds="..." only with the quotes after the '='.
    command_line = (f'"{editor}" "{project}" -ExecCmds="{commands}" -unattended -nopause -nosplash '
                    f"-NullRHI -NoSound -log={log_name}")
    print(f"Self-tests in {project.name} ...")
    subprocess.run(command_line, timeout=1800)
    log = project.parent / "Saved" / "Logs" / log_name
    if log.exists():
        shutil.copyfile(log, out / "selftest.log")
    for name in VECTORS:
        report = tests / f"{name}.json.result.json"
        if not report.exists():
            fail(f"{name}: no result (see {out / 'selftest.log'})")
        result = json.loads(report.read_text(encoding="utf-8"))
        failures = len(result.get("failures") or [])
        print(f"  {name}: {result.get('passed')}/{result.get('checks')}, {failures} failures")
        if failures:
            fail(f"{name} has failures")


def write_zip(source: Path, archive: Path) -> None:
    with zipfile.ZipFile(archive, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zipped:
        for file in sorted(p for p in source.rglob("*") if p.is_file()):
            zipped.write(file, (Path("UEAgent") / file.relative_to(source)).as_posix())


def trim_for_fab(source: Path) -> None:
    for entry in source.iterdir():
        if entry.name in FAB_KEEP:
            continue
        if entry.is_dir():
            shutil.rmtree(entry)
        else:
            entry.unlink()


def stamp_descriptor(descriptor: Path, engine_version: str, fab_url: str | None) -> None:
    """Adds EngineVersion after VersionName and FabURL after MarketplaceURL: the repository's
    descriptor serves both engines and has no listing, so it carries neither."""
    plugin = json.loads(descriptor.read_text(encoding="utf-8"))
    stamped = {}
    for key, value in plugin.items():
        if key in ("EngineVersion", "FabURL"):
            continue
        stamped[key] = value
        if key == "VersionName":
            stamped["EngineVersion"] = f"{engine_version}.0"
        if key == "MarketplaceURL" and fab_url:
            stamped["FabURL"] = fab_url
    descriptor.write_text(json.dumps(stamped, indent="\t") + "\n", encoding="utf-8", newline="\n")


def fab_problems(source: Path) -> tuple[list[str], list[str]]:
    """What Fab refuses in the plugin folder, and the names its naming rule may refuse."""
    problems = [f"no copyright notice: {p.relative_to(source).as_posix()}" for p in missing_notice(source)[1]]
    plugin = json.loads((source / "UEAgent.uplugin").read_text(encoding="utf-8"))
    if "EngineVersion" not in plugin:
        problems.append("the descriptor has no EngineVersion")
    for module in plugin["Modules"]:
        if "PlatformAllowList" not in module and "PlatformDenyList" not in module:
            problems.append(f"module {module['Name']} has no PlatformAllowList or PlatformDenyList")
    odd_names = set()
    for path in sorted(source.rglob("*")):
        relative = (Path("UEAgent") / path.relative_to(source)).as_posix()
        if len(relative) > FAB_MAX_PATH:
            problems.append(f"path over {FAB_MAX_PATH} characters: {relative}")
        if path.suffix.lower() in (".exe", ".msi"):
            problems.append(f"executable: {relative}")
        if path.is_dir() and path.name in FAB_REFUSED_DIRS:
            problems.append(f"local build folder: {relative}")
        if not FAB_NAME.match(path.name):
            odd_names.add(path.name)
    return problems, sorted(odd_names)


def compiler_warnings(log: Path) -> dict[str, int]:
    """The build's warnings, each one once with how many times it came (one per file that included it)."""
    counts: dict[str, int] = {}
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        match = COMPILER_WARNING.match(line)
        if match:
            warning = f"{Path(match.group(1)).name}: {match.group(2)}"[:200]
            counts[warning] = counts.get(warning, 0) + 1
    return counts


def package_for_fab(engine: Path, engine_version: str, version: str, source: Path, package: Path, out: Path,
                    fab_url: str | None) -> int:
    trim_for_fab(source)
    stamp_descriptor(source / "UEAgent.uplugin", engine_version, fab_url)
    problems, odd_names = fab_problems(source)
    warnings: dict[str, int] = {}
    if not problems:
        log = out / "buildplugin.log"
        build_plugin(engine, source, package, log)
        warnings = compiler_warnings(log)
        problems = fab_problems(source)[0]  # BuildPlugin must not have left anything in the source
    for problem in problems:
        print(f"  {problem}")
    if problems:
        fail(f"{len(problems)} things Fab refuses")

    archive = out / f"UEAgent-{version}-UE{engine_version}-Fab.zip"
    write_zip(source, archive)
    print(f"\n  {archive.name}  {archive.stat().st_size:,} bytes, EngineVersion {engine_version}.0")
    if warnings:
        print(f"\nWARNING: {sum(warnings.values())} compiler warnings building UEAgent; Fab refuses consequential ones:")
        for warning, count in sorted(warnings.items(), key=lambda item: -item[1])[:20]:
            print(f"  {count:4}x {warning}")
    if odd_names:
        print(f"\nWARNING: {len(odd_names)} names have characters other than letters, digits and underscores, "
              "which Fab's naming rule does not list: " + ", ".join(odd_names))
    if not fab_url:
        print("\nWARNING: no --fab-url, so the descriptor has no FabURL and Fab refuses the archive. Create the "
              "listing's draft in the publisher portal and run again with its URL.")
    print(f"\nNext: upload {archive.name} as the listing's project version for Unreal Engine {engine_version}.")
    return 0


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--engine", required=True, type=Path, help="Unreal Engine root (the folder with Engine/)")
    parser.add_argument("--out", required=True, type=Path, help="Output folder; keep the path short")
    parser.add_argument("--test-project", type=Path, help="A .uproject to install the package into and self-test")
    parser.add_argument("--fab", action="store_true", help="Build the source archive Fab takes, for this engine")
    parser.add_argument("--fab-url", help=f"With --fab: the listing's FabURL, {FAB_URL_PREFIX}<id>")
    args = parser.parse_args()
    sys.stdout.reconfigure(line_buffering=True)  # keep our lines in order with the tools' output
    if args.fab and args.test_project:
        fail("--fab builds no binaries to self-test: run without --fab for that")
    if args.fab_url and not args.fab:
        fail("--fab-url goes with --fab")
    if args.fab_url and not args.fab_url.startswith(FAB_URL_PREFIX):
        fail(f"--fab-url must start with {FAB_URL_PREFIX}")

    engine, out = args.engine.resolve(), args.out.resolve()
    build_version = engine / "Engine" / "Build" / "Build.version"
    if not build_version.exists():
        fail(f"not an Unreal Engine root: {engine}")
    version_json = json.loads(build_version.read_text(encoding="utf-8"))
    engine_version = f"{version_json['MajorVersion']}.{version_json['MinorVersion']}"
    if len(str(out)) > 40:
        print(f"WARNING: {out} is long; BuildPlugin may hit Windows' path limit")

    out.mkdir(parents=True, exist_ok=True)
    source, package = out / "src" / "UEAgent", out / "pkg" / "UEAgent"
    dist, symbols = out / "dist" / "UEAgent", out / "symbols" / "UEAgent"
    for stale in (out / "src", out / "pkg", out / "dist", out / "symbols"):
        shutil.rmtree(stale, ignore_errors=True)
    # Each mode clears only its own archives, so both can share --out.
    stale_archives = list(out.glob("UEAgent-*-Fab.zip")) if args.fab else \
        list(out.glob("UEAgent-*-Win64*.zip")) + [out / "SHA256SUMS.txt"]
    for stale in stale_archives:
        stale.unlink(missing_ok=True)

    commit = export_head(source)
    version = json.loads((source / "UEAgent.uplugin").read_text(encoding="utf-8"))["VersionName"]
    print(f"UEAgent {version} from {commit[:7]}, Unreal Engine {engine_version}")
    if f"## {version}" not in (source / "CHANGELOG.md").read_text(encoding="utf-8"):
        print(f"WARNING: CHANGELOG.md has no '## {version}' section")
    if args.fab:
        return package_for_fab(engine, engine_version, version, source, package, out, args.fab_url)

    build_plugin(engine, source, package, out / "buildplugin.log")
    split_symbols(package, dist, symbols)
    if args.test_project:
        run_self_tests(engine, args.test_project.resolve(), dist, out)

    stem = f"UEAgent-{version}-UE{engine_version}-Win64"
    archives = [out / f"{stem}.zip", out / f"{stem}-symbols.zip"]
    write_zip(dist, archives[0])
    write_zip(symbols, archives[1])
    sums = "".join(f"{sha256(a)}  {a.name}\n" for a in archives)
    (out / "SHA256SUMS.txt").write_text(sums, encoding="utf-8", newline="\n")

    print()
    for archive in archives:
        print(f"  {archive.name}  {archive.stat().st_size:,} bytes")
    print(sums, end="")
    files = " ".join(f'"{p}"' for p in archives + [out / "SHA256SUMS.txt"])
    print(f'\nNext: gh release create v{version} --title "UEAgent {version}" --target {commit} '
          f"--notes-file <notes.md> {files}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

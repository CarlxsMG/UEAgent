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

BuildPlugin fails on paths longer than Windows' 260 characters, so keep --out short.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import shutil
import subprocess
import sys
import tarfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VECTORS = ("refuse_vectors", "registry_vectors", "readback_vectors")


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
    args = parser.parse_args()
    sys.stdout.reconfigure(line_buffering=True)  # keep our lines in order with the tools' output

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
    for stale in list(out.glob("UEAgent-*.zip")) + [out / "SHA256SUMS.txt"]:
        stale.unlink(missing_ok=True)

    commit = export_head(source)
    version = json.loads((source / "UEAgent.uplugin").read_text(encoding="utf-8"))["VersionName"]
    print(f"UEAgent {version} from {commit[:7]}, Unreal Engine {engine_version}")
    if f"## {version}" not in (source / "CHANGELOG.md").read_text(encoding="utf-8"):
        print(f"WARNING: CHANGELOG.md has no '## {version}' section")

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

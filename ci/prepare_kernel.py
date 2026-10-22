#!/usr/bin/env python3
"""Verify the SY108 kernel archive against its pinned Git tree and audited hashes.

No source files or checksum checks are disabled. This helper changes only the
archive SHA256 in the build checkout, after complete content verification. It
does not accept arbitrary new hashes. See README-zh-CN.txt for the evidence.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile
import time


COMMIT = "606233b63c83997b59d68cdceaab4dd6ca4a3286"
TREE = "d8453d0a85cdb05e3a7b462ade2c904b3a672a5f"
ARCHIVE = "linux-7.3.0-rc7-sy108-606233b63c83.tar.gz"
PREFIX = "linux-7.3.0-rc7/"
ORIGINAL_SHA256 = "6ee4031a7f2df84f217e79b572c35d5ff7208338d95bc8a3cf913133364e221f"
AUDITED_SHA256 = "d2729f146878d4cace62a560d84f98af56837b4e33e1e554d626e29887e90694"
APPROVED_HASHES = {ORIGINAL_SHA256, AUDITED_SHA256}
REPO_URL = "https://github.com/jason-liu-4980/linux-udx710.git"


def command(args: list[str], **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def git(repo: Path, *args: str) -> bytes:
    return subprocess.check_output(["git", "-C", str(repo), *args])


def prepare_repository(repo: Path) -> None:
    if not repo.exists():
        repo.parent.mkdir(parents=True, exist_ok=True)
        # bootstrap.py and kernel-source.py only need Git objects, not a checkout.
        # A bare repository avoids storing an extra ~1.6 GB working tree.
        command(["git", "init", "--bare", str(repo)])
        command(["git", "-C", str(repo), "remote", "add", "origin", REPO_URL])
        command(["git", "-C", str(repo), "fetch", "--depth=1", "--no-tags", "origin", COMMIT])
        command(["git", "-C", str(repo), "update-ref", "refs/heads/sy108-source", COMMIT])
        command(["git", "-C", str(repo), "symbolic-ref", "HEAD", "refs/heads/sy108-source"])
    actual_commit = git(repo, "rev-parse", COMMIT + "^{commit}").decode().strip()
    actual_tree = git(repo, "rev-parse", COMMIT + "^{tree}").decode().strip()
    if (actual_commit, actual_tree) != (COMMIT, TREE):
        raise RuntimeError("The pinned kernel commit/tree does not match the audited source")
    command(["git", "-C", str(repo), "fsck", "--full", "--no-dangling"])


def create_archive(repo: Path, archive: Path) -> None:
    archive.parent.mkdir(parents=True, exist_ok=True)
    partial = archive.with_name(archive.name + ".partial")
    env = os.environ.copy()
    env.pop("GZIP", None)
    try:
        with partial.open("wb") as output:
            process = subprocess.Popen(
                ["git", "-C", str(repo), "-c", "tar.umask=0002", "archive",
                 "--format=tar", "--prefix=" + PREFIX, COMMIT],
                stdout=subprocess.PIPE,
            )
            assert process.stdout is not None
            try:
                compressed = subprocess.run(
                    ["gzip", "-6", "-n"], stdin=process.stdout, stdout=output, env=env
                )
            finally:
                process.stdout.close()
            export_status = process.wait()
            if export_status or compressed.returncode:
                raise RuntimeError("git archive or gzip failed; the partial archive was rejected")
        partial.replace(archive)
    finally:
        partial.unlink(missing_ok=True)


def verify_content(repo: Path, archive: Path) -> dict:
    expected: dict[str, tuple[str, str]] = {}
    expected_dirs = {""}
    for record in git(repo, "ls-tree", "-r", "-z", "--full-tree", COMMIT).split(b"\0"):
        if not record:
            continue
        meta, raw_name = record.split(b"\t", 1)
        mode, object_type, oid = meta.decode("ascii").split()
        name = raw_name.decode("utf-8", "surrogateescape")
        if object_type != "blob" or mode not in {"100644", "100755", "120000"}:
            raise RuntimeError(f"Unexpected Git object: {name!r} ({mode} {object_type})")
        expected[name] = (mode, oid)
        expected_dirs.update(str(p) for p in PurePosixPath(name).parents if str(p) != ".")

    commit_time = int(git(repo, "show", "-s", "--format=%ct", COMMIT).strip())
    seen: set[str] = set()
    seen_dirs: set[str] = set()
    counts = {"regular_files": 0, "symlinks": 0, "directories": 0, "regular_file_bytes": 0}
    with tarfile.open(archive, mode="r|gz", encoding="utf-8", errors="surrogateescape") as tar:
        for member in tar:
            if not member.name.startswith(PREFIX) and member.name != PREFIX.rstrip("/"):
                raise RuntimeError(f"Archive path is outside its expected prefix: {member.name!r}")
            name = member.name[len(PREFIX):] if member.name.startswith(PREFIX) else ""
            if name.startswith("/") or ".." in PurePosixPath(name).parts:
                raise RuntimeError(f"Unsafe archive path: {name!r}")
            if member.uid != 0 or member.gid != 0 or member.mtime != commit_time:
                raise RuntimeError(f"Unexpected owner/time metadata: {name!r}")
            if member.isdir():
                name = name.rstrip("/")
                if name not in expected_dirs or name in seen_dirs or member.mode != 0o775:
                    raise RuntimeError(f"Unexpected or duplicate directory: {name!r}")
                seen_dirs.add(name)
                counts["directories"] += 1
                continue
            if name not in expected or name in seen:
                raise RuntimeError(f"Unexpected or duplicate archive entry: {name!r}")
            mode, oid = expected[name]
            if mode == "120000":
                if not member.issym() or member.mode != 0o777:
                    raise RuntimeError(f"Expected a Git symlink: {name!r}")
                # Hash the link text itself. Never follow or extract the symlink.
                data = member.linkname.encode("utf-8", "surrogateescape")
                actual = hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()
                counts["symlinks"] += 1
            else:
                wanted_mode = 0o775 if mode == "100755" else 0o664
                if member.type not in {tarfile.REGTYPE, tarfile.AREGTYPE} or member.mode != wanted_mode:
                    raise RuntimeError(f"Unexpected file type/permissions: {name!r}")
                digest = hashlib.sha1(b"blob " + str(member.size).encode() + b"\0")
                reader = tar.extractfile(member)
                if reader is None:
                    raise RuntimeError(f"Cannot read archive entry: {name!r}")
                total = 0
                with reader:
                    while chunk := reader.read(1024 * 1024):
                        digest.update(chunk)
                        total += len(chunk)
                if total != member.size:
                    raise RuntimeError(f"Truncated archive entry: {name!r}")
                actual = digest.hexdigest()
                counts["regular_files"] += 1
                counts["regular_file_bytes"] += total
            if actual != oid:
                raise RuntimeError(f"Archive content differs from the pinned Git blob: {name!r}")
            seen.add(name)
        if tar.pax_headers.get("comment") != COMMIT:
            raise RuntimeError("Archive PAX commit metadata does not match the pinned commit")
    if seen != set(expected) or seen_dirs != expected_dirs:
        raise RuntimeError("Archive file/directory set differs from the pinned Git tree")
    return counts


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, default=Path.cwd())
    parser.add_argument("--repo", type=Path, help="Existing kernel repository; defaults to PROJECT/out/linux")
    parser.add_argument("--archive", type=Path, help="Archive path; defaults to PROJECT/out/downloads/<locked name>")
    parser.add_argument("--report", type=Path, help="JSON verification report")
    parser.add_argument("--verify-only", action="store_true", help="Do not download, generate, or change the manifest")
    args = parser.parse_args()
    start = time.monotonic()
    project = args.project.resolve()
    manifest = project / "manifests/kernel.json"
    lock = json.loads(manifest.read_text())
    if (lock.get("commit"), lock.get("archive"), lock.get("repo")) != (COMMIT, ARCHIVE, REPO_URL):
        raise RuntimeError("This helper is scoped to the audited SY108 kernel lock only")
    if lock.get("sha256") not in APPROVED_HASHES:
        raise RuntimeError("Manifest has an unreviewed archive hash; refusing to overwrite it")
    repo = (args.repo or project / "out/linux").resolve()
    archive = (args.archive or project / "out/downloads" / ARCHIVE).resolve()
    report_path = (args.report or project / "out/kernel-source-verification.json").resolve()
    if args.verify_only and (not repo.exists() or not archive.is_file()):
        raise RuntimeError("--verify-only requires an existing repository and archive")
    prepare_repository(repo)
    if not archive.exists():
        create_archive(repo, archive)
    with archive.open("rb") as source:
        archive_hash = hashlib.file_digest(source, "sha256").hexdigest()
    # A fixed hash check remains mandatory; do not trust a new value merely
    # because it was just computed. Approved hashes are explicit above.
    if archive_hash not in APPROVED_HASHES:
        raise RuntimeError(
            f"Unreviewed archive SHA256: {archive_hash}. Expected {AUDITED_SHA256} "
            f"or the author's original {ORIGINAL_SHA256}; no manifest was changed."
        )
    counts = verify_content(repo, archive)
    report = {
        "status": "passed", "kernel_commit": COMMIT, "kernel_tree": TREE,
        "original_archive_sha256": ORIGINAL_SHA256, "archive_sha256": archive_hash,
        "matches_original_archive_hash": archive_hash == ORIGINAL_SHA256,
        "archive_bytes": archive.stat().st_size, "content_matches_git_tree": True,
        "git_version": subprocess.check_output(["git", "--version"], text=True).strip(),
        "gzip_version": subprocess.check_output(["gzip", "--version"], text=True).splitlines()[0],
        "verify_only": args.verify_only, **counts,
        "elapsed_seconds": round(time.monotonic() - start, 3),
    }
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n")
    if not args.verify_only:
        lock["sha256"] = archive_hash
        manifest.write_text(json.dumps(lock, indent=2) + "\n")
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()

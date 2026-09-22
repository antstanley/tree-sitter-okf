"""Shared code for the provenance locks (spec §9.2, §9.6).

A lock pins an upstream repository at one commit and records the SHA-256 of
every file copied from it:

    {
      "upstream": "https://github.com/...",
      "commit": "<full sha>",
      "files": { "<local path, relative to the lock>": {"from": "<upstream path>", "sha256": "..."} }
    }

`script/revendor` and `script/sync-fixtures` write locks from a fresh clone;
`script/verify-vendor` checks the working tree against them offline.
"""

import hashlib
import json
import os
import shutil
import subprocess
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sha256(path):
    with open(path, "rb") as f:
        return hashlib.sha256(f.read()).hexdigest()


def clone(url, commit):
    """A temporary checkout of `url` at `commit` (the caller removes it)."""
    tmp = tempfile.mkdtemp(prefix="okf-lock-")
    subprocess.run(["git", "clone", "--quiet", url, tmp], check=True)
    subprocess.run(["git", "-C", tmp, "checkout", "--quiet", commit], check=True)
    full = subprocess.run(["git", "-C", tmp, "rev-parse", "HEAD"], check=True,
                          capture_output=True, text=True).stdout.strip()
    return tmp, full


def install(checkout, mapping, dest):
    """Copy `mapping` ({local: upstream}) from `checkout` into `dest` and
    return the lock's `files` table."""
    files = {}
    for local, upstream in sorted(mapping.items()):
        target = os.path.join(dest, local)
        os.makedirs(os.path.dirname(target), exist_ok=True)
        shutil.copyfile(os.path.join(checkout, upstream), target)
        files[local] = {"from": upstream, "sha256": sha256(target)}
    return files


def write(lock_path, url, commit, files):
    with open(lock_path, "w") as f:
        json.dump({"upstream": url, "commit": commit, "files": files}, f, indent=2, sort_keys=True)
        f.write("\n")


def verify(lock_path, tracked=None):
    """Problems with the files under the lock's directory: hash mismatches,
    missing files, and (if `tracked` is given: a predicate on local paths)
    files that the lock does not cover."""
    base = os.path.dirname(lock_path)
    lock = json.load(open(lock_path))
    problems = []
    for local, entry in sorted(lock["files"].items()):
        path = os.path.join(base, local)
        if not os.path.exists(path):
            problems.append(f"missing: {os.path.relpath(path, ROOT)}")
        elif sha256(path) != entry["sha256"]:
            problems.append(f"modified: {os.path.relpath(path, ROOT)}")
    if tracked:
        for dirpath, _, names in os.walk(base):
            for name in names:
                local = os.path.relpath(os.path.join(dirpath, name), base)
                if tracked(local) and local not in lock["files"]:
                    problems.append(f"not in lock: {os.path.relpath(os.path.join(base, local), ROOT)}")
    return lock, problems

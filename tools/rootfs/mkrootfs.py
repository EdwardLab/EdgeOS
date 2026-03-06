#!/usr/bin/env python3
import argparse
import os
import shutil
import subprocess
import tempfile
from pathlib import Path


def run(cmd):
    subprocess.run(cmd, check=True)


def debugfs(image: Path, request: str, check: bool = False):
    p = subprocess.run(["debugfs", "-w", "-R", request, str(image)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if check and p.returncode != 0:
        raise RuntimeError(f"debugfs failed: {request}")


def ensure_dir(image: Path, p: str):
    debugfs(image, f"mkdir {p}")


def write_file(image: Path, src: Path, dst: str):
    debugfs(image, f"unlink {dst}")
    debugfs(image, f"write {src} {dst}", check=True)


def populate_override(image: Path, override_dir: Path):
    if not override_dir.exists():
        return
    for root, dirs, files in os.walk(override_dir):
        rel_root = Path(root).relative_to(override_dir)
        dst_root = Path("/") / rel_root
        ensure_dir(image, str(dst_root))
        for d in dirs:
            ensure_dir(image, str(dst_root / d))
        for f in files:
            src = Path(root) / f
            dst = str(dst_root / f)
            write_file(image, src, dst)


def create_defaults(image: Path, tmpdir: Path):
    for d in ["/bin", "/sbin", "/etc", "/root", "/boot", "/dev", "/mnt", "/lib", "/tmp"]:
        ensure_dir(image, d)

    os_release = tmpdir / "os-release"
    os_release.write_text("NAME=EdgeOS\nVERSION=unknown\n", encoding="ascii")
    write_file(image, os_release, "/etc/os-release")


def main():
    print("[mkrootfs] EdgeOS rootfs image creator (edgeos-mkrootfs tool)")
    print("[mkrootfs] This tool creates a minimal ext4 rootfs image for EdgeOS, and applies override files from the specified directory.")
    print("[mkrootfs] Start creating rootfs image... Please wait...")
    ap = argparse.ArgumentParser(description="Create minimal EdgeOS ext4/ext2 rootfs image")
    ap.add_argument("--output", required=True, help="Output filesystem image path")
    ap.add_argument("--size-mb", type=int, default=64, help="Filesystem size in MB")
    ap.add_argument("--fs", choices=["ext4", "ext2"], default="ext4", help="Filesystem type (default: ext4)")
    ap.add_argument("--override-dir", default="tools/rootfs/rootfs_override", help="Directory with override files")
    ap.add_argument("--apply-override-only", action="store_true", help="Do not recreate filesystem, only apply override files")
    args = ap.parse_args()

    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)

    if shutil.which("mke2fs") is None:
        raise SystemExit("Error: mke2fs not found")
    if shutil.which("debugfs") is None:
        raise SystemExit("Error: debugfs not found")

    with tempfile.TemporaryDirectory(prefix="edgeos-rootfs-") as t:
        tmpdir = Path(t)
        if not args.apply_override_only:
            with open(out, "wb") as f:
                f.truncate(args.size_mb * 1024 * 1024)
            if args.fs == "ext4":
                # Minimal ext4 profile compatible with EdgeOS kernel subset:
                # keep extents enabled, disable advanced features not supported yet.
                run([
                    "mke2fs", "-q", "-F", "-t", "ext4",
                    "-O", "extent,filetype,sparse_super,large_file,^has_journal,^64bit,^metadata_csum,^metadata_csum_seed,^bigalloc,^encrypt,^quota,^verity,^casefold,^inline_data,^ea_inode,^extra_isize,^huge_file,^orphan_file,^flex_bg,^dir_index",
                    str(out)
                ])
            else:
                run(["mke2fs", "-q", "-F", "-t", "ext2", str(out)])
            create_defaults(out, tmpdir)
        populate_override(out, Path(args.override_dir))
    print(f"[mkrootfs] Rootfs image created at: {out}")


if __name__ == "__main__":
    main()

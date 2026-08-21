#!/usr/bin/env bash
# Generates the M1 acceptance dataset (development plan §3.5) into DEST_DIR:
#
#   tree/        varied content: sizes around XDR/read-chunk boundaries, nested dirs,
#                unicode + space names, relative/absolute/dangling symlinks
#   bigdir/      BIGDIR_COUNT (default 100000) empty files f000000.. — readdir gate
#   cthon/       pre-staged tree for the read-only cthon subset (bigfile for test5b)
#   manifest.md5 md5 manifest of tree/ (relative paths) for `md5sum -c` on the mount
set -euo pipefail

dest=${1:?usage: gen_dataset.sh DEST_DIR [BIGDIR_COUNT]}
count=${2:-100000}

mkdir -p "$dest"
dest=$(realpath "$dest")
rm -rf "$dest/tree" "$dest/bigdir" "$dest/cthon" "$dest/manifest.md5"
mkdir -p "$dest/tree/sub1/sub2/sub3" "$dest/tree/unicode" "$dest/bigdir" "$dest/cthon"

# Sizes straddling 4K page, 64K chunk and 1M boundaries, plus 0/1 and an 8 MiB file.
for s in 0 1 511 4095 4096 4097 65535 65536 65537 1048579 8388608; do
  head -c "$s" /dev/urandom > "$dest/tree/f_${s}.bin"
done
printf 'hello lightnfs\n' > "$dest/tree/sub1/hello.txt"
head -c 123456 /dev/urandom > "$dest/tree/sub1/sub2/sub3/deep.bin"
head -c 2048 /dev/urandom > "$dest/tree/unicode/文件-测试 空格.txt"
ln -sfn ../f_4096.bin "$dest/tree/sub1/link_rel"
ln -sfn /nonexistent/target "$dest/tree/link_dangling"
ln -sfn sub1/hello.txt "$dest/tree/link_hello"

# 100k-entry flat directory (empty files: the gate is readdir correctness, not I/O).
seq 0 $((count - 1)) | awk '{printf "f%06d\n", $1}' | \
  (cd "$dest/bigdir" && xargs -n 5000 touch)

# cthon read-only staging: test5b reads NFSTESTDIR/bigfile (default 1 MiB).
head -c 1048576 /dev/urandom > "$dest/cthon/bigfile"

(cd "$dest" && find tree -type f -print0 | sort -z | xargs -0 md5sum > manifest.md5)

echo "dataset ready at $dest (bigdir: $count entries)"

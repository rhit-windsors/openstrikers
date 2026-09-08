#!/usr/bin/env bash
set -e

ROOT="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
EXTERN_DIR="$ROOT/extern"
cd "$EXTERN_DIR"

for repo in decomp decomp/extern/musyx aurora; do
  dirty="$(git -C "$repo" status --porcelain)"
  # A fresh clone on Windows reports include/dolphin/GX.h and VI.h as modified.
  # They are symlinks to gx.h and vi.h, which collide with their own targets on
  # a case-insensitive filesystem, so git checks out the target's content and
  # then sees the symlink as changed. It is not an unsaved edit.
  if [ "$repo" = decomp ]; then
    dirty="$(printf '%s\n' "$dirty" | grep -v ' include/dolphin/GX\.h$' | grep -v ' include/dolphin/VI\.h$' || true)"
  fi
  dirty="$(printf '%s' "$dirty" | sed '/^$/d')"
  if [ -n "$dirty" ]; then
    echo "WARNING: $repo has edits that aren't saved as a patch. Aborting...."
    printf '%s\n' "$dirty" | sed 's/^/    /'
    echo "Generate the missing patch with: git -C $repo diff > patches/.../name.patch"
    exit 1
  fi
done

echo "== reset decomp at clean HEAD =="
git -C decomp checkout -- .
git -C decomp clean -fd

echo "== reset musyx at clean HEAD =="
git -C decomp/extern/musyx checkout -- .
git -C decomp/extern/musyx clean -fd

echo "== reset aurora at clean HEAD =="
git -C aurora checkout -- .
git -C aurora clean -fd

echo "== applying decomp patches =="
for p in "$ROOT"/patches/decomp/*.patch; do
  [ -e "$p" ] || continue
  echo "-> $p"
  git -C decomp apply --check "$p" || { echo "FAILED: $p"; exit 1; }
  git -C decomp apply "$p"
done

echo "== applying musyx patches =="
for p in "$ROOT"/patches/musyx/*.patch; do
  [ -e "$p" ] || continue
  echo "-> $p"
  git -C decomp/extern/musyx apply --check "$p" || { echo "FAILED: $p"; exit 1; }
  git -C decomp/extern/musyx apply "$p"
done

echo "== applying aurora patches =="
for p in "$ROOT"/patches/aurora/*.patch; do
  [ -e "$p" ] || continue
  echo "-> $p"
  git -C aurora apply --check "$p" || { echo "FAILED: $p"; exit 1; }
  git -C aurora apply "$p"
done

echo "== applying temporary decomp patches =="
for p in "$ROOT"/patches/tmp/*.patch; do
  [ -e "$p" ] || continue
  echo "-> $p"
  git -C decomp apply --check "$p" || { echo "FAILED: $p"; exit 1; }
  git -C decomp apply "$p"
done

# Applied after patches/tmp, not with patches/decomp. A patch saved by the
# workflow in README.md is generated against a tree that already has the tmp
# workarounds applied, so any decomp fix touching a file that a tmp patch also
# touches cannot apply back in the decomp stage -- it needs context that does
# not exist yet at that point. Patches that depend on the tmp stage live here.
echo "== applying late decomp patches =="
for p in "$ROOT"/patches/decomp-late/*.patch; do
  [ -e "$p" ] || continue
  echo "-> $p"
  git -C decomp apply --check "$p" || { echo "FAILED: $p"; exit 1; }
  git -C decomp apply "$p"
done

echo "all patches applied successfully"

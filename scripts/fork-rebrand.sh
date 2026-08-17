#!/usr/bin/env bash
#
# fork-rebrand.sh — apply or remove this fork's identity layer.
#
# The fork renames the upstream project owner to "hodizoda". Carrying that
# rename as a git commit makes every upstream resync conflict in ~30 files
# that hold no functionality, only the name. So the rename is not carried as
# a commit to be merged: it is regenerated from scratch after each resync.
#
# Resync workflow:
#   git checkout -B sync/upstream-<date> upstream/main
#   git cherry-pick <feature commits>          # real code, real conflicts only
#   scripts/fork-rebrand.sh --apply
#   git commit -am "chore: rebrand fork identity to hodizoda"
#
# Usage:
#   fork-rebrand.sh --apply           rewrite the tree to fork identity
#   fork-rebrand.sh --revert [ref]    restore upstream identity (default upstream/main)
#   fork-rebrand.sh --check           exit 0 if fully rebranded, 1 otherwise
#   fork-rebrand.sh --self-test       prove --apply then --revert is a no-op vs upstream
#
# Runs on bash 3.2 (macOS system bash). Uses perl rather than sed -i for
# BSD/GNU portability.

set -euo pipefail

UPSTREAM_NAME_CAMEL='DeusData'
UPSTREAM_NAME_LOWER='deusdata'
FORK_NAME='hodizoda'

UPSTREAM_COPYRIGHT='Copyright (c) 2025 DeusData'
FORK_COPYRIGHT='Copyright (c) 2026 Shidfar Hodizoda'

# Paths deliberately left carrying the upstream name:
#   LICENSE                  upstream's copyright notice must survive verbatim
#   tests/repro/repro_*.c    upstream issue URLs and parser fixture data
is_carve_out() {
    case "$1" in
        LICENSE) return 0 ;;
        tests/repro/repro_issue*.c) return 0 ;;
        tests/repro/repro_grammar_build.c) return 0 ;;
        scripts/fork-rebrand.sh) return 0 ;;
        *) return 1 ;;
    esac
}

repo_root() { git rev-parse --show-toplevel; }

die() { printf 'fork-rebrand: %s\n' "$1" >&2; exit 1; }

# Scratch files are script-scope so the EXIT trap can see them under `set -u`.
_TMP_REF=''
_TMP_BRAND=''
_TMP_WORKTREE=''
cleanup() {
    [ -n "$_TMP_REF" ] && rm -f "$_TMP_REF"
    [ -n "$_TMP_BRAND" ] && rm -f "$_TMP_BRAND"
    if [ -n "$_TMP_WORKTREE" ]; then
        git worktree remove --force "$_TMP_WORKTREE" >/dev/null 2>&1 || rm -rf "$_TMP_WORKTREE"
    fi
    return 0
}
trap cleanup EXIT

# Tracked, non-binary files that are not carve-outs.
candidate_files() {
    git ls-files -z | while IFS= read -r -d '' f; do
        is_carve_out "$f" && continue
        [ -f "$f" ] || continue
        grep -Iq . "$f" 2>/dev/null || continue
        printf '%s\n' "$f"
    done
}

subst_stream() {
    perl -pe "s/\Q$UPSTREAM_NAME_CAMEL\E/$FORK_NAME/g; s/\Q$UPSTREAM_NAME_LOWER\E/$FORK_NAME/g"
}

# ---------------------------------------------------------------- apply

apply_content() {
    local changed=0 f
    while IFS= read -r f; do
        if grep -q -e "$UPSTREAM_NAME_CAMEL" -e "$UPSTREAM_NAME_LOWER" "$f"; then
            perl -pi -e "s/\Q$UPSTREAM_NAME_CAMEL\E/$FORK_NAME/g; s/\Q$UPSTREAM_NAME_LOWER\E/$FORK_NAME/g" "$f"
            changed=$((changed + 1))
        fi
    done < <(candidate_files)
    printf '%s' "$changed"
}

# winget lays manifests out as manifests/<first-letter>/<Publisher>/...
# and names files <Publisher>.<Package>.*, so the publisher appears in paths.
apply_winget_paths() {
    local moved=0 src dst dir
    dir="pkg/winget/manifests"
    [ -d "$dir" ] || { printf '0'; return; }
    for src in "$dir"/*/"$UPSTREAM_NAME_CAMEL" "$dir"/*/"$UPSTREAM_NAME_LOWER"; do
        [ -d "$src" ] || continue
        dst="$dir/$(printf '%s' "$FORK_NAME" | cut -c1)/$FORK_NAME"
        mkdir -p "$(dirname "$dst")"
        git mv "$src" "$dst"
        rmdir "$(dirname "$src")" 2>/dev/null || true
        moved=$((moved + 1))
    done
    # Filenames embed the publisher too.
    while IFS= read -r src; do
        dst=$(printf '%s' "$src" | perl -pe "s{([^/]*)\Q$UPSTREAM_NAME_CAMEL\E}{\${1}$FORK_NAME}g; s{([^/]*)\Q$UPSTREAM_NAME_LOWER\E}{\${1}$FORK_NAME}g")
        [ "$src" = "$dst" ] && continue
        git mv "$src" "$dst"
        moved=$((moved + 1))
    done < <(git ls-files "$dir" | grep -E "$UPSTREAM_NAME_CAMEL|$UPSTREAM_NAME_LOWER" || true)
    printf '%s' "$moved"
}

# Upstream's notice stays; ours is added directly beneath it, same indent.
apply_license() {
    [ -f LICENSE ] || { printf 'absent'; return; }
    if grep -qF "$FORK_COPYRIGHT" LICENSE; then printf 'already'; return; fi
    grep -qF "$UPSTREAM_COPYRIGHT" LICENSE \
        || die "LICENSE has no '$UPSTREAM_COPYRIGHT' line to anchor to (upstream changed it?)"
    perl -i -pe "if (/\Q$UPSTREAM_COPYRIGHT\E/ && !\$done) {
                     my (\$indent) = /^(\s*)/;
                     \$_ .= \$indent . q{$FORK_COPYRIGHT} . qq{\n};
                     \$done = 1;
                 }" LICENSE
    printf 'added'
}

do_apply() {
    cd "$(repo_root)"
    local n_files n_moved lic
    n_files=$(apply_content)
    n_moved=$(apply_winget_paths)
    lic=$(apply_license)
    printf 'applied: %s files rewritten, %s paths renamed, LICENSE %s\n' "$n_files" "$n_moved" "$lic"
    do_check || die "tree still carries upstream identity after --apply (see above)"
}

# ---------------------------------------------------------------- revert

# A file is safe to restore from <ref> only if rebranding the ref version
# reproduces the working version exactly. Anything else carries real changes
# on top of the rename, and is reported rather than silently overwritten.
do_revert() {
    cd "$(repo_root)"
    local ref="${1:-upstream/main}"
    git rev-parse --verify --quiet "$ref^{commit}" >/dev/null \
        || die "no such ref: $ref"

    local restored=0 skipped=0 f
    _TMP_REF=$(mktemp); _TMP_BRAND=$(mktemp)

    # Paths first: a file still sitting at its fork path has no counterpart in
    # <ref>, so the content pass below would skip it as fork-added and leave
    # fork content behind at an upstream path.
    revert_winget_paths "$ref"

    while IFS= read -r f; do
        grep -q "$FORK_NAME" "$f" || continue
        if ! git cat-file -e "$ref:$f" 2>/dev/null; then
            continue    # fork-added file, has no upstream version
        fi
        git show "$ref:$f" > "$_TMP_REF"
        subst_stream < "$_TMP_REF" > "$_TMP_BRAND"
        if cmp -s "$_TMP_BRAND" "$f"; then
            cp "$_TMP_REF" "$f"
            restored=$((restored + 1))
        else
            printf '  skipped (has non-brand changes): %s\n' "$f" >&2
            skipped=$((skipped + 1))
        fi
    done < <(candidate_files)

    revert_license
    printf 'reverted: %s files restored from %s, %s skipped\n' "$restored" "$ref" "$skipped"
}

revert_winget_paths() {
    local ref="$1" dir src dst up_dir
    dir="pkg/winget/manifests"
    src="$dir/$(printf '%s' "$FORK_NAME" | cut -c1)/$FORK_NAME"
    [ -d "$src" ] || return 0
    up_dir=$(git ls-tree -d --name-only -r "$ref" -- "$dir" 2>/dev/null \
             | grep -E "/$UPSTREAM_NAME_CAMEL$|/$UPSTREAM_NAME_LOWER$" | head -1 || true)
    [ -n "$up_dir" ] || return 0
    mkdir -p "$(dirname "$up_dir")"
    git mv "$src" "$up_dir"
    rmdir "$(dirname "$src")" 2>/dev/null || true
    while IFS= read -r f; do
        dst=$(printf '%s' "$f" | perl -pe "s{([^/]*)\Q$FORK_NAME\E}{\${1}$UPSTREAM_NAME_CAMEL}g")
        [ "$f" = "$dst" ] && continue
        git mv "$f" "$dst"
    done < <(git ls-files "$up_dir" | grep -F "$FORK_NAME" || true)
}

revert_license() {
    [ -f LICENSE ] || return 0
    grep -qF "$FORK_COPYRIGHT" LICENSE || return 0
    perl -i -ne "print unless /\Q$FORK_COPYRIGHT\E/" LICENSE
}

# ---------------------------------------------------------------- check

do_check() {
    cd "$(repo_root)"
    local bad=0 f

    while IFS= read -r f; do
        if grep -qiE "$UPSTREAM_NAME_LOWER" "$f"; then
            printf '  residual upstream name in content: %s\n' "$f" >&2
            bad=1
        fi
    done < <(candidate_files)

    while IFS= read -r f; do
        printf '  residual upstream name in path: %s\n' "$f" >&2
        bad=1
    done < <(git ls-files | grep -iE "$UPSTREAM_NAME_LOWER" || true)

    if [ -f LICENSE ] && ! grep -qF "$FORK_COPYRIGHT" LICENSE; then
        printf '  LICENSE missing fork copyright line\n' >&2
        bad=1
    fi

    [ "$bad" -eq 0 ] || return 1
    printf 'check: tree is fully rebranded to %s\n' "$FORK_NAME"
}

# ---------------------------------------------------------------- self-test

# Round-trip proof. If upstream ever introduces a spelling this script does
# not model, apply→revert stops being identity and this fails loudly.
do_self_test() {
    local ref="${1:-upstream/main}" self root base_tree after_tree n
    root=$(repo_root)
    self="$root/scripts/fork-rebrand.sh"
    git -C "$root" rev-parse --verify --quiet "$ref^{commit}" >/dev/null \
        || die "no such ref: $ref"

    # Runs in a throwaway worktree so the caller's tree is never touched.
    _TMP_WORKTREE=$(mktemp -d)
    rmdir "$_TMP_WORKTREE"
    git -C "$root" worktree add --quiet --detach "$_TMP_WORKTREE" "$ref"
    cp "$self" "$_TMP_WORKTREE/scripts/fork-rebrand.sh"

    base_tree=$(git -C "$_TMP_WORKTREE" rev-parse "$ref^{tree}")
    ( cd "$_TMP_WORKTREE" && ./scripts/fork-rebrand.sh --apply ) >/dev/null
    ( cd "$_TMP_WORKTREE" && ./scripts/fork-rebrand.sh --revert "$ref" ) >/dev/null
    git -C "$_TMP_WORKTREE" add -A >/dev/null
    after_tree=$(git -C "$_TMP_WORKTREE" write-tree)

    # The copied-in script is the one expected difference. `grep -v` exits 1
    # when it filters everything out, which under pipefail is the pass case.
    n=$(git -C "$_TMP_WORKTREE" diff --name-only "$base_tree" "$after_tree" \
        | { grep -v '^scripts/fork-rebrand\.sh$' || true; } | wc -l | tr -d ' ')
    if [ "$n" = "0" ]; then
        printf 'self-test: PASS — apply then revert is identity against %s\n' "$ref"
    else
        printf 'self-test: FAIL — round-trip does not restore %s (%s files differ)\n' "$ref" "$n" >&2
        git -C "$_TMP_WORKTREE" diff --stat "$base_tree" "$after_tree" >&2
        return 1
    fi
}

# ---------------------------------------------------------------- main

case "${1:---help}" in
    --apply)     do_apply ;;
    --revert)    do_revert "${2:-upstream/main}" ;;
    --check)     do_check ;;
    --self-test) do_self_test "${2:-upstream/main}" ;;
    *)
        sed -n '3,30p' "$0" | sed 's/^# \{0,1\}//'
        exit 1
        ;;
esac

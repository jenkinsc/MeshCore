#!/usr/bin/env bash
# Local test of sync-upstream.sh against a throw-away fake "upstream". No network needed.
set -euo pipefail
SCRIPT="$(cd "$(dirname "$0")" && pwd)/sync-upstream.sh"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
export GIT_AUTHOR_NAME=t GIT_AUTHOR_EMAIL=t@t GIT_COMMITTER_NAME=t GIT_COMMITTER_EMAIL=t@t
G="git -c init.defaultBranch=main"
fail() { echo "FAIL: $*"; exit 1; }
run() { ( cd "$W/fork" && UPSTREAM_URL="$W/upstream.git" "$@" ) ; }

# --- fake upstream with a release tag
$G init -q "$W/seed"; cd "$W/seed"
mkdir -p .github/workflows src
echo "name: upstream-release" > .github/workflows/release.yml
printf 'line1\nline2\nline3\n' > src/shared.txt
echo a > src/a.txt
git add -A; git commit -qm base; git tag repeater-v1.9.0
git clone -q --bare "$W/seed" "$W/upstream.git"

# --- our fork: base + our own workflow + a feature edit in a different file
git clone -q "$W/upstream.git" "$W/fork"; cd "$W/fork"
git remote rename origin fork-origin
echo "name: ours" > .github/workflows/ours.yml
echo feature > src/feature.txt
git add -A; git commit -qm "our feature"
OURS_WF="$(cat .github/workflows/release.yml)"

# 1. fork already contains newest tag
out="$(run "$SCRIPT")"; echo "$out" | grep -q "status=up-to-date" || fail "expected up-to-date: $out"
echo "ok 1 up-to-date"

# 2. upstream releases 1.10.0 (version sort must prefer it over 1.9.0), touches a workflow, adds a workflow
cd "$W/seed"
echo "name: CHANGED-by-upstream" > .github/workflows/release.yml
echo "name: new-upstream-workflow" > .github/workflows/new.yml
echo b > src/b.txt
git add -A; git commit -qm "upstream 1.10"; git tag repeater-v1.10.0
git push -q "$W/upstream.git" main --tags
out="$(run "$SCRIPT")"; echo "$out" | grep -q "status=merged" || fail "expected merged: $out"
echo "$out" | grep -q "ref=repeater-v1.10.0" || fail "expected 1.10.0 chosen over 1.9.0: $out"
cd "$W/fork"
[ -f src/b.txt ] || fail "upstream change missing"
[ -f src/feature.txt ] || fail "our feature lost"
[ "$(cat .github/workflows/release.yml)" = "$OURS_WF" ] || fail "upstream workflow change was taken"
[ ! -e .github/workflows/new.yml ] || fail "upstream workflow file was added"
[ -f .github/workflows/ours.yml ] || fail "our workflow lost"
[ "$(git rev-list --parents -n1 HEAD | wc -w)" = 3 ] || fail "HEAD is not a merge commit"
git diff --quiet && git diff --cached --quiet || fail "tree not clean after merge"
echo "ok 2 merged, workflows protected, version sort"

# 3. running again is a no-op
out="$(run "$SCRIPT")"; echo "$out" | grep -q "status=up-to-date" || fail "expected up-to-date after merge: $out"
echo "ok 3 idempotent"

# 4. conflict: both sides change the same line; fork keeps its own state, conflicts listed
cd "$W/fork"; sed -i 's/line2/line2-OURS/' src/shared.txt; git commit -qam "ours edits shared"
cd "$W/seed"; sed -i 's/line2/line2-UPSTREAM/' src/shared.txt; git commit -qam "upstream edits shared"; git tag repeater-v1.11.0
git push -q "$W/upstream.git" main --tags
set +e; out="$(run "$SCRIPT")"; rc=$?; set -e
[ "$rc" = 3 ] || fail "expected exit 3, got $rc: $out"
echo "$out" | grep -q "status=conflict" || fail "expected conflict: $out"
cd "$W/fork"
grep -q "src/shared.txt" sync-conflicts.txt || fail "conflict file not listed"
rm -f sync-conflicts.txt
git diff --quiet && git diff --cached --quiet || fail "tree not restored after conflict"
[ ! -f .git/MERGE_HEAD ] || fail "merge left in progress"
grep -q "line2-OURS" src/shared.txt || fail "our content lost"
echo "ok 4 conflict reported and aborted cleanly"

# 5. no matching tag pattern, and explicit ref
set +e; out="$(run env TAG_PATTERN='nothing-*' "$SCRIPT")"; rc=$?; set -e
[ "$rc" = 4 ] && echo "$out" | grep -q "status=no-ref" || fail "expected no-ref/4: rc=$rc $out"
echo "ok 5 no matching release"

# 6. dirty tree is refused
cd "$W/fork"; echo x >> src/a.txt
set +e; run "$SCRIPT" >/dev/null 2>&1; rc=$?; set -e
[ "$rc" = 1 ] || fail "dirty tree should be refused, rc=$rc"
git checkout -q src/a.txt
echo "ok 6 dirty tree refused"
echo ALL PASSED

#!/usr/bin/env bash
# Merge the newest upstream MeshCore release (or a chosen ref) into the current branch, WITHOUT pushing.
# Used by .github/workflows/sync-upstream.yml; also safe to run by hand in a clean clone.
#
# Environment:
#   UPSTREAM_URL   default https://github.com/meshcore-dev/MeshCore.git
#   TAG_PATTERN    default 'repeater-v*'   (which upstream release tags count as "new firmware")
#   SYNC_REF       optional explicit tag/branch (e.g. upstream/main or repeater-v1.19.0); overrides TAG_PATTERN
# Output: key=value lines on stdout and, if set, appended to $GITHUB_OUTPUT:
#   status=up-to-date | merged | conflict | no-ref        ref=<what was merged>
# Exit code: 0 for up-to-date/merged, 3 for conflict, 4 for no matching ref.
set -euo pipefail

UPSTREAM_URL="${UPSTREAM_URL:-https://github.com/meshcore-dev/MeshCore.git}"
TAG_PATTERN="${TAG_PATTERN:-repeater-v*}"
SYNC_REF="${SYNC_REF:-}"

emit() { echo "$1=$2"; [ -n "${GITHUB_OUTPUT:-}" ] && echo "$1=$2" >> "$GITHUB_OUTPUT"; return 0; }

if ! git diff --quiet || ! git diff --cached --quiet; then
  echo "working tree is not clean; refusing to merge" >&2; exit 1
fi

git remote get-url upstream >/dev/null 2>&1 || git remote add upstream "$UPSTREAM_URL"
git fetch upstream --tags --prune --force

if [ -n "$SYNC_REF" ]; then
  REF="$SYNC_REF"
  git rev-parse --verify --quiet "$REF^{commit}" >/dev/null || { emit status no-ref; emit ref "$REF"; exit 4; }
else
  REF="$(git tag -l "$TAG_PATTERN" --sort=-v:refname | head -n 1)"
  [ -n "$REF" ] || { emit status no-ref; emit ref "$TAG_PATTERN"; exit 4; }
fi
emit ref "$REF"

if git merge-base --is-ancestor "$REF" HEAD; then
  echo "already contains $REF"
  emit status up-to-date
  exit 0
fi

echo "merging $REF"
if ! git merge --no-commit --no-ff "$REF" >/tmp/sync-merge.log 2>&1; then
  git diff --name-only --diff-filter=U > sync-conflicts.txt || true
  cat /tmp/sync-merge.log
  git merge --abort || true
  emit status conflict
  exit 3
fi

# Never take upstream's workflow changes: the default GITHUB_TOKEN cannot push workflow edits, and upstream's
# release workflows must not start running on this fork. Our own workflows win.
git reset -q HEAD -- .github/workflows
git checkout -- .github/workflows 2>/dev/null || true
git clean -fdq -- .github/workflows

git -c user.name="${GIT_AUTHOR_NAME:-rak9154-sync}" -c user.email="${GIT_AUTHOR_EMAIL:-rak9154-sync@users.noreply.github.com}" \
    commit -q -m "Merge upstream $REF into the RAK9154 fork

Automated by .github/workflows/sync-upstream.yml. Upstream workflow changes are intentionally not taken."
emit status merged

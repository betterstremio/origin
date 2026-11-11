#!/bin/bash
# A release in two halves:
#   tools/release.sh build X.Y.Z               Linux or WSL: every package from empty folders, into dist/X.Y.Z/
#   tools/release.sh publish X.Y.Z NOTES.md    where git and gh are signed in: the checks, main pushed, one GitHub release
#                                              with every package as its own file
# Each platform's updater picks its own file from the release (src/platform/<target>/target.h, SB_PKG_KIND).
# GH_AS=<account>: gh switches to that account for the push and the release, and back to the one it was on afterwards.
# Publish only when the maintainer says so.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/paths.sh"
CMD="${1:-}"; V="${2:-}"
SLUG="${REPO_SLUG:-betterstremio/origin}"
AUTHOR="${RELEASE_AUTHOR:-Better Stremio <better-stremio@users.noreply.github.com>}"
D="$ROOT/dist/$V"
die() { echo "STOPPED: $*"; exit 1; }
[[ "$V" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "usage: tools/release.sh build|publish X.Y.Z [NOTES.md]"
g() {   # git for this checkout, also from WSL when the worktree's link is a Windows path
  if git -C "$ROOT" rev-parse >/dev/null 2>&1; then git -C "$ROOT" "$@"
  else git --git-dir="$(sed -n 's/^gitdir: //p' "$ROOT/.git" | sed -E 's#^([A-Za-z]):#/mnt/\L\1#')" --work-tree="$ROOT" "$@"; fi
}
FILES=("${APPID}_${V}_arm.ipk" "${APPID}_${V}_arm.ipk.sha256" "${APPID}_${V}_android-arm64.apk" "${APPID}_${V}_android-armv7.apk" "${APPID}_${V}_android.apk"
       "${APPID}_${V}_windows_setup.exe" "${APPID}_${V}_windows_setup.exe.sha256" "${APPID}_${V}_windows_portable.zip")

case "$CMD" in
build)
  [ -z "$(g status --porcelain)" ] || die "uncommitted changes"
  [ "$(cat "$ROOT/VERSION")" = "$V" ] || die "VERSION says $(cat "$ROOT/VERSION"): commit the new version first"
  grep -q '^NUVIO_SUPABASE_URL=.' "$KEYS" && grep -q '^NUVIO_SUPABASE_ANON_KEY=.' "$KEYS" || die "no Nuvio settings in $KEYS: the packages would have no Nuvio sign-in"
  [ -f "$ORIGIN_BUILD/keys/android-release.jks" ] || die "no Android release key: a new one would break every update"
  [ -d "$ORIGIN_BUILD/official/data" ] || die "no $ORIGIN_BUILD/official: the IPK takes its P2P and DTS libraries from it"
  grep -q "\"version\": \"$V\"" "$ROOT/assets/whatsnew.json" || echo "note: assets/whatsnew.json is not $V, so no What's new card shows for this version"
  rm -rf "$D"; mkdir -p "$D"
  OUT="$D" bash "$ROOT/packaging/webos/pack.sh" --clean
  bash "$ROOT/tools/android/build-apk.sh" --clean
  for k in android-arm64 android-armv7 android; do cp "$BUILD_ROOT/android/apk/${APPID}_${V}_$k.apk" "$D/"; done
  OUT="$D" bash "$ROOT/packaging/windows/pack.sh" --clean   # (setup, its .sha256, the portable zip)
  want="$(tr -d '[:space:]' < "$ROOT/packaging/android/release-cert.sha256")"
  for f in "$D"/*.apk; do
    got="$("${ANDROID_HOME:-$HOME/android-sdk}/build-tools/35.0.0/apksigner" verify --print-certs "$f" | grep -m1 'SHA-256' | sed 's/.*: //')"
    [ "$got" = "$want" ] || die "$(basename "$f") is signed with another certificate: updates would not install over it"
  done
  for f in "${FILES[@]}"; do [ -f "$D/$f" ] || die "missing $f"; done
  g rev-parse HEAD > "$D/COMMIT"
  ls -l "$D" | awk 'NR>1 {printf "  %-48s %6.1f MB\n", $NF, $5/1048576}'
  echo "built $V from $(cut -c1-7 "$D/COMMIT"): check them, then tools/release.sh publish $V NOTES.md"
  ;;
publish)
  NOTES="${3:-}"; [ -f "$NOTES" ] || die "no notes file"
  for f in "${FILES[@]}"; do [ -f "$D/$f" ] || die "missing dist/$V/$f: run build first"; done
  [ "$(cat "$D/COMMIT" 2>/dev/null)" = "$(g rev-parse HEAD)" ] || die "dist/$V was not built from this commit: run build again"
  [ -z "$(g status --porcelain)" ] || die "uncommitted changes"
  [ "$(sha256sum "$D/${FILES[0]}" | cut -c1-64)" = "$(cut -c1-64 "$D/${FILES[1]}")" ] || die "the IPK does not match its .sha256"
  [ "$(sha256sum "$D/${FILES[5]}" | cut -c1-64)" = "$(cut -c1-64 "$D/${FILES[6]}")" ] || die "the Windows setup does not match its .sha256"
  # unsigned Windows files: Defender's machine-learning rules once took a build for a trojan, so nothing goes out
  # that Defender flags (rebuild, or report the false positive to Microsoft first)
  case "$(uname -s)" in MINGW*|MSYS*) bash "$ROOT/tools/windows/test/avscan.sh" "$D/${FILES[5]}" "$D/${FILES[7]}" || die "Defender flags a Windows file";;
    *) die "publish from Git Bash on Windows: the Windows files need a Defender scan first";; esac
  g fetch -q "https://github.com/$SLUG.git" main
  g merge-base --is-ancestor FETCH_HEAD HEAD || die "main on GitHub has commits that are not in this branch: merge them first"
  g log FETCH_HEAD..HEAD --format='%an <%ae>%n%cn <%ce>' | grep -qvxF "$AUTHOR" && die "a commit is not by $AUTHOR"
  g log FETCH_HEAD..HEAD --format=%B | grep -qiE 'co-authored|claude' && die "a commit message has an attribution line"
  was="$(gh api user --jq .login)"
  trap 'gh auth switch -u "$was" >/dev/null 2>&1; echo "gh account: $(gh api user --jq .login)"' EXIT
  if [ -n "${GH_AS:-}" ]; then gh auth switch -u "$GH_AS" >/dev/null 2>&1; [ "$(gh api user --jq .login)" = "$GH_AS" ] || die "could not switch gh to $GH_AS"; fi
  g -c credential.helper= -c credential.helper='!gh auth git-credential' push "https://github.com/$SLUG.git" HEAD:refs/heads/main
  paths=(); for f in "${FILES[@]}"; do paths+=("$D/$f"); done
  gh release create "v$V" --repo "$SLUG" --target "$(g rev-parse HEAD)" --latest --title "Origin $V" --notes-file "$NOTES" "${paths[@]}"
  for f in "${FILES[@]}"; do
    d="$(gh api "repos/$SLUG/releases/tags/v$V" --jq ".assets[] | select(.name == \"$f\") | .digest")"
    [ "$d" = "sha256:$(sha256sum "$D/$f" | cut -c1-64)" ] && echo "ok $f" || echo "DIFFERS: $f ($d)"
  done
  # the webOS manifest and Homebrew listing on the website: the app checks it first, so a release without it is not seen
  # by TVs whose GitHub API allowance is used up. SITE=<clone of betterstremio/betterstremio.github.io>
  SITE="${SITE:-$ROOT/../site-publish}"
  if [ -d "$SITE/.git" ]; then
    bash "$ROOT/tools/webos-feed.sh" "$V" "$NOTES" "$SITE"
    git -C "$SITE" add webos
    git -C "$SITE" -c user.name="${AUTHOR% <*}" -c user.email="$(sed 's/.*<\(.*\)>/\1/' <<<"$AUTHOR")" commit -q -m "webOS listing $V" || true
    git -C "$SITE" -c credential.helper= -c credential.helper='!gh auth git-credential' push -q "https://github.com/${SITE_SLUG:-betterstremio/betterstremio.github.io}.git" HEAD:main && echo "site: webOS listing $V pushed"
  else echo "NOTE: no site clone at $SITE: run tools/webos-feed.sh $V $NOTES <site> and push it, or TVs fall back to GitHub's API"; fi
  ;;
*) die "usage: tools/release.sh build|publish X.Y.Z [NOTES.md]";;
esac

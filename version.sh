#!/bin/sh

export LC_ALL=C

version_h="version.h"
print=yes

for ac_option do
  ac_arg=$(echo $ac_option | cut -d '=' -f 2-)
  case "$ac_option" in
  --extra=*)
    extra="-$ac_arg"
    ;;
  --versionh=*)
    version_h="$(pwd)/$ac_arg"
    print=no
    ;;
  --cwd=*)
    cwd="$ac_arg"
    ;;
  *)
    echo "Unknown parameter: $ac_option" >&2
    exit 1
    ;;

  esac
done

if test "$cwd" ; then
  cd "$cwd"
fi

version="$(git describe --match "v[0-9]*" --always --tags | sed 's/^v//')"

VERSION="${version}${extra}-dancingmirrors"

if test "$print" = yes ; then
    echo "$VERSION"
    exit 0
fi

NEW_REVISION="#define VERSION \"${VERSION}\""
OLD_REVISION=$(head -n 1 "$version_h" 2> /dev/null)
MPVCOPYRIGHT="#define MPVCOPYRIGHT \"Copyright © 2000-2025 mpv/MPlayer/mplayer2 projects\""

# Update version.h only on revision changes to avoid spurious rebuilds
if test "$NEW_REVISION" != "$OLD_REVISION"; then
    cat <<EOF > "$version_h"
$NEW_REVISION
$MPVCOPYRIGHT
EOF
fi

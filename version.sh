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
    case "$ac_arg" in
      /*)
        version_h="$ac_arg"
        ;;
      *)
        version_h="$(pwd)/$ac_arg"
        ;;
    esac
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

version="$(git describe --match "v[0-9]*" --always --tags --abbrev=0 | sed 's/^v//')"

if test -z "$version" ; then
  version="UNKNOWN"
fi

VERSION="${version}${extra}"

if test "$print" = yes ; then
    echo "$VERSION"
    exit 0
fi

NEW_REVISION="#define VERSION \"${VERSION}\""
OLD_REVISION=$(head -n 1 "$version_h" 2> /dev/null)
DMPVCOPYRIGHT="#define DMPVCOPYRIGHT \"Copyright © 2000-2026 dmpv/mpv/MPlayer/mplayer2 projects\""

if test "$NEW_REVISION" != "$OLD_REVISION"; then
    cat <<EOF > "$version_h"
$NEW_REVISION
$DMPVCOPYRIGHT
EOF
fi

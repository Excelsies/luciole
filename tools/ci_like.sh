#!/usr/bin/env bash
# Runs the unit suite the way the CI container does, without a container: a scratch build whose prefix holds no
# installed copy, media off, the superuser (a user namespace that maps you to root and your sub-ids to the rest),
# no GPU (Mesa's software renderer), and fonts as the image installs them: the Noto and Adwaita directories, no emoji font. Extra arguments go to `meson test`, e.g. `tools/ci_like.sh font-preview`.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
state=${CI_LIKE_DIR:-$root/.tmp/ci}
build=$state/build
fonts=${CI_LIKE_FONTS:-/usr/share/fonts/noto:/usr/share/fonts/Adwaita} # colon separated

for dir in ${fonts//:/ }; do
  [[ -d $dir ]] || echo "warning: font directory $dir does not exist; set CI_LIKE_FONTS" >&2
done
mkdir -p "$state/home" "$state/font-cache"
if [[ ! -f $build/build.ninja ]]; then
  meson setup "$build" --buildtype=debug --prefix /nonexistent/luciole -Dmedia=disabled -Dtests=enabled
fi
ninja -C "$build"

cat > "$state/fonts.conf" <<CONF
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "fonts.dtd">
<fontconfig>
$(for dir in ${fonts//:/ }; do echo "  <dir>$dir</dir>"; done)
  <include ignore_missing="yes">/etc/fonts/conf.d</include>
  <selectfont><rejectfont><glob>*/NotoColorEmoji.ttf</glob></rejectfont></selectfont>
  <cachedir>$state/font-cache</cachedir>
</fontconfig>
CONF

exec unshare --map-root-user --map-auto env HOME="$state/home" FONTCONFIG_FILE="$state/fonts.conf" \
  __EGL_VENDOR_LIBRARY_FILENAMES=/usr/share/glvnd/egl_vendor.d/50_mesa.json LIBGL_ALWAYS_SOFTWARE=1 \
  GALLIUM_DRIVER=llvmpipe \
  meson test -C "$build" --no-rebuild --print-errorlogs "$@"

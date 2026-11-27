# Building from source

Build Luciole yourself when no package exists for your distribution or when you want to develop it.

Luciole builds with `just`, Meson and a C++23 compiler (GCC 13+ or Clang 16+).

## Install the dependencies

### Arch Linux

Build and library dependencies:

```sh
sudo pacman -S --needed base-devel meson just \
  wayland wayland-protocols libglvnd \
  glib2 freetype2 fontconfig cairo pango harfbuzz libxkbcommon \
  gdk-pixbuf2 glycin librsvg libpng libarchive xz zlib acl curl libxml2 \
  tomlplusplus nlohmann-json stb
```

For audio and video playback (the optional `media` build option), also install GStreamer:

```sh
sudo pacman -S --needed gstreamer gst-plugins-base gst-plugins-good gst-libav
```

Runtime services and optional helpers:

```sh
sudo pacman -S --needed shared-mime-info glycin gsettings-desktop-schemas dconf \
  adwaita-icon-theme gvfs gvfs-smb gvfs-dnssd glib-networking \
  ffmpeg ffmpegthumbnailer poppler 7zip xdg-terminal-exec
```

GVfs provides Trash and network locations. The other helpers enable media and PDF thumbnails, encrypted 7z archives and terminal actions; see [optional features](installation.md#optional-features).

### Fedora

Build and library dependencies:

```sh
sudo dnf install gcc-c++ meson just \
  wayland-devel wayland-protocols-devel libglvnd-devel \
  glib2-devel freetype-devel fontconfig-devel cairo-devel pango-devel harfbuzz-devel libxkbcommon-devel \
  gdk-pixbuf2-devel glycin-devel librsvg2-devel libpng-devel libarchive-devel xz-devel zlib-devel libacl-devel \
  libcurl-devel libxml2-devel tomlplusplus-devel json-devel stb_image_resize2-devel
```

For audio and video playback (the optional `media` build option), also install GStreamer:

```sh
sudo dnf install gstreamer1-devel gstreamer1-plugins-base-devel gstreamer1-plugins-good
```

Runtime services and optional helpers:

```sh
sudo dnf install shared-mime-info glycin-loaders gsettings-desktop-schemas dconf \
  adwaita-icon-theme gvfs gvfs-smb glib-networking \
  ffmpeg-free ffmpegthumbnailer poppler-utils 7zip xdg-terminal-exec
```

Fedora's `ffmpeg-free` omits some patent-encumbered codecs. The `ffmpeg` package from [RPM Fusion](https://rpmfusion.org/) gives thumbnails for more video formats.

### Other distributions

Install the development packages listed in [PACKAGING.md](../../PACKAGING.md#dependencies).

## Build and install

```sh
git clone https://github.com/noctalia-dev/luciole.git
cd luciole
just build release
sudo just install release
```

This installs under `/usr/local`. Run `just configure release /usr` before building to choose another prefix. To uninstall, run `sudo ninja -C build-release uninstall` from the same checkout.

## Development builds

Inside the source directory, `just run release` builds and starts Luciole with the source tree's assets, without installing. Do not reuse that asset override in an installed launcher.

Run `just` to list every recipe. [CONTRIBUTING.md](../../CONTRIBUTING.md) covers checks and code style.

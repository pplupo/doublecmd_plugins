#!/bin/bash

export OS_TARGET=$(fpc -iTO)
export CPU_TARGET=$(fpc -iTP)

export ARCH=$CPU_TARGET-$OS_TARGET

cd $(dirname "$0")

rm -rf release
mkdir -p release/wcx/diskdir
mkdir -p release/wdx/crx_wdx
mkdir -p release/wdx/exif
mkdir -p release/wdx/ooinfo
mkdir -p release/wdx/ooxml
mkdir -p release/wdx/mediainfo
mkdir -p release/wdx/translitwdx
mkdir -p release/wdx/similarity
mkdir -p release/wdx/xpi_wdx
mkdir -p release/wfx/gvfs
mkdir -p release/wfx/rclone
mkdir -p release/wlx/gstplayer
mkdir -p release/wlx/fileinfo

make -C wcx/diskdir/src clean all
install -m 644 wcx/diskdir/diskdir.wcx release/wcx/diskdir/
install -m 644 wcx/diskdir/*.txt       release/wcx/diskdir/

make -C wdx/crx_wdx clean all
install -m 644 wdx/crx_wdx/crx_wdx.wdx release/wdx/crx_wdx/

make -C wdx/exif clean all
install -m 644 wdx/exif/exif.wdx release/wdx/exif/
install -m 644 wdx/exif/exif.lng release/wdx/exif/
install -m 644 wdx/exif/*.txt    release/wdx/exif/

make -C  wdx/ooinfo/src clean all
install -m 644 wdx/ooinfo/ooinfo.wdx release/wdx/ooinfo/
install -m 644 wdx/ooinfo/ooinfo.lng release/wdx/ooinfo/
install -m 644 wdx/ooinfo/*.txt      release/wdx/ooinfo/

make -C wdx/ooxml/src clean all
install -m 644 wdx/ooxml/ooxml.wdx release/wdx/ooxml/
install -m 644 wdx/ooxml/*.txt     release/wdx/ooxml/

make -C wdx/similarity/src clean all
install -m 644 wdx/similarity/similarity.wdx release/wdx/similarity/
install -m 644 wdx/similarity/leven.ini      release/wdx/similarity/
install -m 644 wdx/similarity/readme.txt     release/wdx/similarity/

make -C  wdx/xpi_wdx/src clean all
install -m 644 wdx/xpi_wdx/xpi_wdx.wdx release/wdx/xpi_wdx/

make -C wfx/gvfs/src clean all
install -m 644 wfx/gvfs/gvfs.wfx release/wfx/gvfs/

lazbuild --build-mode=Release wfx/rclone/src/rclone.lpi
install -m 644 wfx/rclone/rclone.wfx     release/wfx/rclone/
install -m 644 wfx/rclone/src/rclone.ico release/wfx/rclone/
install -m 644 wfx/rclone/COPYING.txt    release/wfx/rclone/

make -C wlx/gstplayer/src clean all
install -m 644 wlx/gstplayer/gstplayer.wlx release/wlx/gstplayer/
install -m 644 wlx/gstplayer/readme.txt    release/wlx/gstplayer/

wlx/fileinfo/build.sh
install -m 644 wlx/fileinfo/fileinfo.wlx* release/wlx/fileinfo/
install -m 755 wlx/fileinfo/fileinfo.sh   release/wlx/fileinfo/
install -m 644 wlx/fileinfo/*.txt         release/wlx/fileinfo/

install -m 644 wdx/mediainfo/luajit/*.lua      release/wdx/mediainfo/
install -m 644 wdx/translitwdx/translitwdx.lua release/wdx/translitwdx/
install -m 644 wdx/translitwdx/readme.txt      release/wdx/translitwdx/

# jsonview
mkdir -p release/wlx/jsonview
make -C wlx/jsonview/src clean all
install -m 644 wlx/jsonview/jsonview_qt6.wlx release/wlx/jsonview/
cp -r wlx/jsonview/langs release/wlx/jsonview/
install -m 644 wlx/jsonview/*.md release/wlx/jsonview/
install -m 644 wlx/jsonview/*.png release/wlx/jsonview/

# csvview
mkdir -p release/wlx/csvview
mkdir -p wlx/csvview/build
(cd wlx/csvview/build && cmake .. && make)
install -m 644 wlx/csvview/build/csvview_qt6.wlx release/wlx/csvview/
[ -f wlx/csvview/build/csvview_gtk3.wlx ] && install -m 644 wlx/csvview/build/csvview_gtk3.wlx release/wlx/csvview/
cp -r wlx/csvview/langs release/wlx/csvview/
install -m 644 wlx/csvview/*.md release/wlx/csvview/
install -m 644 wlx/csvview/*.png release/wlx/csvview/

# kpart
mkdir -p release/wlx/kpart
mkdir -p wlx/kpart/build
(cd wlx/kpart/build && cmake .. && make)
install -m 644 wlx/kpart/build/kpart_host_qt6.wlx release/wlx/kpart/
install -m 644 wlx/kpart/*.md release/wlx/kpart/
install -m 644 wlx/kpart/*.png release/wlx/kpart/

# officeview
mkdir -p release/wlx/officeview
mkdir -p wlx/officeview/build
(cd wlx/officeview/build && cmake .. && make)
install -m 644 wlx/officeview/build/officeview.wlx release/wlx/officeview/
install -m 644 wlx/officeview/*.md release/wlx/officeview/

# logview
mkdir -p release/wlx/logview
mkdir -p wlx/logview/build
(cd wlx/logview/build && cmake .. && make)
install -m 644 wlx/logview/build/logviewer.wlx release/wlx/logview/
[ -f wlx/logview/build/logviewer_gtk3.wlx ] && install -m 644 wlx/logview/build/logviewer_gtk3.wlx release/wlx/logview/
install -m 644 wlx/logview/*.md release/wlx/logview/
install -m 644 wlx/logview/*.png release/wlx/logview/

# mpv_wayland
mkdir -p release/wlx/mpv_wayland
mkdir -p wlx/mpv_wayland/build
(cd wlx/mpv_wayland/build && cmake .. && make)
install -m 644 wlx/mpv_wayland/build/mpv_wayland_qt6.wlx release/wlx/mpv_wayland/
[ -f wlx/mpv_wayland/build/mpv_wayland_gtk3.wlx ] && install -m 644 wlx/mpv_wayland/build/mpv_wayland_gtk3.wlx release/wlx/mpv_wayland/
install -m 644 wlx/mpv_wayland/*.md release/wlx/mpv_wayland/
install -m 644 wlx/mpv_wayland/*.png release/wlx/mpv_wayland/

# kate
mkdir -p release/wlx/kate
mkdir -p wlx/kate/build
(cd wlx/kate/build && cmake .. && make)
install -m 644 wlx/kate/build/rich_editor_qt.wlx release/wlx/kate/
install -m 644 wlx/kate/*.md release/wlx/kate/
install -m 644 wlx/kate/*.png release/wlx/kate/

# diagramview
mkdir -p release/wlx/diagramview
mkdir -p wlx/diagramview/build
(cd wlx/diagramview/build && cmake .. && make)
install -m 644 wlx/diagramview/build/diagramview_qt6.wlx release/wlx/diagramview/
[ -f wlx/diagramview/build/diagramview_gtk3.wlx ] && install -m 644 wlx/diagramview/build/diagramview_gtk3.wlx release/wlx/diagramview/
install -m 644 wlx/diagramview/config.json release/wlx/diagramview/
install -m 644 wlx/diagramview/*.md release/wlx/diagramview/
install -m 644 wlx/diagramview/*.png release/wlx/diagramview/

# mdk
mkdir -p release/wlx/mdk
mkdir -p wlx/mdk/build
(cd wlx/mdk/build && cmake .. && make)
install -m 644 wlx/mdk/build/mdk_qt6.wlx release/wlx/mdk/
[ -f wlx/mdk/build/mdk_gtk3.wlx ] && install -m 644 wlx/mdk/build/mdk_gtk3.wlx release/wlx/mdk/
install -m 644 wlx/mdk/*.md release/wlx/mdk/

pushd release
tar -czpf ../plugins-$(date +%y.%m.%d)-$ARCH.tar.gz *
popd

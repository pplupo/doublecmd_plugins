CC = gcc
CFLAGS = -I../../../sdk -Wall -fPIC -O2

# -Wl,--version-script: export ONLY the WLX "List*" C ABI (see
# ../../../sdk/exports/wlx.map); keep every other symbol local so it cannot be
# interposed against Double Commander's own in-process GTK stack.
all shared static:
	$(CC) -shared -o ../gstplayer.wlx gstplayer.c $(CFLAGS) `pkg-config --cflags --libs gstreamer-video-1.0 gtk+-2.0 gstreamer-1.0` -Wl,--version-script=../../../sdk/exports/wlx.map

clean:
	rm -f ../gstplayer.wlx

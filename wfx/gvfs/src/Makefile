# compiler options
CC = gcc
CFLAGS =-I. -I/usr/include -I../../../sdk \
	-Wall -fPIC -O2 -g \
	-DG_DISABLE_DEPRECATED -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -D_GNU_SOURCE

LIB_SUFFIX=`if test \`uname -m\` = x86_64; then echo 64; fi`

VFS_OBJECTS=gvfs.o

.SUFFIXES: .c
.c.o:
	$(CC) $(CFLAGS) `pkg-config glib-2.0 gio-2.0 --cflags` -c $<

all shared static: gvfs.wfx

# -Wl,--version-script: export ONLY the WFX "Fs*" C ABI (see
# ../../../sdk/exports/wfx.map); keep every other symbol local so it cannot be
# interposed against Double Commander's own in-process libraries.
gvfs.wfx: $(VFS_OBJECTS)
	$(CC) -shared -o ../gvfs.wfx $(VFS_OBJECTS) $(CFLAGS) `pkg-config glib-2.0 gio-2.0 --libs` -Wl,--version-script=../../../sdk/exports/wfx.map

clean: 
	rm -f *.o *.d *.gch
	rm -f ../gvfs.wfx

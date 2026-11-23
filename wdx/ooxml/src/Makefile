#############################################################################
# Makefile for building: ooxml.wdx
# Project:  ooxml.pro
#############################################################################

####### Compiler, tools and options

CC            = gcc
CXX           = g++
DEFINES       = -DQT_NO_DEBUG -DQT_GUI_LIB -DQT_CORE_LIB
CFLAGS        = -O2 -pipe -fstack-protector-strong -fno-plt -Wall -W -D_REENTRANT -fPIC $(DEFINES)
CXXFLAGS      = -O2 -pipe -fstack-protector-strong -fno-plt -std=gnu++11 -Wall -W -D_REENTRANT -fPIC $(DEFINES)
INCPATH       = -I. -I../../../sdk
DISTNAME      = ../ooxml_wdx
DISTDIR       = $(DISTNAME)/

MKDIR         = mkdir -p
MOVE          = mv -f
DEL_FILE      = rm -f
DEL_TREE      = rm -rf
TAR           = tar -cf
COMPRESS      = gzip -9f
LINK          = g++
# --version-script: export ONLY the WDX "Content*" C ABI (see
# ../../../sdk/exports/wdx.map); keep the vendored minizip/tinyxml and C++
# runtime symbols local so none can be interposed in Double Commander's process.
LFLAGS        = -shared -Wl,-O1,--sort-common,--as-needed,-z,relro,-z,now -Wl,--version-script=../../../sdk/exports/wdx.map
LIBS          = $(SUBLIBS) -lz
STRIP         = strip

####### Files

OBJECTS       = OfficeFullText.o \
		OfficeCore.o \
		OfficeApp.o \
		Office2007.o \
		tinyxmlerror.o \
		tinyxml.o \
		tinystr.o \
		tinyxmlparser.o \
		unzip.o \
		miniunz.o \
		ioapi.o

TARGET        = ../ooxml.wdx

first: all

####### Build rules

$(TARGET): $(OBJECTS)
	$(LINK) $(LFLAGS) -o $(TARGET) $(OBJECTS) $(OBJCOMP) $(LIBS)

all: $(TARGET)

dist: $(TARGET) FORCE
	$(STRIP) $(TARGET)
	$(MKDIR) $(DISTNAME) && $(MOVE) $(TARGET) $(DISTDIR)
	$(TAR) $(DISTNAME).tar $(DISTNAME) && $(COMPRESS) $(DISTNAME).tar
	$(DEL_TREE) $(DISTNAME)

clean:
	-$(DEL_FILE) $(TARGET)
	-$(DEL_FILE) $(OBJECTS)

distclean: clean
	-$(DEL_FILE) $(DISTNAME).*

####### Compile

.c.o:
	$(CC) -c $(CFLAGS) $(INCPATH) -c $<

.cpp.o:
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(INCPATH) -c $<

FORCE:

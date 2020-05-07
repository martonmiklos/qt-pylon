QT += multimedia

win32 {
  PYLON_VERSION = 6.1
  PYLON_ROOT = $(ProgramFiles)/Basler/pylon $$PYLON_VERSION
  INCLUDEPATH += $$system($$PYLON_ROOT/bin/pylon-config.exe --cflags)
  CPPFLAGS += $$system( $$PYLON_ROOT/bin/pylon-config.exe --cflags)
  LIBS += $$system( $$PYLON_ROOT/bin/pylon-config.exe --libs-rpath) $$system( $$PYLON_ROOT/bin/pylon-config.exe --libs)
}

linux {
  PYLON_ROOT = /opt/pylon
  INCLUDEPATH += $$system($$PYLON_ROOT/bin/pylon-config --cflags)
  CPPFLAGS += $$system( $$PYLON_ROOT/bin/pylon-config --cflags)
  LIBS += $$system( $$PYLON_ROOT/bin/pylon-config --libs-rpath) $$system( $$PYLON_ROOT/bin/pylon-config --libs)
}

INCLUDEPATH += $$PWD
HEADERS += $$PWD/pyloncamera.h
SOURCES += $$PWD/pyloncamera.cpp

QT += core gui widgets network multimedia

CONFIG += c++17

TARGET   = VideoReferee
TEMPLATE = app

SOURCES += VideoReferee.cpp
HEADERS += VideoReferee.h

OPENCV_ROOT = C:/opencv
OPENCV_VER  = 4120

INCLUDEPATH += $${OPENCV_ROOT}/include

LIBS += -L$${OPENCV_ROOT}/x64/mingw/lib \
        -lopencv_world$${OPENCV_VER}

CONFIG(release, debug|release): win32: CONFIG += windows
CONFIG(debug,   debug|release): win32: CONFIG += console

win32 {
    CONFIG(release, debug|release): DESTDIR_COPY = $$shell_path($${OUT_PWD}/release)
    CONFIG(debug,   debug|release): DESTDIR_COPY = $$shell_path($${OUT_PWD}/debug)

    OPENCV_BIN = $${OPENCV_ROOT}/x64/mingw/bin

    copyopencv.commands  = $(CHK_DIR_EXISTS) $$DESTDIR_COPY $(MKDIR) $$DESTDIR_COPY $$escape_expand(\n\t)
    SRC_WORLD = $$shell_path($${OPENCV_BIN}/libopencv_world$${OPENCV_VER}.dll)
    copyopencv.commands += $${QMAKE_COPY} $$SRC_WORLD $$DESTDIR_COPY $$escape_expand(\n\t)

    for(ffname, libopencv_ffmpeg$${OPENCV_VER}_64.dll opencv_videoio_ffmpeg$${OPENCV_VER}_64.dll) {
        SRC = $$shell_path($${OPENCV_BIN}/$$ffname)
        copyopencv.commands += IF EXIST $$SRC ($${QMAKE_COPY} $$SRC $$DESTDIR_COPY) $$escape_expand(\n\t)
    }

    copyopencv.target    = copy_opencv_dlls
    QMAKE_EXTRA_TARGETS += copyopencv
    POST_TARGETDEPS     += copy_opencv_dlls
}
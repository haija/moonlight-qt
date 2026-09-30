QT = core
CONFIG += console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = dualsense_bluetooth_test
DEFINES += SDL_MAIN_HANDLED SDL_STATIC_LIB
INCLUDEPATH += ../../app ../../moonlight-common-c/moonlight-common-c/src
SOURCES += main.cpp ../../app/streaming/audio/dualsensebluetooth.cpp
win32 {
    DEPENDENCIES_ROOT = $$getenv(MOONLIGHT_TEST_DEPENDENCIES)
    isEmpty(DEPENDENCIES_ROOT): DEPENDENCIES_ROOT = $$PWD/../../libs/windows
    INCLUDEPATH += $$DEPENDENCIES_ROOT/include $$DEPENDENCIES_ROOT/include/x64 $$DEPENDENCIES_ROOT/include/x64/SDL2
    LIBS += $$quote($$DEPENDENCIES_ROOT/lib/x64/opus.lib)
    LIBS += ole32.lib uuid.lib setupapi.lib
    SOURCES += ../../app/streaming/audio/dualsensehaptics.cpp
} else {
    CONFIG += link_pkgconfig
    PKGCONFIG += opus sdl2
}

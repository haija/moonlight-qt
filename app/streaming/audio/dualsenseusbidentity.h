#pragma once
#include <QtGlobal>

#ifdef Q_OS_WIN32
#include "SDL_compat.h"
#include <QString>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include <devpkey.h>
#include <vector>

// HID and audio interfaces belonging to one physical device share a container
// ID. Friendly names and enumeration order are not device identities.
inline bool dualSenseUsbContainer(SDL_GameController* controller, GUID& container)
{
    container = GUID{};
    if (controller == nullptr) return false;
#if SDL_VERSION_ATLEAST(2, 24, 0)
    const char* path = SDL_JoystickPath(SDL_GameControllerGetJoystick(controller));
    if (path == nullptr) return false;
    const auto widePath = QString::fromUtf8(path).toStdWString();
    HDEVINFO info = SetupDiCreateDeviceInfoList(nullptr, nullptr);
    if (info == INVALID_HANDLE_VALUE) return false;
    SP_DEVICE_INTERFACE_DATA interfaceData = {};
    interfaceData.cbSize = sizeof(interfaceData);
    SP_DEVINFO_DATA deviceData = {};
    deviceData.cbSize = sizeof(deviceData);
    DWORD required = 0;
    bool found = false;
    if (SetupDiOpenDeviceInterfaceW(info, widePath.c_str(), 0, &interfaceData)) {
        SetupDiGetDeviceInterfaceDetailW(info, &interfaceData, nullptr, 0, &required, nullptr);
        if (required >= sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) {
            std::vector<BYTE> storage(required);
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
            detail->cbSize = sizeof(*detail);
            if (SetupDiGetDeviceInterfaceDetailW(info, &interfaceData, detail, required,
                                                 nullptr, &deviceData)) {
                DEVPROPTYPE type = 0;
                DWORD size = 0;
                found = SetupDiGetDevicePropertyW(info, &deviceData, &DEVPKEY_Device_ContainerId,
                    &type, reinterpret_cast<BYTE*>(&container), sizeof(container), &size, 0) &&
                    type == DEVPROP_TYPE_GUID && size == sizeof(container) && container != GUID{};
            }
        }
    }
    SetupDiDestroyDeviceInfoList(info);
    return found;
#else
    return false;
#endif
}
#endif

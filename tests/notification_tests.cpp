#include <initguid.h>
#include "../src/device_notifications.h"
#include <functiondiscoverykeys_devpkey.h>
#include <iostream>

int main() {
    HANDLE changed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!changed) return 1;
    try {
        Notifications callback(changed, L"secondary");
        callback.watchPrimary(L"primary");
        const auto expect = [&](bool signaled, const char* reason) {
            const DWORD result = WaitForSingleObject(changed, 0);
            ResetEvent(changed);
            if (result != (signaled ? WAIT_OBJECT_0 : WAIT_TIMEOUT)) throw std::runtime_error(reason);
        };
        callback.OnPropertyValueChanged(L"primary", PKEY_Device_FriendlyName);
        expect(false, "Metadata interrupted playback");
        callback.OnPropertyValueChanged(L"unrelated", PKEY_AudioEngine_DeviceFormat);
        expect(false, "Unrelated format interrupted playback");
        callback.OnDeviceStateChanged(L"unrelated", DEVICE_STATE_ACTIVE);
        callback.OnDeviceAdded(L"unrelated");
        callback.OnDeviceRemoved(L"unrelated");
        expect(false, "Unrelated device interrupted playback");
        callback.OnPropertyValueChanged(L"primary", PKEY_AudioEngine_DeviceFormat);
        expect(true, "Primary format change ignored");
        callback.OnPropertyValueChanged(L"secondary", PKEY_AudioEngine_OEMFormat);
        expect(true, "Secondary format change ignored");
        callback.OnDeviceStateChanged(L"secondary", DEVICE_STATE_UNPLUGGED);
        expect(true, "Secondary disconnect ignored");
        callback.OnDeviceAdded(L"secondary");
        expect(true, "Secondary reconnect ignored");
        callback.OnDefaultDeviceChanged(eRender, eConsole, L"primary");
        expect(false, "Unchanged default interrupted playback");
        callback.OnDefaultDeviceChanged(eCapture, eConsole, L"other");
        callback.OnDefaultDeviceChanged(eRender, eCommunications, L"other");
        expect(false, "Unrelated default role interrupted playback");
        callback.OnDefaultDeviceChanged(eRender, eConsole, L"new-primary");
        expect(true, "New default output ignored");
        callback.watchPrimary(L"new-primary");
        callback.OnPropertyValueChanged(L"primary", PKEY_AudioEngine_DeviceFormat);
        expect(false, "Old primary still monitored");
        callback.OnDeviceRemoved(L"new-primary");
        expect(true, "New primary removal ignored");
        callback.OnDefaultDeviceChanged(eRender, eConsole, nullptr);
        expect(true, "Loss of default output ignored");
        CloseHandle(changed);
        std::cout << "Notification filtering and route-change tests passed.\n";
        return 0;
    } catch (const std::exception& e) {
        CloseHandle(changed);
        std::cerr << e.what() << '\n';
        return 1;
    }
}

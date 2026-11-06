#pragma once
#include <windows.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>

// The mutex protects route IDs during setup only; audio processing never takes it.
class Notifications final : public IMMNotificationClient {
    std::atomic<ULONG> refs_{1};
    HANDLE changed_;
    std::mutex mutex_;
    std::wstring primary_, secondary_;
    bool relevant(LPCWSTR id) {
        std::lock_guard<std::mutex> lock(mutex_);
        return id && (primary_ == id || secondary_ == id);
    }
public:
    Notifications(HANDLE changed, const std::wstring& secondary)
        : changed_(changed), secondary_(secondary) {}
    void watchPrimary(const std::wstring& id) {
        std::lock_guard<std::mutex> lock(mutex_);
        primary_ = id;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *object = static_cast<IMMNotificationClient*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override { const ULONG n = --refs_; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR id, DWORD) override {
        if (relevant(id)) SetEvent(changed_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR id) override {
        if (relevant(id)) SetEvent(changed_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR id) override {
        if (relevant(id)) SetEvent(changed_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR id) override {
        if (flow == eRender && role == eConsole) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!id || primary_ != id) SetEvent(changed_);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR id, const PROPERTYKEY key) override {
        // Names, icons, and unrelated metadata don't require interrupting audio.
        if ((IsEqualPropertyKey(key, PKEY_AudioEngine_DeviceFormat) ||
             IsEqualPropertyKey(key, PKEY_AudioEngine_OEMFormat)) && relevant(id))
            SetEvent(changed_);
        return S_OK;
    }
};

struct NotificationScope {
    IMMDeviceEnumerator* enumerator;
    Microsoft::WRL::ComPtr<Notifications> callback;
    NotificationScope(IMMDeviceEnumerator* e, HANDLE changed, const std::wstring& secondary)
        : enumerator(e) {
        callback.Attach(new Notifications(changed, secondary));
        const HRESULT hr = e->RegisterEndpointNotificationCallback(callback.Get());
        if (FAILED(hr)) throw std::runtime_error("Cannot register audio device notifications.");
    }
    ~NotificationScope() { enumerator->UnregisterEndpointNotificationCallback(callback.Get()); }
};

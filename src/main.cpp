#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <shlobj.h>
#include <knownfolders.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>
#include "audio_buffer.h"
#include "decoder.h"
#include "device_notifications.h"

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

class Failure : public std::runtime_error {
public:
    HRESULT code;
    Failure(const char* where, HRESULT hr) : std::runtime_error(where), code(hr) {}
};
static void check(HRESULT hr, const char* where) { if (FAILED(hr)) throw Failure(where, hr); }
static std::wstring errorText(const Failure& e) {
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                   FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, static_cast<DWORD>(e.code), 0,
                   reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wostringstream out;
    out << e.what() << L" (0x" << std::hex << static_cast<unsigned long>(e.code) << L")";
    if (message) { out << L": " << message; LocalFree(message); }
    return out.str();
}
struct Handle {
    HANDLE value = nullptr;
    explicit Handle(bool manual = false) {
        value = CreateEventW(nullptr, manual, FALSE, nullptr);
        if (!value) throw Failure("CreateEvent", HRESULT_FROM_WIN32(GetLastError()));
    }
    ~Handle() { CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct ComScope {
    ComScope() { check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "Initialize COM"); }
    ~ComScope() { CoUninitialize(); }
};
struct TaskFree { void operator()(void* p) const { CoTaskMemFree(p); } };
static HANDLE stopEvent = nullptr, finishedEvent = nullptr, mainThread = nullptr;
static BOOL WINAPI consoleHandler(DWORD signal) {
    if (signal != CTRL_C_EVENT && signal != CTRL_BREAK_EVENT && signal != CTRL_CLOSE_EVENT &&
        signal != CTRL_LOGOFF_EVENT && signal != CTRL_SHUTDOWN_EVENT) return FALSE;
    SetEvent(stopEvent);
    if (mainThread) CancelSynchronousIo(mainThread);
    if (signal != CTRL_C_EVENT && signal != CTRL_BREAK_EVENT)
        WaitForSingleObject(finishedEvent, 4000);
    return TRUE;
}
static std::wstring idOf(IMMDevice* device) {
    LPWSTR raw = nullptr;
    check(device->GetId(&raw), "Get device ID");
    std::unique_ptr<wchar_t, TaskFree> owner(raw);
    return raw;
}
static std::wstring nameOf(IMMDevice* device) {
    ComPtr<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) return idOf(device);
    PROPVARIANT value;
    PropVariantInit(&value);
    const HRESULT hr = props->GetValue(PKEY_Device_FriendlyName, &value);
    std::wstring name = SUCCEEDED(hr) && value.vt == VT_LPWSTR ? value.pwszVal : L"Audio output";
    PropVariantClear(&value);
    return name;
}
static ComPtr<IMMDevice> primaryOf(IMMDeviceEnumerator* enumerator) {
    ComPtr<IMMDevice> device;
    check(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device), "Find default output");
    return device;
}
struct Device { std::wstring id, name; };
static std::vector<Device> devicesOf(IMMDeviceEnumerator* enumerator) {
    ComPtr<IMMDeviceCollection> devices;
    check(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices), "List outputs");
    UINT count = 0;
    check(devices->GetCount(&count), "Count outputs");
    std::vector<Device> result;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (SUCCEEDED(devices->Item(i, &device))) result.push_back({idOf(device.Get()), nameOf(device.Get())});
    }
    return result;
}
static fs::path configPath() {
    PWSTR path = nullptr;
    check(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path), "Find local app data");
    std::unique_ptr<wchar_t, TaskFree> owner(path);
    return fs::path(path) / L"DualMonitorAudio" / L"device.txt";
}
static std::wstring loadConfig(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        if (fs::exists(path)) throw std::runtime_error("Cannot read the saved device configuration.");
        return {};
    }
    std::string bytes((std::istreambuf_iterator<char>(input)), {});
    if (bytes.size() > 32768) throw std::runtime_error("Device configuration is too large.");
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                          static_cast<int>(bytes.size()), nullptr, 0);
    if (!length) return {};
    std::wstring id(length, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), id.data(), length);
    return id;
}
static void saveConfig(const fs::path& path, const std::wstring& id) {
    fs::create_directories(path.parent_path());
    const int length = WideCharToMultiByte(CP_UTF8, 0, id.data(), static_cast<int>(id.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(length, '\0');
    WideCharToMultiByte(CP_UTF8, 0, id.data(), static_cast<int>(id.size()), bytes.data(), length, nullptr, nullptr);
    const auto temporary = fs::path(path.wstring() + L".tmp");
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        file.close();
        if (!file) throw std::runtime_error("Cannot write device configuration.");
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw Failure("Save device configuration", HRESULT_FROM_WIN32(GetLastError()));
}
static std::wstring chooseDevice(IMMDeviceEnumerator* enumerator) {
    const auto primary = primaryOf(enumerator);
    const auto primaryId = idOf(primary.Get());
    std::vector<Device> choices;
    std::wcout << L"Default output: " << nameOf(primary.Get()) << L"\nSecondary outputs:\n";
    for (const auto& device : devicesOf(enumerator)) {
        if (device.id == primaryId) continue;
        choices.push_back(device);
        std::wcout << L"  " << choices.size() << L". " << device.name << L'\n';
    }
    if (choices.empty()) throw std::runtime_error("No secondary output is active. Connect or enable another output and run again.");
    for (;;) {
        std::wcout << L"Select secondary output [1-" << choices.size() << L"]: " << std::flush;
        std::wstring line;
        if (!std::getline(std::wcin, line)) throw std::runtime_error("Device selection canceled.");
        std::wistringstream input(line);
        size_t choice = 0;
        if ((input >> choice) && (input >> std::ws).eof() && choice > 0 && choice <= choices.size())
            return choices[choice - 1].id;
        std::wcout << L"Enter a device number.\n";
    }
}

class Session {
    Handle captureEvent_, renderEvent_;
    ComPtr<IAudioClient> capture_, render_;
    ComPtr<IAudioCaptureClient> source_;
    ComPtr<IAudioRenderClient> sink_;
    std::unique_ptr<audio::Buffer> buffer_;
    std::unique_ptr<Decoder> decoder_;
    unsigned sampleRate_ = 0;
    UINT32 renderFrames_ = 0;
    bool startedCapture_ = false, startedRender_ = false;
    bool seenPacket_ = false;
    void capture() {
        UINT32 packet = 0;
        check(source_->GetNextPacketSize(&packet), "Read capture packet size");
        while (packet) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            check(source_->GetBuffer(&data, &frames, &flags, nullptr, nullptr), "Read loopback audio");
            if (seenPacket_ && (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)) buffer_->clear();
            seenPacket_ = true;
            decoder_->append(*buffer_, data, frames, (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0);
            check(source_->ReleaseBuffer(frames), "Release capture packet");
            check(source_->GetNextPacketSize(&packet), "Read capture packet size");
        }
    }
    void render() {
        UINT32 padding = 0;
        check(render_->GetCurrentPadding(&padding), "Read output padding");
        if (padding > renderFrames_) throw Failure("Invalid output padding", E_UNEXPECTED);
        const UINT32 available = renderFrames_ - padding;
        if (!available) return;
        BYTE* data = nullptr;
        check(sink_->GetBuffer(available, &data), "Get output buffer");
        buffer_->render(reinterpret_cast<float*>(data), available, sampleRate_);
        check(sink_->ReleaseBuffer(available, 0), "Submit output audio");
    }
public:
    Session(IMMDeviceEnumerator* enumerator, const std::wstring& secondaryId) {
        const auto primary = primaryOf(enumerator);
        if (idOf(primary.Get()) == secondaryId)
            throw std::runtime_error("The selected secondary is now the default output. Duplication paused to avoid feedback. Change the Windows default or run --configure.");
        ComPtr<IMMDevice> secondary;
        check(enumerator->GetDevice(secondaryId.c_str(), &secondary), "Find saved secondary output");
        DWORD state = 0;
        check(secondary->GetState(&state), "Read secondary output state");
        if (!(state & DEVICE_STATE_ACTIVE)) throw std::runtime_error("The selected secondary output is disconnected or disabled.");
        check(primary->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &capture_), "Activate loopback endpoint");
        check(secondary->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &render_), "Activate secondary endpoint");
        WAVEFORMATEX* raw = nullptr;
        check(capture_->GetMixFormat(&raw), "Read primary mix format");
        std::unique_ptr<WAVEFORMATEX, TaskFree> mix(raw);
        decoder_ = std::make_unique<Decoder>(mix.get());
        sampleRate_ = mix->nSamplesPerSec;
        REFERENCE_TIME sourcePeriod = 0, sinkPeriod = 0;
        check(capture_->GetDevicePeriod(&sourcePeriod, nullptr), "Read source period");
        check(render_->GetDevicePeriod(&sinkPeriod, nullptr), "Read output period");
        check(capture_->Initialize(AUDCLNT_SHAREMODE_SHARED,
              AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
              0, 0, mix.get(), nullptr), "Initialize loopback capture");
        WAVEFORMATEX output{};
        output.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        output.nChannels = 2;
        output.nSamplesPerSec = sampleRate_;
        output.wBitsPerSample = 32;
        output.nBlockAlign = 8;
        output.nAvgBytesPerSec = sampleRate_ * 8;
        // WASAPI's native converter handles endpoint rate, sample type and channel
        // differences. The FIFO only corrects independent clock drift.
        check(render_->Initialize(AUDCLNT_SHAREMODE_SHARED,
              AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
              AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_NOPERSIST,
              0, 0, &output, nullptr), "Initialize secondary playback / format conversion");
        check(capture_->SetEventHandle(captureEvent_.value), "Set capture event");
        check(render_->SetEventHandle(renderEvent_.value), "Set playback event");
        check(capture_->GetService(IID_PPV_ARGS(&source_)), "Get capture service");
        check(render_->GetService(IID_PPV_ARGS(&sink_)), "Get render service");
        check(render_->GetBufferSize(&renderFrames_), "Read output buffer size");
        // User-tuned 15 ms of jitter headroom, with a one-device-period minimum.
        const double periodSeconds = static_cast<double>(std::max(sourcePeriod, sinkPeriod)) / 10000000.0;
        const auto target = static_cast<size_t>(sampleRate_ * std::max(0.015, periodSeconds));
        buffer_ = std::make_unique<audio::Buffer>(std::max<size_t>(sampleRate_ / 4, target * 4), target);
        std::wcout << L"Primary:   " << nameOf(primary.Get()) << L"\nSecondary: " << nameOf(secondary.Get())
                   << L"\n" << sampleRate_ << L" Hz, stereo forwarding; jitter target "
                   << (target * 1000 / sampleRate_) << L" ms.\n";
    }
    ~Session() {
        if (startedCapture_) capture_->Stop();
        if (startedRender_) render_->Stop();
    }
    void run(HANDLE stop, HANDLE changed) {
        BYTE* data = nullptr;
        check(sink_->GetBuffer(renderFrames_, &data), "Prime secondary output");
        check(sink_->ReleaseBuffer(renderFrames_, AUDCLNT_BUFFERFLAGS_SILENT), "Prime secondary silence");
        check(capture_->Start(), "Start loopback capture"); startedCapture_ = true;
        check(render_->Start(), "Start secondary playback"); startedRender_ = true;
        std::wcout << L"Running. Ctrl+C to exit.\n" << std::flush;
        HANDLE events[] = {stop, changed, captureEvent_.value, renderEvent_.value};
        for (;;) {
            const DWORD result = WaitForMultipleObjects(4, events, FALSE, INFINITE);
            if (result == WAIT_OBJECT_0 || result == WAIT_OBJECT_0 + 1) return;
            if (result == WAIT_FAILED) throw Failure("Wait for audio", HRESULT_FROM_WIN32(GetLastError()));
            // Drain capture first even when playback woke us, reducing FIFO jitter.
            capture();
            render();
        }
    }
};
struct MmcssScope {
    HANDLE handle;
    MmcssScope() { DWORD task = 0; handle = AvSetMmThreadCharacteristicsW(L"Audio", &task); }
    ~MmcssScope() { if (handle) AvRevertMmThreadCharacteristics(handle); }
};

static int application(int argc, wchar_t** argv, HANDLE stop) {
    bool configure = false, list = false, validate = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--configure") configure = true;
        else if (arg == L"--list") list = true;
        else if (arg == L"--check") validate = true;
        else if (arg == L"--help" || arg == L"-h") {
            std::wcout << L"DualMonitorAudio [--configure] [--check] [--list]\n"
                          L"  --configure  Select and save a secondary output.\n"
                          L"  --check      Initialize saved route without starting audio, then exit.\n"
                          L"  --list       List active outputs without changing configuration.\n"
                          L"Ctrl+C stops duplication. Windows audio settings are unchanged.\n";
            return 0;
        } else { std::wcerr << L"Unknown argument: " << arg << L". Use --help.\n"; return 2; }
    }
    ComScope com;
    ComPtr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          IID_PPV_ARGS(&enumerator)), "Create audio device enumerator");
    if (list) {
        std::wstring primary;
        try { primary = idOf(primaryOf(enumerator.Get()).Get()); } catch (const Failure&) {}
        for (const auto& device : devicesOf(enumerator.Get()))
            std::wcout << device.name << (device.id == primary ? L" [default]" : L"") << L"\n  " << device.id << L'\n';
        return 0;
    }
    // Prevent two copies from forwarding the same mix, or racing configuration.
    HANDLE instance = CreateMutexW(nullptr, FALSE, L"Local\\DualMonitorAudio.SingleInstance");
    if (!instance) throw Failure("Create instance guard", HRESULT_FROM_WIN32(GetLastError()));
    const DWORD mutexError = GetLastError();
    std::unique_ptr<void, decltype(&CloseHandle)> guard(instance, &CloseHandle);
    if (mutexError == ERROR_ALREADY_EXISTS) throw std::runtime_error("DualMonitorAudio is already running in this session. Close it before configuring or starting another copy.");
    const auto path = configPath();
    auto secondary = loadConfig(path);
    if (configure || secondary.empty()) { secondary = chooseDevice(enumerator.Get()); saveConfig(path, secondary); }
    Handle changed(true);
    NotificationScope notifications(enumerator.Get(), changed.value, secondary);
    if (validate) {
        Session session(enumerator.Get(), secondary);
        std::wcout << L"Route initialized successfully; no audio was started.\n";
        return 0;
    }
    MmcssScope scheduling;
    std::wstring previousError;
    while (WaitForSingleObject(stop, 0) != WAIT_OBJECT_0) {
        ResetEvent(changed.value);
        try {
            notifications.callback->watchPrimary(idOf(primaryOf(enumerator.Get()).Get()));
            Session session(enumerator.Get(), secondary);
            previousError.clear();
            session.run(stop, changed.value);
        } catch (const Failure& e) {
            const auto message = errorText(e);
            if (message != previousError) { std::wcerr << message << L"\nWaiting for devices; retrying every 2 seconds. Ctrl+C to exit.\n"; previousError = message; }
        } catch (const std::runtime_error& e) {
            const std::string text = e.what();
            const std::wstring message(text.begin(), text.end());
            if (message != previousError) { std::wcerr << message << L"\nWaiting for devices; retrying every 2 seconds. Ctrl+C to exit.\n"; previousError = message; }
        }
        HANDLE events[] = {stop, changed.value};
        const DWORD wait = WaitForMultipleObjects(2, events, FALSE, 2000);
        if (wait == WAIT_FAILED) throw Failure("Wait for device recovery", HRESULT_FROM_WIN32(GetLastError()));
    }
    std::wcout << L"Stopped.\n";
    return 0;
}
int wmain(int argc, wchar_t** argv) {
    try {
        Handle stop(true), finished(true);
        stopEvent = stop.value; finishedEvent = finished.value;
        DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &mainThread,
                        THREAD_TERMINATE, FALSE, 0);
        SetConsoleCtrlHandler(consoleHandler, TRUE);
        int result = 1;
        try { result = application(argc, argv, stop.value); }
        catch (const Failure& e) { std::wcerr << errorText(e) << L'\n'; }
        catch (const std::exception& e) { std::cerr << e.what() << '\n'; }
        if (WaitForSingleObject(stop.value, 0) == WAIT_OBJECT_0) result = 0;
        SetEvent(finished.value);
        SetConsoleCtrlHandler(consoleHandler, FALSE);
        if (mainThread) CloseHandle(mainThread);
        return result;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

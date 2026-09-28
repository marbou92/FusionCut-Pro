#include "audio_preview.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

// SYSTEM HEADERS MUST STAY AT GLOBAL SCOPE (portable-build engineering
// find - this file's Windows path had NEVER been compiled when it broke
// the first portable run: an #include inside a namespace pastes the
// whole header INTO it, so the original placement below "namespace fc {"
// defined fc::std::mutex, fc::std::condition_variable and the entire
// WASAPI surface inside the project namespace). The Linux build gates
// could never catch that - the platform gate is closed there - and no
// MinGW compile of this file existed before that run; the first one
// rejected the libstdc++ headers instantiated inside fc (gcc resolves
// std names through fc::std first there, where half of them do not
// exist). The winmock offline audit now covers this file, so the class
// of error cannot reach CI silently again. The gate is _WIN32 - the
// compiler's own predefined macro, NOT Q_OS_WIN (which only a Qt header
// can define, and after the unused QString include was removed this
// file includes none). Standard headers come FIRST: Win32 headers are
// macro-heavy, std first keeps them clean.
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <atomic>
#include <condition_variable>
#include <mutex>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <windows.h>
#endif

namespace fc {

#if defined(_WIN32)

namespace {

// {BCDE0395-E52F-467C-8E3D-C4579291692E} - the MMDeviceEnumerator class.
const wchar_t kClsidMMDeviceEnumerator[] = L"{BCDE0395-E52F-467C-8E3D-C4579291692E}";

constexpr REFERENCE_TIME kReferenceUnitsPerSecond = 10000000; // 100 ns units

// The negotiated device sample type. Shared-mode mix formats are
// float32 in practice, but int16 and int32 devices exist (and a
// wrong guess here either whispers garbage or hands the device raw
// float bits as integers - the old "32 bits = float" shortcut did
// exactly that on int32 mix formats).
// File-local at namespace scope, NOT a nested type of the private
// AudioPreview::Impl: the free parseMixFormat helper below must name
// it, and a private nested enum can only be named by members and
// friends - the portable Windows leg (the only one that compiles
// this path) rejected every reference with C++ access errors. Members
// keep the same unqualified spelling through ordinary namespace
// lookup, exactly like kClsidMMDeviceEnumerator above.
enum class DeviceFormat { Float32, Int16, Int32 };

} // namespace

struct AudioPreview::Impl {
    // ---- owned by the render thread (MTA) ----
    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDevice *device = nullptr;
    IAudioClient *client = nullptr;
    IAudioRenderClient *render = nullptr;
    WAVEFORMATEX *format = nullptr; // CoTaskMemAlloc'd mix format
    HANDLE streamEvent = nullptr;   // buffer-empty notifications
    HANDLE controlEvent = nullptr;  // begin/stop control changes
    UINT32 bufferFrames = 0;
    bool comInit = false;

    int rate = 0;
    int channels = 0;
    // The negotiated device sample type - see DeviceFormat above.
    DeviceFormat outFormat = DeviceFormat::Float32;

    // ---- shared state ----
    std::thread thread;
    std::mutex mutex;
    std::condition_variable ready; // probe handshake
    bool probeDone = false;
    bool probeOk = false;
    std::atomic<bool> exitFlag{false};
    std::atomic<bool> beginFlag{false};
    std::atomic<bool> playing{false};
    PullFn pull; // installed by begin() (before Start)
    std::atomic<int64_t> playedFrames{0};

    void closeAll() {
        // Runs on the render thread (the objects' apartment).
        if (client) {
            client->Stop();
        }
        if (render) {
            render->Release();
            render = nullptr;
        }
        if (client) {
            client->Release();
            client = nullptr;
        }
        if (format) {
            CoTaskMemFree(format);
            format = nullptr;
        }
        if (device) {
            device->Release();
            device = nullptr;
        }
        if (enumerator) {
            enumerator->Release();
            enumerator = nullptr;
        }
        if (streamEvent) {
            CloseHandle(streamEvent);
            streamEvent = nullptr;
        }
        if (controlEvent) {
            CloseHandle(controlEvent);
            controlEvent = nullptr;
        }
        if (comInit) {
            CoUninitialize();
            comInit = false;
        }
    }
};

namespace {

// Parses the negotiated mix format into the geometry the mixer needs.
// Returns false for sample types the render path cannot feed.
bool parseMixFormat(const WAVEFORMATEX *fmt, int &rate, int &ch, DeviceFormat &outFormat) {
    rate = fmt->nSamplesPerSec;
    ch = fmt->nChannels;
    outFormat = DeviceFormat::Float32;
    if (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        outFormat = fmt->wBitsPerSample == 32 ? DeviceFormat::Float32 : DeviceFormat::Int16;
        return fmt->wBitsPerSample == 32;
    }
    if (fmt->wFormatTag == WAVE_FORMAT_PCM) {
        if (fmt->wBitsPerSample == 16) {
            outFormat = DeviceFormat::Int16;
            return true;
        }
        if (fmt->wBitsPerSample == 32) {
            outFormat = DeviceFormat::Int32;
            return true;
        }
        return false;
    }
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22) {
        const WAVEFORMATEXTENSIBLE *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(fmt);
        // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT {00000003-0000-0010-8000-00aa00389b71}
        const bool isFloatSub =
            ext->SubFormat.Data1 == 3 && ext->SubFormat.Data2 == 0 &&
            ext->SubFormat.Data3 == 0x0010 && ext->SubFormat.Data4[0] == 0x80 &&
            ext->SubFormat.Data4[1] == 0x00 && ext->SubFormat.Data4[2] == 0xaa &&
            ext->SubFormat.Data4[3] == 0x00 && ext->SubFormat.Data4[4] == 0x38 &&
            ext->SubFormat.Data4[5] == 0x9b && ext->SubFormat.Data4[6] == 0x71;
        // KSDATAFORMAT_SUBTYPE_PCM {00000001-0000-0010-8000-00aa00389b71}
        const bool isPcmSub = ext->SubFormat.Data1 == 1 && ext->SubFormat.Data2 == 0 &&
                              ext->SubFormat.Data3 == 0x0010;
        if (isFloatSub && ext->Samples.wValidBitsPerSample == 32) {
            outFormat = DeviceFormat::Float32;
            return true;
        }
        if (isPcmSub && ext->Samples.wValidBitsPerSample == 16 && fmt->wBitsPerSample == 16) {
            outFormat = DeviceFormat::Int16;
            return true;
        }
        if (isPcmSub && ext->Samples.wValidBitsPerSample == 32 && fmt->wBitsPerSample == 32) {
            outFormat = DeviceFormat::Int32;
            return true;
        }
    }
    return false;
}

} // namespace

AudioPreview::AudioPreview() : impl_(new Impl) {}

AudioPreview::~AudioPreview() {
    stop();
}

bool AudioPreview::probe(int &sampleRate, int &channels) {
    if (impl_->thread.joinable()) {
        stop(); // a re-probe implies a fresh run
    }
    impl_->probeDone = false;
    impl_->probeOk = false;
    impl_->exitFlag = false;
    impl_->beginFlag = false;
    impl_->playing = false;
    impl_->playedFrames = 0;
    failed_ = false; // a fresh run starts optimistic (the device may be back)

    // The render thread owns every COM object (created, used, and
    // released there - a clean apartment story, nothing cross-thread).
    impl_->thread = std::thread([this]() {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->probeDone = true;
            impl_->ready.notify_all();
            return;
        }
        impl_->comInit = true;

        CLSID clsid = CLSID();
        IMMDeviceEnumerator *enumerator = nullptr;
        if (FAILED(CLSIDFromString(kClsidMMDeviceEnumerator, &clsid)) ||
            FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
            !enumerator) {
            impl_->closeAll();
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->probeDone = true;
            impl_->ready.notify_all();
            return;
        }
        impl_->enumerator = enumerator;

        IMMDevice *device = nullptr;
        if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) || !device) {
            impl_->closeAll();
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->probeDone = true;
            impl_->ready.notify_all();
            return;
        }
        impl_->device = device;

        IAudioClient *client = nullptr;
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void **>(&client))) ||
            !client) {
            impl_->closeAll();
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->probeDone = true;
            impl_->ready.notify_all();
            return;
        }
        impl_->client = client;

        WAVEFORMATEX *format = nullptr;
        if (FAILED(client->GetMixFormat(&format)) || !format) {
            impl_->closeAll();
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->probeDone = true;
            impl_->ready.notify_all();
            return;
        }
        impl_->format = format;

        {
            int rate = 0, ch = 0;
            DeviceFormat fmt = DeviceFormat::Float32;
            const bool typeKnown = parseMixFormat(format, rate, ch, fmt);
            // Only 1/2-channel float or int16/int32 mix formats can be
            // fed (the mixer produces interleaved float; int formats
            // convert on write). Anything else: refuse audio preview,
            // keep the app silent-but-working.
            const bool geometryOk =
                typeKnown && rate >= 8000 && rate <= 192000 && (ch == 1 || ch == 2);
            if (!geometryOk) {
                impl_->closeAll();
                std::lock_guard<std::mutex> lock(impl_->mutex);
                impl_->probeDone = true;
                impl_->ready.notify_all();
                return;
            }
            impl_->rate = rate;
            impl_->channels = ch;
            impl_->outFormat = fmt;
        }

        // Event-driven shared mode. Per the WASAPI contract the buffer
        // duration MUST be 0 for shared + EVENTCALLBACK streams: the
        // engine picks its own buffer and GetBufferSize below reports
        // it. Windows 10 tolerates a nonzero hint; Windows 7 - the
        // target platform - can fail Initialize outright or accept it
        // and then never signal the event, which starved the render
        // loop into 200 ms WAIT_TIMEOUT chunks (chopped, lagging, and
        // on some drivers dead-silent audio).
        impl_->streamEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        impl_->controlEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!impl_->streamEvent || !impl_->controlEvent) {
            impl_->closeAll();
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->probeDone = true;
            impl_->ready.notify_all();
            return;
        }
        const HRESULT initRc = client->Initialize(
            AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 0, 0, format, nullptr);
        if (FAILED(initRc) || FAILED(client->SetEventHandle(impl_->streamEvent)) ||
            FAILED(client->GetBufferSize(&impl_->bufferFrames)) ||
            FAILED(client->GetService(__uuidof(IAudioRenderClient),
                                      reinterpret_cast<void **>(&impl_->render))) ||
            !impl_->render) {
            impl_->closeAll();
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->probeDone = true;
            impl_->ready.notify_all();
            return;
        }

        // Probe succeeded: signal the caller, then WAIT for begin/stop.
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->probeDone = true;
            impl_->probeOk = true;
            impl_->ready.notify_all();
        }

        // ---- pre-roll wait: hold the stream stopped ----
        while (!impl_->exitFlag.load() && !impl_->beginFlag.load()) {
            WaitForSingleObject(impl_->controlEvent, 200);
        }
        if (impl_->exitFlag.load()) {
            impl_->closeAll();
            return;
        }

        // ---- render loop ----
        if (FAILED(impl_->client->Start())) {
            failed_ = true; // device death: the transport must not trust this clock
            impl_->closeAll();
            return;
        }
        impl_->playing = true;
        int64_t submitted = 0;
        std::vector<float> scratch;
        while (!impl_->exitFlag.load()) {
            HANDLE waits[2] = {impl_->controlEvent, impl_->streamEvent};
            const DWORD rc = WaitForMultipleObjects(2, waits, FALSE, 200);
            if (rc == WAIT_OBJECT_0) {
                break; // control: stop
            }
            if (rc != WAIT_OBJECT_0 + 1 && rc != WAIT_TIMEOUT) {
                failed_ = true;
                break; // unexpected: bail out rather than spin
            }
            UINT32 padding = 0;
            if (FAILED(impl_->client->GetCurrentPadding(&padding))) {
                failed_ = true; // endpoint gone / format changed
                break;
            }
            impl_->playedFrames = submitted - int64_t(padding);
            const UINT32 available =
                padding < impl_->bufferFrames ? impl_->bufferFrames - padding : 0;
            if (available == 0) {
                continue;
            }
            const int frames = static_cast<int>(available);
            const int ch = impl_->channels;
            scratch.assign(size_t(frames) * ch, 0.0f);
            {
                // The pull callback runs HERE (audio thread). Any
                // failure is silence, never an exception off the
                // audio thread.
                const PullFn pull = [&]() {
                    std::lock_guard<std::mutex> lock(impl_->mutex);
                    return impl_->pull;
                }();
                if (pull) {
                    try {
                        pull(scratch.data(), frames);
                    } catch (...) {
                        std::fill(scratch.begin(), scratch.end(), 0.0f);
                    }
                }
            }
            BYTE *dst = nullptr;
            if (FAILED(impl_->render->GetBuffer(frames, &dst))) {
                failed_ = true; // the render pipeline is wedged: no submission, frozen clock
                continue;
            }
            if (impl_->outFormat == DeviceFormat::Float32) {
                std::memcpy(dst, scratch.data(), size_t(frames) * ch * sizeof(float));
            } else if (impl_->outFormat == DeviceFormat::Int16) {
                // int16 device (rare in shared mode): convert + clamp.
                int16_t *out = reinterpret_cast<int16_t *>(dst);
                for (size_t i = 0; i < scratch.size(); ++i) {
                    const double v = std::clamp(double(scratch[i]), -1.0, 1.0);
                    out[i] = int16_t(std::lrint(v * 32767.0));
                }
            } else {
                // int32 device: same clamp, wider target (the old code
                // guessed "32 bits = float" and pasted raw float bits
                // into the int32 buffer - full-scale white noise).
                int32_t *out = reinterpret_cast<int32_t *>(dst);
                for (size_t i = 0; i < scratch.size(); ++i) {
                    const double v = std::clamp(double(scratch[i]), -1.0, 1.0);
                    out[i] = int32_t(std::llrint(v * 2147483647.0));
                }
            }
            if (FAILED(impl_->render->ReleaseBuffer(frames, 0))) {
                failed_ = true; // the samples never reached the device
                continue;
            }
            submitted += frames;
            impl_->playedFrames = submitted - int64_t(padding);
        }
        impl_->playing = false;
        impl_->closeAll();
    });

    // Wait for the probe handshake (bounded: device open is fast, but
    // never hang the GUI forever on a wedged driver).
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->ready.wait_for(lock, std::chrono::seconds(4), [this]() { return impl_->probeDone; });
    if (!impl_->probeDone || !impl_->probeOk) {
        // The thread will notice and exit on its own; join it here so
        // the object is always either idle or alive.
        lock.unlock();
        if (impl_->thread.joinable()) {
            impl_->exitFlag = true;
            if (impl_->controlEvent) {
                SetEvent(impl_->controlEvent);
            }
            impl_->thread.join();
        }
        return false;
    }
    sampleRate = impl_->rate;
    channels = impl_->channels;
    return true;
}

bool AudioPreview::begin(const PullFn &pull) {
    if (!impl_->thread.joinable() || !impl_->probeOk || impl_->playing.load() || failed_.load()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->pull = pull;
        impl_->playedFrames = 0;
    }
    impl_->beginFlag = true;
    SetEvent(impl_->controlEvent);
    running_ = true;
    return true;
}

void AudioPreview::stop() {
    if (impl_->thread.joinable()) {
        impl_->exitFlag = true;
        if (impl_->controlEvent) {
            SetEvent(impl_->controlEvent);
        }
        impl_->thread.join();
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->pull = PullFn();
    }
    running_ = false;
}

double AudioPreview::playedSeconds() const {
    if (!impl_->playing.load() || impl_->rate <= 0) {
        return 0.0;
    }
    return double(impl_->playedFrames.load()) / double(impl_->rate);
}

bool AudioPreview::healthy() const {
    // Alive AND not failed AND actually rendering: after a mid-run
    // device death the thread has exited but running_ is only cleared
    // by stop(), so the transport must not keep reading a frozen clock.
    return running_.load() && !failed_.load() && impl_->playing.load();
}

#else // !_WIN32

struct AudioPreview::Impl {};

AudioPreview::AudioPreview() : impl_(new Impl) {}

AudioPreview::~AudioPreview() {
    stop();
}

bool AudioPreview::probe(int &sampleRate, int &channels) {
    (void)sampleRate;
    (void)channels;
    return false; // no audio output layer on this platform
}

bool AudioPreview::begin(const PullFn &pull) {
    (void)pull;
    return false;
}

void AudioPreview::stop() {}

bool AudioPreview::healthy() const {
    return false; // no audio output layer on this platform
}

double AudioPreview::playedSeconds() const {
    return 0.0;
}

#endif // _WIN32

} // namespace fc

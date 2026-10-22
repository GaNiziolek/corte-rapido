// Corte Rápido — cortador de vídeo minimalista.
// Win32 + Media Foundation puro: nenhuma dependência além do próprio Windows 10+.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <dxgiformat.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#include <propvarutil.h>
#include <wrl/client.h>
#include <string>
#include <atomic>
#include <thread>
#include <algorithm>
#include <cmath>
#include <cwchar>
#undef GetCurrentTime

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using Microsoft::WRL::ComPtr;

static const wchar_t* kAppName = L"Corte Rápido";

enum : UINT {
    WM_APP_ENGINE = WM_APP + 1,   // wParam = evento, lParam = param1
    WM_APP_PROGRESS,              // wParam = 0..1000
    WM_APP_DONE,                  // wParam = HRESULT
};

enum : UINT {
    ID_OPEN = 101, ID_PLAY, ID_CANCEL,
    ID_EXPORT = 201, ID_PLAYSEL, ID_CLEARSEL,
    TIMER_TICK = 1,
};

// Paleta
static const COLORREF C_BG = RGB(24, 24, 27);
static const COLORREF C_PANEL = RGB(34, 34, 38);
static const COLORREF C_TRACK = RGB(58, 58, 64);
static const COLORREF C_SEL = RGB(122, 52, 36);
static const COLORREF C_ACCENT = RGB(255, 99, 56);
static const COLORREF C_TEXT = RGB(230, 230, 230);
static const COLORREF C_DIM = RGB(140, 140, 148);

struct App {
    HWND hwnd = nullptr, hVideo = nullptr, hTimeline = nullptr;
    HWND hBtnOpen = nullptr, hBtnPlay = nullptr, hLabel = nullptr, hProgress = nullptr, hBtnCancel = nullptr;
    HFONT font = nullptr, fontSmall = nullptr;
    HBRUSH brBg = nullptr;
    UINT dpi = 96;
    RECT videoArea{};

    ComPtr<IMFMediaEngine> engine;
    ComPtr<IMFMediaEngineEx> engineEx;
    std::wstring path;
    bool loaded = false;
    double duration = 0, fps = 30;
    DWORD vw = 0, vh = 0;

    double cursor = 0;
    double selA = -1, selB = -1;
    bool playSel = false;
    bool hasPendingSeek = false;
    double pendingSeek = 0;

    int dragMode = 0;     // 0 nada, 1 nova seleção, 2 borda inicial, 3 borda final
    double anchor = 0;
    int downX = 0;
    bool moved = false, wasPlaying = false;

    std::thread worker;
    std::atomic<bool> exporting{false}, cancel{false};
    std::wstring exportPath;
    std::wstring lastLabel;
} g;

static int S(int v) { return MulDiv(v, (int)g.dpi, 96); }
static bool HasSel() { return g.selA >= 0 && g.selB > g.selA; }

static std::wstring FmtTime(double t) {
    if (t < 0) t = 0;
    long long ms = llround(t * 1000);
    long long s = ms / 1000;
    wchar_t b[64];
    if (s >= 3600)
        swprintf(b, 64, L"%lld:%02lld:%02lld.%03lld", s / 3600, (s / 60) % 60, s % 60, ms % 1000);
    else
        swprintf(b, 64, L"%02lld:%02lld.%03lld", s / 60, s % 60, ms % 1000);
    return b;
}

static std::wstring FmtFileTime(double t) {
    long long ds = llround(t * 10);  // décimos de segundo
    long long s = ds / 10;
    wchar_t b[64];
    if (s >= 3600)
        swprintf(b, 64, L"%lldh%02lldm%02lld.%llds", s / 3600, (s / 60) % 60, s % 60, ds % 10);
    else
        swprintf(b, 64, L"%02lldm%02lld.%llds", s / 60, s % 60, ds % 10);
    return b;
}

static std::wstring HrText(HRESULT hr) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, hr, 0, (LPWSTR)&msg, 0, nullptr);
    if (!msg) {
        HMODULE mf = GetModuleHandleW(L"mfplat.dll");
        if (mf)
            FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
                           mf, hr, 0, (LPWSTR)&msg, 0, nullptr);
    }
    wchar_t code[32];
    swprintf(code, 32, L"0x%08X", (unsigned)hr);
    std::wstring r = msg ? std::wstring(msg) + L"(" + code + L")" : std::wstring(L"Erro ") + code;
    if (msg) LocalFree(msg);
    return r;
}

// ---------------------------------------------------------------------------
// Pré-visualização (IMFMediaEngine renderizando direto na janela de vídeo)

class EngineNotify : public IMFMediaEngineNotify {
    LONG ref_ = 1;
    HWND hwnd_;
public:
    explicit EngineNotify(HWND h) : hwnd_(h) {}
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFMediaEngineNotify)) {
            *ppv = static_cast<IMFMediaEngineNotify*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override {
        LONG r = InterlockedDecrement(&ref_);
        if (!r) delete this;
        return r;
    }
    STDMETHODIMP EventNotify(DWORD ev, DWORD_PTR p1, DWORD) override {
        if (ev == MF_MEDIA_ENGINE_EVENT_NOTIFYSTABLESTATE) {
            SetEvent(reinterpret_cast<HANDLE>(p1));
            return S_OK;
        }
        PostMessageW(hwnd_, WM_APP_ENGINE, ev, (LPARAM)p1);
        return S_OK;
    }
};

static HRESULT CreateEngine() {
    ComPtr<IMFMediaEngineClassFactory> factory;
    HRESULT hr = CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    ComPtr<IMFAttributes> attr;
    MFCreateAttributes(&attr, 3);
    EngineNotify* notify = new EngineNotify(g.hwnd);
    attr->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, notify);
    notify->Release();
    attr->SetUINT64(MF_MEDIA_ENGINE_PLAYBACK_HWND, (UINT64)(ULONG_PTR)g.hVideo);
    attr->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
    hr = factory->CreateInstance(0, attr.Get(), &g.engine);
    if (FAILED(hr)) return hr;
    g.engine.As(&g.engineEx);
    g.engine->SetPreload(MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC);
    g.engine->SetAutoPlay(FALSE);
    return S_OK;
}

static void UpdateVideoRect() {
    if (!g.engineEx || !g.loaded) return;
    RECT rc;
    GetClientRect(g.hVideo, &rc);
    MFARGB black = {0, 0, 0, 255};
    g.engineEx->UpdateVideoStream(nullptr, &rc, &black);
}

static bool IsPlaying() { return g.engine && g.loaded && !g.engine->IsPaused(); }

static void FlushSeek() {
    if (g.hasPendingSeek && g.engine && !g.engine->IsSeeking()) {
        g.engine->SetCurrentTime(g.pendingSeek);
        g.hasPendingSeek = false;
    }
}

static void Seek(double t) {
    if (!g.loaded) return;
    t = std::clamp(t, 0.0, g.duration);
    g.cursor = t;
    g.pendingSeek = t;
    g.hasPendingSeek = true;
    FlushSeek();
    InvalidateRect(g.hTimeline, nullptr, FALSE);
}

static void Pause() {
    if (g.engine) g.engine->Pause();
    g.playSel = false;
}

static void Play() {
    if (!g.loaded) return;
    if (g.cursor >= g.duration - 0.02) Seek(0);
    g.engine->Play();
}

static void TogglePlay() {
    if (!g.loaded || g.exporting) return;
    if (IsPlaying()) Pause();
    else Play();
}

static void PlaySelection() {
    if (!HasSel() || g.exporting) return;
    Seek(g.selA);
    g.engine->Play();
    g.playSel = true;
}

static void ProbeFps(const std::wstring& file) {
    g.fps = 30;
    ComPtr<IMFSourceReader> r;
    if (FAILED(MFCreateSourceReaderFromURL(file.c_str(), nullptr, &r))) return;
    ComPtr<IMFMediaType> mt;
    if (FAILED(r->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &mt))) return;
    UINT32 n = 0, d = 0;
    if (SUCCEEDED(MFGetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, &n, &d)) && n && d) g.fps = (double)n / d;
}

static void Layout();

static void OpenFile(const std::wstring& file) {
    if (g.exporting || !g.engine) return;
    g.engine->Pause();
    g.loaded = false;
    g.path = file;
    g.duration = 0;
    g.cursor = 0;
    g.selA = g.selB = -1;
    g.playSel = false;
    g.hasPendingSeek = false;
    g.vw = g.vh = 0;
    ShowWindow(g.hVideo, SW_HIDE);
    ProbeFps(file);
    BSTR url = SysAllocString(file.c_str());
    HRESULT hr = g.engine->SetSource(url);
    SysFreeString(url);
    if (FAILED(hr)) {
        MessageBoxW(g.hwnd, (L"Não foi possível abrir o vídeo.\n\n" + HrText(hr)).c_str(), kAppName, MB_ICONERROR);
        return;
    }
    std::wstring title = std::wstring(kAppName) + L" — " + PathFindFileNameW(file.c_str());
    SetWindowTextW(g.hwnd, title.c_str());
    InvalidateRect(g.hwnd, nullptr, TRUE);
    InvalidateRect(g.hTimeline, nullptr, FALSE);
}

static void OnEngineEvent(DWORD ev) {
    switch (ev) {
    case MF_MEDIA_ENGINE_EVENT_LOADEDMETADATA:
        g.duration = g.engine->GetDuration();
        if (!std::isfinite(g.duration) || g.duration < 0) g.duration = 0;
        g.engine->GetNativeVideoSize(&g.vw, &g.vh);
        g.loaded = true;
        ShowWindow(g.hVideo, g.vw ? SW_SHOW : SW_HIDE);
        Layout();
        InvalidateRect(g.hwnd, nullptr, TRUE);
        InvalidateRect(g.hTimeline, nullptr, FALSE);
        break;
    case MF_MEDIA_ENGINE_EVENT_LOADEDDATA:
        // força a exibição do primeiro quadro
        g.engine->SetCurrentTime(0);
        break;
    case MF_MEDIA_ENGINE_EVENT_ERROR: {
        ComPtr<IMFMediaError> err;
        USHORT code = 0;
        HRESULT ext = S_OK;
        if (SUCCEEDED(g.engine->GetError(&err)) && err) {
            code = err->GetErrorCode();
            ext = err->GetExtendedErrorCode();
        }
        if (code == MF_MEDIA_ENGINE_ERR_ABORTED) break;
        g.loaded = false;
        ShowWindow(g.hVideo, SW_HIDE);
        InvalidateRect(g.hwnd, nullptr, TRUE);
        std::wstring msg = L"Não foi possível reproduzir este vídeo.\n"
                           L"O formato/codec pode não ser suportado pelo Windows.";
        if (FAILED(ext)) msg += L"\n\n" + HrText(ext);
        MessageBoxW(g.hwnd, msg.c_str(), kAppName, MB_ICONERROR);
        break;
    }
    }
}

// ---------------------------------------------------------------------------
// Exportação (Source Reader decodifica -> Sink Writer recodifica H.264/AAC em MP4)

static HRESULT ConfigureWriter(const std::wstring& dst, bool hw, IMFSourceReader* reader,
                               bool hasV, DWORD vIdx, bool& hasA, DWORD aIdx,
                               ComPtr<IMFSinkWriter>& writer, DWORD& vOut, DWORD& aOut) {
    writer.Reset();
    ComPtr<IMFAttributes> wa;
    MFCreateAttributes(&wa, 3);
    wa->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hw ? TRUE : FALSE);
    wa->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    wa->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    HRESULT hr = MFCreateSinkWriterFromURL(dst.c_str(), nullptr, wa.Get(), &writer);
    if (FAILED(hr)) return hr;

    if (hasV) {
        ComPtr<IMFMediaType> native, in, out;
        reader->GetNativeMediaType(vIdx, 0, &native);
        hr = reader->GetCurrentMediaType(vIdx, &in);
        if (FAILED(hr)) return hr;
        UINT32 w = 0, h = 0, fn = 30, fd = 1, pn = 1, pd = 1;
        MFGetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, &w, &h);
        if (FAILED(MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &fn, &fd)) || !fn || !fd) { fn = 30; fd = 1; }
        if (FAILED(MFGetAttributeRatio(native.Get(), MF_MT_PIXEL_ASPECT_RATIO, &pn, &pd)) || !pn || !pd) { pn = pd = 1; }

        // Taxa de bits: a mesma do original (H.265 precisa de ~1.5x em H.264)
        UINT32 bitrate = MFGetAttributeUINT32(native.Get(), MF_MT_AVG_BITRATE, 0);
        GUID sub = GUID_NULL;
        native->GetGUID(MF_MT_SUBTYPE, &sub);
        if (!bitrate) bitrate = (UINT32)std::min<double>(w * (double)h * ((double)fn / fd) * 0.15, 40e6);
        if (sub == MFVideoFormat_HEVC) bitrate = (UINT32)std::min<double>(bitrate * 1.5, 4e9);
        bitrate = std::clamp<UINT32>(bitrate, 1500000, 100000000);

        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        out->SetUINT32(MF_MT_AVG_BITRATE, bitrate);
        out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, fn, fd);
        MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, pn, pd);
        UINT32 rot = 0;
        if (SUCCEEDED(native->GetUINT32(MF_MT_VIDEO_ROTATION, &rot)) && rot) out->SetUINT32(MF_MT_VIDEO_ROTATION, rot);

        hr = writer->AddStream(out.Get(), &vOut);
        if (FAILED(hr)) return hr;
        hr = writer->SetInputMediaType(vOut, in.Get(), nullptr);
        if (FAILED(hr)) return hr;
    }

    if (hasA) {
        ComPtr<IMFMediaType> in, out;
        HRESULT ahr = reader->GetCurrentMediaType(aIdx, &in);
        UINT32 ch = 0, rate = 0, bits = 0;
        if (SUCCEEDED(ahr)) {
            ch = MFGetAttributeUINT32(in.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0);
            rate = MFGetAttributeUINT32(in.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
            bits = MFGetAttributeUINT32(in.Get(), MF_MT_AUDIO_BITS_PER_SAMPLE, 0);
        }
        bool ok = SUCCEEDED(ahr) && (ch == 1 || ch == 2) && (rate == 44100 || rate == 48000) && bits == 16;
        if (ok) {
            MFCreateMediaType(&out);
            out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
            out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
            out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, ch);
            out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);  // 192 kbps
            ok = SUCCEEDED(writer->AddStream(out.Get(), &aOut)) &&
                 SUCCEEDED(writer->SetInputMediaType(aOut, in.Get(), nullptr));
        }
        if (!ok) {
            if (!hasV) return MF_E_INVALIDMEDIATYPE;
            // Sem como recodificar o áudio: recomeça só com vídeo.
            hasA = false;
            reader->SetStreamSelection(aIdx, FALSE);
            return ConfigureWriter(dst, hw, reader, hasV, vIdx, hasA, aIdx, writer, vOut, aOut);
        }
    }
    return S_OK;
}

static HRESULT ExportClip(const std::wstring& src, const std::wstring& dst, double a, double b, HWND notify) {
    ComPtr<IMFAttributes> ra;
    MFCreateAttributes(&ra, 1);
    ra->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    ComPtr<IMFSourceReader> reader;
    HRESULT hr = MFCreateSourceReaderFromURL(src.c_str(), ra.Get(), &reader);
    if (FAILED(hr)) return hr;

    reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    bool hasV = false, hasA = false;
    DWORD vIdx = 0, aIdx = 0;
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> mt;
        if (FAILED(reader->GetNativeMediaType(i, 0, &mt))) break;
        GUID major;
        mt->GetGUID(MF_MT_MAJOR_TYPE, &major);
        if (major == MFMediaType_Video && !hasV) { hasV = true; vIdx = i; }
        else if (major == MFMediaType_Audio && !hasA) { hasA = true; aIdx = i; }
    }
    if (!hasV && !hasA) return MF_E_INVALIDSTREAMNUMBER;

    if (hasV) {
        ComPtr<IMFMediaType> t;
        MFCreateMediaType(&t);
        t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        hr = reader->SetCurrentMediaType(vIdx, nullptr, t.Get());
        if (FAILED(hr)) return hr;
        reader->SetStreamSelection(vIdx, TRUE);
    }
    if (hasA) {
        ComPtr<IMFMediaType> native;
        reader->GetNativeMediaType(aIdx, 0, &native);
        UINT32 rate = MFGetAttributeUINT32(native.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        UINT32 ch = MFGetAttributeUINT32(native.Get(), MF_MT_AUDIO_NUM_CHANNELS, 2);
        auto tryPcm = [&](UINT32 r, UINT32 c) {
            ComPtr<IMFMediaType> t;
            MFCreateMediaType(&t);
            t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            if (r) t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, r);
            if (c) t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, c);
            return SUCCEEDED(reader->SetCurrentMediaType(aIdx, nullptr, t.Get()));
        };
        UINT32 wantRate = (rate == 44100 || rate == 48000) ? rate : 48000;
        UINT32 wantCh = ch > 2 ? 2 : ch;
        if (tryPcm(wantRate, wantCh) || tryPcm(0, 0)) reader->SetStreamSelection(aIdx, TRUE);
        else hasA = false;
    }
    if (!hasV && !hasA) return MF_E_INVALIDMEDIATYPE;

    ComPtr<IMFSinkWriter> writer;
    DWORD vOut = 0, aOut = 0;
    bool hasA0 = hasA;
    hr = ConfigureWriter(dst, true, reader.Get(), hasV, vIdx, hasA, aIdx, writer, vOut, aOut);
    if (SUCCEEDED(hr)) hr = writer->BeginWriting();
    if (FAILED(hr)) {
        // Codificador por hardware recusou: tenta o de software.
        hasA = hasA0;
        if (hasA) reader->SetStreamSelection(aIdx, TRUE);
        writer.Reset();
        hr = ConfigureWriter(dst, false, reader.Get(), hasV, vIdx, hasA, aIdx, writer, vOut, aOut);
        if (SUCCEEDED(hr)) hr = writer->BeginWriting();
        if (FAILED(hr)) return hr;
    }

    const LONGLONG A = (LONGLONG)llround(a * 1e7), B = (LONGLONG)llround(b * 1e7);
    PROPVARIANT pos;
    InitPropVariantFromInt64(A, &pos);
    hr = reader->SetCurrentPosition(GUID_NULL, pos);
    PropVariantClear(&pos);
    if (FAILED(hr)) return hr;

    bool vDone = !hasV, aDone = !hasA;
    int lastPct = -1;
    while (!(vDone && aDone)) {
        if (g.cancel) return E_ABORT;
        DWORD si = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> s;
        hr = reader->ReadSample((DWORD)MF_SOURCE_READER_ANY_STREAM, 0, &si, &flags, &ts, &s);
        if (FAILED(hr)) return hr;
        const bool isV = hasV && si == vIdx;
        if (!isV && !(hasA && si == aIdx)) continue;
        bool& done = isV ? vDone : aDone;
        const DWORD outIdx = isV ? vOut : aOut;
        if (done) continue;
        if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) { done = true; continue; }
        if (flags & MF_SOURCE_READERF_STREAMTICK) {
            if (ts >= A && ts < B) writer->SendStreamTick(outIdx, ts - A);
        }
        if (!s) continue;
        LONGLONG dur = 0;
        s->GetSampleDuration(&dur);
        if (ts >= B) {
            done = true;
            reader->SetStreamSelection(si, FALSE);
            continue;
        }
        if (ts + dur <= A) continue;
        if (ts < A) {
            s->SetSampleDuration(ts + dur - A);
            ts = A;
        }
        s->SetSampleTime(ts - A);
        hr = writer->WriteSample(outIdx, s.Get());
        if (FAILED(hr)) return hr;
        if (isV || !hasV) {
            int pct = (int)std::clamp<LONGLONG>((ts - A) * 1000 / std::max<LONGLONG>(B - A, 1), 0, 1000);
            if (pct != lastPct) {
                lastPct = pct;
                PostMessageW(notify, WM_APP_PROGRESS, pct, 0);
            }
        }
    }
    return writer->Finalize();
}

static void SetExportingUi(bool on) {
    g.exporting = on;
    ShowWindow(g.hProgress, on ? SW_SHOW : SW_HIDE);
    ShowWindow(g.hBtnCancel, on ? SW_SHOW : SW_HIDE);
    EnableWindow(g.hBtnOpen, !on);
    EnableWindow(g.hBtnPlay, !on);
    SendMessageW(g.hProgress, PBM_SETPOS, 0, 0);
    Layout();
}

static void StartExport() {
    if (!HasSel() || g.exporting || !g.loaded) return;
    Pause();

    wchar_t dir[MAX_PATH * 2];
    wcscpy_s(dir, g.path.c_str());
    PathRemoveFileSpecW(dir);
    std::wstring base = PathFindFileNameW(g.path.c_str());
    size_t dot = base.find_last_of(L'.');
    if (dot != std::wstring::npos) base.resize(dot);
    std::wstring suggestion = std::wstring(dir) + L"\\" + base + L" (corte " + FmtFileTime(g.selA) + L" - " +
                              FmtFileTime(g.selB) + L").mp4";

    wchar_t file[32768];
    wcscpy_s(file, suggestion.c_str());
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = L"Vídeo MP4 (*.mp4)\0*.mp4\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = ARRAYSIZE(file);
    ofn.lpstrInitialDir = dir;
    ofn.lpstrDefExt = L"mp4";
    ofn.lpstrTitle = L"Salvar corte como";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) return;

    if (lstrcmpiW(file, g.path.c_str()) == 0) {
        MessageBoxW(g.hwnd, L"Escolha um nome diferente do vídeo original.", kAppName, MB_ICONWARNING);
        return;
    }

    g.exportPath = file;
    g.cancel = false;
    SetExportingUi(true);
    std::wstring src = g.path, dst = file;
    double a = g.selA, b = g.selB;
    HWND notify = g.hwnd;
    g.worker = std::thread([src, dst, a, b, notify] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        HRESULT hr = ExportClip(src, dst, a, b, notify);
        CoUninitialize();
        PostMessageW(notify, WM_APP_DONE, (WPARAM)hr, 0);
    });
}

static void OnExportDone(HRESULT hr) {
    if (g.worker.joinable()) g.worker.join();
    SetExportingUi(false);
    if (hr == E_ABORT) {
        DeleteFileW(g.exportPath.c_str());
        return;
    }
    if (FAILED(hr)) {
        DeleteFileW(g.exportPath.c_str());
        MessageBoxW(g.hwnd, (L"Falha ao exportar o corte.\n\n" + HrText(hr)).c_str(), kAppName, MB_ICONERROR);
        return;
    }
    std::wstring msg = L"Corte salvo em:\n" + g.exportPath + L"\n\nAbrir a pasta?";
    if (MessageBoxW(g.hwnd, msg.c_str(), kAppName, MB_ICONINFORMATION | MB_YESNO) == IDYES) {
        PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(g.exportPath.c_str());
        if (pidl) {
            SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
            ILFree(pidl);
        }
    }
}

// ---------------------------------------------------------------------------
// Linha do tempo

static RECT TrackRect() {
    RECT rc;
    GetClientRect(g.hTimeline, &rc);
    return {S(14), S(26), rc.right - S(14), S(26) + S(38)};
}

static int TimeToX(double t) {
    RECT r = TrackRect();
    if (g.duration <= 0) return r.left;
    return r.left + (int)llround(t / g.duration * (r.right - r.left));
}

static double XToTime(int x) {
    RECT r = TrackRect();
    if (g.duration <= 0 || r.right <= r.left) return 0;
    return std::clamp((double)(x - r.left) / (r.right - r.left), 0.0, 1.0) * g.duration;
}

static void FillRectColor(HDC dc, const RECT& r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, (HBRUSH)GetStockObject(DC_BRUSH));
}

static void PaintTimeline(HDC dc, const RECT& rc) {
    FillRectColor(dc, rc, C_PANEL);
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, g.fontSmall);
    RECT tr = TrackRect();
    FillRectColor(dc, tr, C_TRACK);

    if (g.loaded && g.duration > 0) {
        // Marcas de tempo
        static const double steps[] = {0.1, 0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200};
        double step = steps[ARRAYSIZE(steps) - 1];
        for (double s : steps) {
            if ((tr.right - tr.left) * s / g.duration >= S(80)) { step = s; break; }
        }
        SetTextColor(dc, C_DIM);
        for (double t = 0; t <= g.duration + 1e-9; t += step) {
            int x = TimeToX(t);
            RECT tick = {x, tr.top - S(6), x + 1, tr.top};
            FillRectColor(dc, tick, C_DIM);
            wchar_t b[32];
            long long s = (long long)floor(t + 1e-6);
            if (step < 1) swprintf(b, 32, L"%lld:%02lld.%d", s / 60, s % 60, (int)llround((t - s) * 10) % 10);
            else if (s >= 3600) swprintf(b, 32, L"%lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
            else swprintf(b, 32, L"%lld:%02lld", s / 60, s % 60);
            // rótulo à direita da marca; perto do fim da trilha, à esquerda para não cortar
            RECT lr = {x + S(3), tr.top - S(22), x + S(80), tr.top - S(4)};
            UINT align = DT_LEFT;
            if (x + S(40) > rc.right) { lr = {x - S(80), tr.top - S(22), x - S(3), tr.top - S(4)}; align = DT_RIGHT; }
            DrawTextW(dc, b, -1, &lr, align | DT_SINGLELINE | DT_BOTTOM | DT_NOCLIP);
        }

        // Seleção
        if (HasSel()) {
            int xa = TimeToX(g.selA), xb = TimeToX(g.selB);
            RECT sr = {xa, tr.top, std::max(xb, xa + 1), tr.bottom};
            FillRectColor(dc, sr, C_SEL);
            RECT ea = {xa - S(1), tr.top - S(2), xa + S(2), tr.bottom + S(2)};
            RECT eb = {xb - S(2), tr.top - S(2), xb + S(1), tr.bottom + S(2)};
            FillRectColor(dc, ea, C_ACCENT);
            FillRectColor(dc, eb, C_ACCENT);
            RECT ha = {xa - S(1), tr.top + S(10), xa + S(5), tr.bottom - S(10)};
            RECT hb = {xb - S(5), tr.top + S(10), xb + S(1), tr.bottom - S(10)};
            FillRectColor(dc, ha, C_ACCENT);
            FillRectColor(dc, hb, C_ACCENT);
        }

        // Cursor de reprodução
        int xc = TimeToX(g.cursor);
        RECT pl = {xc - S(1), tr.top - S(4), xc + S(1), tr.bottom + S(4)};
        FillRectColor(dc, pl, RGB(255, 255, 255));
        POINT tri[3] = {{xc - S(6), tr.top - S(10)}, {xc + S(6), tr.top - S(10)}, {xc, tr.top - S(3)}};
        SetDCBrushColor(dc, RGB(255, 255, 255));
        SetDCPenColor(dc, RGB(255, 255, 255));
        HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH));
        HGDIOBJ op = SelectObject(dc, GetStockObject(DC_PEN));
        Polygon(dc, tri, 3);
        SelectObject(dc, ob);
        SelectObject(dc, op);
    }

    SetTextColor(dc, C_DIM);
    RECT hr = {tr.left, tr.bottom + S(6), rc.right - S(14), rc.bottom - S(4)};
    const wchar_t* hint = g.loaded
        ? L"Arraste para selecionar  •  Botão direito no trecho → exportar  •  Espaço: play/pause  •  I / O: marcar início/fim  •  ← →: quadro a quadro"
        : L"Abra ou arraste um vídeo para começar";
    DrawTextW(dc, hint, -1, &hr, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
}

static int NearEdge(int x) {
    if (!HasSel()) return 0;
    int xa = TimeToX(g.selA), xb = TimeToX(g.selB);
    int tol = S(6);
    int da = abs(x - xa), db = abs(x - xb);
    if (da <= tol && da <= db) return 2;
    if (db <= tol) return 3;
    return 0;
}

static void ShowContextMenu(HWND hwnd, int x, int y) {
    if (!g.loaded || g.exporting) return;
    HMENU m = CreatePopupMenu();
    UINT selFlags = HasSel() ? MF_ENABLED : MF_GRAYED;
    std::wstring exp = HasSel()
        ? L"✂  Exportar este trecho…  (" + FmtTime(g.selB - g.selA) + L")\tCtrl+E"
        : L"✂  Exportar trecho  (selecione arrastando primeiro)";
    AppendMenuW(m, MF_STRING | selFlags, ID_EXPORT, exp.c_str());
    AppendMenuW(m, MF_STRING | selFlags, ID_PLAYSEL, L"▶  Tocar trecho\tEnter");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING | selFlags, ID_CLEARSEL, L"Limpar seleção\tEsc");
    if (HasSel()) SetMenuDefaultItem(m, ID_EXPORT, FALSE);
    POINT p = {x, y};
    ClientToScreen(hwnd, &p);
    SetForegroundWindow(g.hwnd);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, g.hwnd, nullptr);
    DestroyMenu(m);
    if (cmd) SendMessageW(g.hwnd, WM_COMMAND, cmd, 0);
}

static LRESULT CALLBACK TimelineProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        PaintTimeline(mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT && g.loaded && !g.exporting) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(hwnd, &p);
            bool edge = g.dragMode == 2 || g.dragMode == 3 || (g.dragMode == 0 && NearEdge(p.x));
            SetCursor(LoadCursor(nullptr, edge ? IDC_SIZEWE : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN: {
        if (!g.loaded || g.exporting) return 0;
        SetFocus(g.hwnd);
        SetCapture(hwnd);
        int x = GET_X_LPARAM(lp);
        g.wasPlaying = IsPlaying();
        Pause();
        g.dragMode = NearEdge(x);
        if (!g.dragMode) {
            g.dragMode = 1;
            g.anchor = XToTime(x);
            g.downX = x;
            g.moved = false;
            Seek(g.anchor);
        } else {
            Seek(g.dragMode == 2 ? g.selA : g.selB);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!g.dragMode) return 0;
        int x = GET_X_LPARAM(lp);
        double t = XToTime(x);
        if (g.dragMode == 1) {
            if (abs(x - g.downX) > S(3)) g.moved = true;
            if (g.moved) {
                g.selA = std::min(g.anchor, t);
                g.selB = std::max(g.anchor, t);
            }
        } else if (g.dragMode == 2) {
            g.selA = t;
            if (g.selA > g.selB) { std::swap(g.selA, g.selB); g.dragMode = 3; }
        } else {
            g.selB = t;
            if (g.selB < g.selA) { std::swap(g.selA, g.selB); g.dragMode = 2; }
        }
        Seek(t);
        return 0;
    }
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED: {
        if (!g.dragMode) return 0;
        int mode = g.dragMode;
        g.dragMode = 0;
        if (msg == WM_LBUTTONUP) ReleaseCapture();
        if (HasSel() && g.selB - g.selA < 0.05) g.selA = g.selB = -1;
        if (mode == 1 && !g.moved && g.wasPlaying) Play();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_RBUTTONUP:
        ShowContextMenu(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static LRESULT CALLBACK VideoProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        if (g.engine && g.loaded && g.engine->IsPaused()) {
            // repinta o quadro atual após redimensionar/descobrir a janela
            UpdateVideoRect();
        }
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetFocus(g.hwnd);
        TogglePlay();
        return 0;
    case WM_RBUTTONUP:
        ShowContextMenu(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Janela principal

static void MakeFonts() {
    if (g.font) DeleteObject(g.font);
    if (g.fontSmall) DeleteObject(g.fontSmall);
    g.font = CreateFontW(-S(14), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g.fontSmall = CreateFontW(-S(12), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    for (HWND h : {g.hBtnOpen, g.hBtnPlay, g.hLabel, g.hBtnCancel})
        if (h) SendMessageW(h, WM_SETFONT, (WPARAM)g.font, TRUE);
}

static void Layout() {
    if (!g.hwnd) return;
    RECT rc;
    GetClientRect(g.hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    int top = S(44), tl = S(100), pad = S(8);
    MoveWindow(g.hBtnOpen, pad, S(8), S(110), S(28), TRUE);
    MoveWindow(g.hBtnPlay, pad + S(116), S(8), S(84), S(28), TRUE);
    int lx = pad + S(212);
    int right = W - pad;
    MoveWindow(g.hBtnCancel, right - S(84), S(8), S(84), S(28), TRUE);
    MoveWindow(g.hProgress, right - S(84) - S(8) - S(180), S(13), S(180), S(18), TRUE);
    if (g.exporting) right -= S(84 + 8 + 180 + 8);
    MoveWindow(g.hLabel, lx, S(13), std::max(0, right - lx), S(20), TRUE);
    MoveWindow(g.hTimeline, 0, H - tl, W, tl, TRUE);

    g.videoArea = {0, top, W, std::max<LONG>(top, H - tl)};
    int aw = g.videoArea.right - g.videoArea.left, ah = g.videoArea.bottom - g.videoArea.top;
    if (g.vw && g.vh && aw > 0 && ah > 0) {
        double s = std::min((double)aw / g.vw, (double)ah / g.vh);
        int w = std::max(1, (int)(g.vw * s)), h = std::max(1, (int)(g.vh * s));
        MoveWindow(g.hVideo, g.videoArea.left + (aw - w) / 2, g.videoArea.top + (ah - h) / 2, w, h, TRUE);
        UpdateVideoRect();
    }
    InvalidateRect(g.hwnd, nullptr, TRUE);
}

static void UpdateLabel() {
    std::wstring s;
    if (g.exporting) {
        int pct = (int)SendMessageW(g.hProgress, PBM_GETPOS, 0, 0) / 10;
        s = L"Exportando corte… " + std::to_wstring(pct) + L"%";
    } else if (g.loaded) {
        s = FmtTime(g.cursor) + L" / " + FmtTime(g.duration);
        if (HasSel())
            s += L"     Trecho: " + FmtTime(g.selA) + L" → " + FmtTime(g.selB) + L"  (" + FmtTime(g.selB - g.selA) + L")";
    }
    if (s != g.lastLabel) {
        g.lastLabel = s;
        SetWindowTextW(g.hLabel, s.c_str());
    }
    const wchar_t* play = IsPlaying() ? L"❚❚  Pausar" : L"▶  Play";
    wchar_t cur[32];
    GetWindowTextW(g.hBtnPlay, cur, 32);
    if (wcscmp(cur, play)) SetWindowTextW(g.hBtnPlay, play);
}

static void OnTick() {
    if (!g.loaded) return;
    FlushSeek();
    if (!g.dragMode && !g.hasPendingSeek && !g.engine->IsSeeking()) {
        double t = g.engine->GetCurrentTime();
        if (g.playSel && t >= g.selB) {
            Pause();
            g.engine->SetCurrentTime(g.selB);
            t = g.selB;
        }
        if (fabs(t - g.cursor) > 1e-4) {
            g.cursor = t;
            InvalidateRect(g.hTimeline, nullptr, FALSE);
        }
    }
    UpdateLabel();
}

static void BrowseOpen() {
    if (g.exporting) return;
    wchar_t file[32768] = L"";
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = g.hwnd;
    ofn.lpstrFilter = L"Vídeos (*.mp4;*.m4v;*.mov;*.mkv;*.avi;*.wmv)\0*.mp4;*.m4v;*.mov;*.mkv;*.avi;*.wmv\0"
                      L"Todos os arquivos (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = ARRAYSIZE(file);
    ofn.lpstrTitle = L"Abrir vídeo";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn)) OpenFile(file);
}

static void StepFrames(int dir, bool bigStep) {
    if (!g.loaded) return;
    Pause();
    double step = bigStep ? 1.0 : 1.0 / std::max(1.0, g.fps);
    Seek(g.cursor + dir * step);
}

static bool HandleKey(WPARAM vk) {
    if (GetForegroundWindow() != g.hwnd) return false;
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    if (g.exporting) {
        if (vk == VK_ESCAPE) { g.cancel = true; return true; }
        return false;
    }
    if (ctrl && vk == 'O') { BrowseOpen(); return true; }
    if (!g.loaded) return false;
    switch (vk) {
    case VK_SPACE: TogglePlay(); return true;
    case VK_RETURN: PlaySelection(); return true;
    case VK_LEFT: StepFrames(-1, ctrl || shift); return true;
    case VK_RIGHT: StepFrames(+1, ctrl || shift); return true;
    case VK_HOME: Pause(); Seek(0); return true;
    case VK_END: Pause(); Seek(g.duration); return true;
    case VK_ESCAPE: g.selA = g.selB = -1; InvalidateRect(g.hTimeline, nullptr, FALSE); return true;
    case 'E': if (ctrl) { StartExport(); return true; } return false;
    case 'I':
        g.selA = g.cursor;
        if (g.selB <= g.selA) g.selB = g.duration;
        InvalidateRect(g.hTimeline, nullptr, FALSE);
        return true;
    case 'O':
        g.selB = g.cursor;
        if (g.selA < 0 || g.selA >= g.selB) g.selA = 0;
        InvalidateRect(g.hTimeline, nullptr, FALSE);
        return true;
    }
    return false;
}

static LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g.hwnd = hwnd;
        g.dpi = GetDpiForWindow(hwnd);
        HINSTANCE hi = ((CREATESTRUCTW*)lp)->hInstance;
        g.hBtnOpen = CreateWindowW(L"BUTTON", L"📂  Abrir vídeo", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, (HMENU)ID_OPEN, hi, nullptr);
        g.hBtnPlay = CreateWindowW(L"BUTTON", L"▶  Play", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, (HMENU)ID_PLAY, hi, nullptr);
        g.hLabel = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, 0, 0, 0, 0, hwnd, nullptr, hi, nullptr);
        g.hProgress = CreateWindowW(PROGRESS_CLASSW, L"", WS_CHILD, 0, 0, 0, 0, hwnd, nullptr, hi, nullptr);
        SendMessageW(g.hProgress, PBM_SETRANGE32, 0, 1000);
        g.hBtnCancel = CreateWindowW(L"BUTTON", L"Cancelar", WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, hwnd, (HMENU)ID_CANCEL, hi, nullptr);
        g.hVideo = CreateWindowW(L"CRVideo", L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, hwnd, nullptr, hi, nullptr);
        g.hTimeline = CreateWindowW(L"CRTimeline", L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd, nullptr, hi, nullptr);
        MakeFonts();
        DragAcceptFiles(hwnd, TRUE);
        HRESULT hr = CreateEngine();
        if (FAILED(hr)) {
            MessageBoxW(hwnd, (L"Media Foundation indisponível neste Windows.\n"
                               L"(Edições N precisam do \"Media Feature Pack\".)\n\n" + HrText(hr)).c_str(),
                        kAppName, MB_ICONERROR);
            return -1;
        }
        SetTimer(hwnd, TIMER_TICK, 30, nullptr);
        return 0;
    }
    case WM_SIZE:
        Layout();
        return 0;
    case WM_GETMINMAXINFO:
        ((MINMAXINFO*)lp)->ptMinTrackSize = {S(560), S(360)};
        return 0;
    case WM_DPICHANGED: {
        g.dpi = HIWORD(wp);
        MakeFonts();
        RECT* r = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, g.brBg);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (!g.loaded) {
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, C_DIM);
            SelectObject(dc, g.font);
            const wchar_t* text = g.path.empty() ? L"Arraste um vídeo MP4 para cá\nou clique em “Abrir vídeo”" : L"Carregando…";
            RECT r = g.videoArea;
            DrawTextW(dc, text, -1, &r, DT_CENTER | DT_NOPREFIX | DT_CALCRECT);
            int h = r.bottom - r.top;
            r = g.videoArea;
            r.top += (r.bottom - r.top - h) / 2;
            DrawTextW(dc, text, -1, &r, DT_CENTER | DT_NOPREFIX);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        SetTextColor((HDC)wp, C_TEXT);
        SetBkColor((HDC)wp, C_BG);
        return (LRESULT)g.brBg;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_OPEN: BrowseOpen(); break;
        case ID_PLAY: TogglePlay(); SetFocus(hwnd); break;
        case ID_CANCEL: g.cancel = true; break;
        case ID_EXPORT: StartExport(); break;
        case ID_PLAYSEL: PlaySelection(); break;
        case ID_CLEARSEL: g.selA = g.selB = -1; InvalidateRect(g.hTimeline, nullptr, FALSE); break;
        }
        return 0;
    case WM_TIMER:
        OnTick();
        return 0;
    case WM_APP_ENGINE:
        OnEngineEvent((DWORD)wp);
        return 0;
    case WM_APP_PROGRESS:
        SendMessageW(g.hProgress, PBM_SETPOS, wp, 0);
        UpdateLabel();
        return 0;
    case WM_APP_DONE:
        OnExportDone((HRESULT)wp);
        return 0;
    case WM_DROPFILES: {
        wchar_t file[32768];
        if (DragQueryFileW((HDROP)wp, 0, file, ARRAYSIZE(file))) OpenFile(file);
        DragFinish((HDROP)wp);
        return 0;
    }
    case WM_CLOSE:
        if (g.exporting) {
            if (MessageBoxW(hwnd, L"Uma exportação está em andamento. Cancelar e sair?", kAppName, MB_ICONQUESTION | MB_YESNO) != IDYES)
                return 0;
            g.cancel = true;
            if (g.worker.joinable()) g.worker.join();
            DeleteFileW(g.exportPath.c_str());
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_TICK);
        if (g.engine) g.engine->Shutdown();
        g.engineEx.Reset();
        g.engine.Reset();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int show) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(MFStartup(MF_VERSION))) {
        MessageBoxW(nullptr, L"Media Foundation indisponível neste Windows.\n(Edições N precisam do \"Media Feature Pack\".)",
                    kAppName, MB_ICONERROR);
        return 1;
    }
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    g.brBg = CreateSolidBrush(C_BG);
    HICON icon = LoadIconW(hi, MAKEINTRESOURCEW(1));

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = g.brBg;
    wc.lpszClassName = L"CorteRapido";
    wc.hIcon = icon;
    wc.hIconSm = icon;
    RegisterClassExW(&wc);

    wc.lpfnWndProc = VideoProc;
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"CRVideo";
    wc.hCursor = LoadCursor(nullptr, IDC_HAND);
    RegisterClassExW(&wc);

    wc.lpfnWndProc = TimelineProc;
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"CRTimeline";
    wc.hCursor = nullptr;
    wc.style = 0;
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    HWND hwnd = CreateWindowExW(0, L"CorteRapido", kAppName, WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, MulDiv(1100, dpi, 96), MulDiv(720, dpi, 96),
                                nullptr, nullptr, hi, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) OpenFile(argv[1]);
    if (argv) LocalFree(argv);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (m.message == WM_KEYDOWN && HandleKey(m.wParam)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    MFShutdown();
    CoUninitialize();
    return 0;
}

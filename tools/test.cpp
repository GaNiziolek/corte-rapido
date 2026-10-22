// Harness de teste: gera um MP4 sintético e exercita ExportClip sem GUI.
//   test.exe gen <saida.mp4> <segundos>
//   test.exe cut <entrada.mp4> <saida.mp4> <inicio> <fim>
//   test.exe info <arquivo.mp4>
#define wWinMain AppMain
#include "../src/main.cpp"
#undef wWinMain
#include <cstdio>
#pragma comment(lib, "mf.lib")

static HRESULT Gen(const wchar_t* out, double secs) {
    const UINT32 W = 640, H = 360, FPS = 30, RATE = 48000;
    ComPtr<IMFSinkWriter> w;
    HRESULT hr = MFCreateSinkWriterFromURL(out, nullptr, nullptr, &w);
    if (FAILED(hr)) return hr;
    ComPtr<IMFMediaType> vo, vi, ao, ai;
    MFCreateMediaType(&vo);
    vo->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    vo->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    vo->SetUINT32(MF_MT_AVG_BITRATE, 2000000);
    vo->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(vo.Get(), MF_MT_FRAME_SIZE, W, H);
    MFSetAttributeRatio(vo.Get(), MF_MT_FRAME_RATE, FPS, 1);
    MFSetAttributeRatio(vo.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    DWORD vs, as;
    if (FAILED(hr = w->AddStream(vo.Get(), &vs))) return hr;
    MFCreateMediaType(&vi);
    vi->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    vi->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    vi->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(vi.Get(), MF_MT_FRAME_SIZE, W, H);
    MFSetAttributeRatio(vi.Get(), MF_MT_FRAME_RATE, FPS, 1);
    MFSetAttributeRatio(vi.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    if (FAILED(hr = w->SetInputMediaType(vs, vi.Get(), nullptr))) return hr;

    MFCreateMediaType(&ao);
    ao->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    ao->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    ao->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    ao->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, RATE);
    ao->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    ao->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16000);
    if (FAILED(hr = w->AddStream(ao.Get(), &as))) return hr;
    MFCreateMediaType(&ai);
    ai->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    ai->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    ai->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    ai->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, RATE);
    ai->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    ai->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    ai->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, RATE * 4);
    if (FAILED(hr = w->SetInputMediaType(as, ai.Get(), nullptr))) return hr;
    if (FAILED(hr = w->BeginWriting())) return hr;

    const int frames = (int)(secs * FPS);
    const LONGLONG fdur = 10000000 / FPS;
    const UINT32 spf = RATE / FPS;
    for (int f = 0; f < frames; ++f) {
        ComPtr<IMFMediaBuffer> b;
        MFCreateMemoryBuffer(W * H * 4, &b);
        BYTE* p;
        b->Lock(&p, nullptr, nullptr);
        int sec = f / FPS;
        DWORD bg = (sec % 3 == 0) ? 0x203060 : (sec % 3 == 1) ? 0x206030 : 0x602030;
        DWORD* px = (DWORD*)p;
        int barX = (int)((f % FPS) * (W - 40) / (double)FPS);
        for (UINT32 y = 0; y < H; ++y)
            for (UINT32 x = 0; x < W; ++x) {
                DWORD c = bg;
                if ((int)x >= barX && (int)x < barX + 40) c = 0xFFFFFF;
                // contador de segundos em blocos no topo
                if (y < 30 && x < (UINT32)(sec + 1) * 20 && x % 20 < 16) c = 0xFFCC00;
                px[(H - 1 - y) * W + x] = c;  // RGB32 é bottom-up
            }
        b->Unlock();
        b->SetCurrentLength(W * H * 4);
        ComPtr<IMFSample> s;
        MFCreateSample(&s);
        s->AddBuffer(b.Get());
        s->SetSampleTime(f * fdur);
        s->SetSampleDuration(fdur);
        if (FAILED(hr = w->WriteSample(vs, s.Get()))) return hr;

        ComPtr<IMFMediaBuffer> ab;
        MFCreateMemoryBuffer(spf * 4, &ab);
        ab->Lock(&p, nullptr, nullptr);
        short* sp = (short*)p;
        double freq = 220.0 * (1 + sec % 4);
        for (UINT32 i = 0; i < spf; ++i) {
            double t = (f * (double)spf + i) / RATE;
            short v = (short)(8000 * sin(2 * 3.14159265 * freq * t));
            sp[i * 2] = sp[i * 2 + 1] = v;
        }
        ab->Unlock();
        ab->SetCurrentLength(spf * 4);
        ComPtr<IMFSample> as2;
        MFCreateSample(&as2);
        as2->AddBuffer(ab.Get());
        as2->SetSampleTime(f * fdur);
        as2->SetSampleDuration(fdur);
        if (FAILED(hr = w->WriteSample(as, as2.Get()))) return hr;
    }
    return w->Finalize();
}

static void Info(const wchar_t* file) {
    ComPtr<IMFSourceReader> r;
    if (FAILED(MFCreateSourceReaderFromURL(file, nullptr, &r))) { wprintf(L"nao abriu\n"); return; }
    PROPVARIANT v;
    PropVariantInit(&v);
    ComPtr<IMFMediaSource> src;
    r->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &v);
    wprintf(L"duracao: %.3f s\n", v.uhVal.QuadPart / 1e7);
    PropVariantClear(&v);
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> mt;
        if (FAILED(r->GetNativeMediaType(i, 0, &mt))) break;
        GUID major, sub;
        mt->GetGUID(MF_MT_MAJOR_TYPE, &major);
        mt->GetGUID(MF_MT_SUBTYPE, &sub);
        if (major == MFMediaType_Video) {
            UINT32 w, h, n, d;
            MFGetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, &w, &h);
            MFGetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, &n, &d);
            wprintf(L"video: %ux%u @ %u/%u %s bitrate=%u\n", w, h, n, d, sub == MFVideoFormat_H264 ? L"H264" : L"?",
                    MFGetAttributeUINT32(mt.Get(), MF_MT_AVG_BITRATE, 0));
        } else if (major == MFMediaType_Audio) {
            wprintf(L"audio: %u Hz %u ch %s\n", MFGetAttributeUINT32(mt.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0),
                    MFGetAttributeUINT32(mt.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0), sub == MFAudioFormat_AAC ? L"AAC" : L"?");
        }
    }
    // primeiro e último timestamp de vídeo
    r->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
    r->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);
    LONGLONG first = -1, last = -1;
    int n = 0;
    for (;;) {
        DWORD si, fl;
        LONGLONG ts;
        ComPtr<IMFSample> s;
        if (FAILED(r->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &si, &fl, &ts, &s)) || (fl & MF_SOURCE_READERF_ENDOFSTREAM)) break;
        if (!s) continue;
        if (first < 0) first = ts;
        last = ts;
        ++n;
    }
    wprintf(L"quadros: %d, primeiro=%.3f ultimo=%.3f\n", n, first / 1e7, last / 1e7);
}

#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

// Captura só a janela do app (PrintWindow com PW_RENDERFULLCONTENT) em PNG.
static int Shot(const wchar_t* file) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    HWND h = FindWindowW(L"#32770", L"Salvar corte como");
    if (!h) h = FindWindowW(L"CorteRapido", nullptr);
    if (!h) return 1;
    RECT r;
    GetWindowRect(h, &r);
    int w = r.right - r.left, hh = r.bottom - r.top;
    HDC sdc = GetDC(nullptr), mdc = CreateCompatibleDC(sdc);
    HBITMAP bmp = CreateCompatibleBitmap(sdc, w, hh);
    SelectObject(mdc, bmp);
    PrintWindow(h, mdc, 2);
    ULONG_PTR tok;
    Gdiplus::GdiplusStartupInput in;
    Gdiplus::GdiplusStartup(&tok, &in, nullptr);
    {
        Gdiplus::Bitmap b(bmp, nullptr);
        CLSID png;
        CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &png);
        b.Save(file, &png);
    }
    Gdiplus::GdiplusShutdown(tok);
    return 0;
}

// Arrasta na linha do tempo de fração f1 a f2 da largura da trilha; "rclick f" abre o menu.
static int Mouse(const wchar_t* op, double f1, double f2) {
    HWND h = FindWindowW(L"CorteRapido", nullptr);
    HWND tl = FindWindowExW(h, nullptr, L"CRTimeline", nullptr);
    if (!tl) return 1;
    UINT dpi = GetDpiForWindow(tl);
    RECT rc;
    GetClientRect(tl, &rc);
    int L = MulDiv(14, dpi, 96), R = rc.right - L, y = MulDiv(40, dpi, 96);
    auto X = [&](double f) { return L + (int)(f * (R - L)); };
    if (!wcscmp(op, L"drag")) {
        SendMessageW(tl, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(X(f1), y));
        for (int i = 1; i <= 10; ++i) {
            SendMessageW(tl, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(X(f1 + (f2 - f1) * i / 10), y));
            Sleep(30);
        }
        SendMessageW(tl, WM_LBUTTONUP, 0, MAKELPARAM(X(f2), y));
    } else if (!wcscmp(op, L"rclick")) {
        PostMessageW(tl, WM_RBUTTONUP, 0, MAKELPARAM(X(f1), y));
    } else if (!wcscmp(op, L"key")) {
        PostMessageW(h, WM_KEYDOWN, (WPARAM)f1, 0);
    } else if (!wcscmp(op, L"menu")) {
        HWND m = FindWindowW(L"#32768", nullptr);
        wprintf(L"menu aberto: %s\n", m && IsWindowVisible(m) ? L"sim" : L"nao");
        PostMessageW(h, WM_CANCELMODE, 0, 0);
    } else if (!wcscmp(op, L"cmd")) {
        PostMessageW(h, WM_COMMAND, (WPARAM)f1, 0);
    } else if (!wcscmp(op, L"save")) {
        HWND d = nullptr;
        for (int i = 0; i < 50 && !d; ++i, Sleep(100)) d = FindWindowW(L"#32770", L"Salvar corte como");
        if (!d) { wprintf(L"dialogo nao encontrado\n"); return 1; }
        wprintf(L"dialogo encontrado\n");
        HWND ok = GetDlgItem(d, IDOK);
        wprintf(L"botao salvar: %p\n", (void*)ok);
        if (ok) PostMessageW(d, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), (LPARAM)ok);
        Sleep(1500);
        wprintf(L"dialogo ainda aberto: %s\n", IsWindow(d) ? L"sim" : L"nao");
    }
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc >= 3 && !wcscmp(argv[1], L"shot")) return Shot(argv[2]);
    if (argc >= 3 && (!wcscmp(argv[1], L"drag") || !wcscmp(argv[1], L"rclick") || !wcscmp(argv[1], L"key") ||
                      !wcscmp(argv[1], L"menu") || !wcscmp(argv[1], L"cmd") || !wcscmp(argv[1], L"save")))
        return Mouse(argv[1], _wtof(argv[2]), argc > 3 ? _wtof(argv[3]) : 0);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);
    HRESULT hr = S_OK;
    if (argc >= 4 && !wcscmp(argv[1], L"gen")) hr = Gen(argv[2], _wtof(argv[3]));
    else if (argc >= 6 && !wcscmp(argv[1], L"cut")) {
        DWORD t0 = GetTickCount();
        hr = ExportClip(argv[2], argv[3], _wtof(argv[4]), _wtof(argv[5]), nullptr);
        wprintf(L"tempo: %u ms\n", GetTickCount() - t0);
    } else if (argc >= 3 && !wcscmp(argv[1], L"info")) Info(argv[2]);
    wprintf(L"hr=0x%08X %s\n", (unsigned)hr, HrText(hr).c_str());
    MFShutdown();
    return FAILED(hr);
}

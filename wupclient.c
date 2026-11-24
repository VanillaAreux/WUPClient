/* WUP Stream Client: mirror your Windows desktop (and its sound) to a Wii U GamePad running WUP Stream.
 * Uses libwupstream (../host). Build with build.sh in an MSYS2 UCRT64 shell. GPL-3.0-or-later. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <shellapi.h>
#include <commdlg.h>
#include <urlmon.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propsys.h>
#include <wchar.h>

#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "wupstream.h"
#include "ViGEm/Client.h"

static HWND g_win;
static void status(const char *fmt, ...);
#define WUPCLIENT_VERSION "2.3.3"
#define WM_UPDATE_AVAILABLE (WM_APP + 5)
typedef struct { char tag[64], url[2048], digest[80]; } update_info;
static const char RELEASE_API[] = "https://api.github.com/repos/VanillaAreux/WUPClient/releases/latest";

static int json_string(const char *json, const char *key, char *out, size_t cap) {
    char needle[80]; snprintf(needle, sizeof needle, "\"%s\"", key);
    const char *p = strstr(json, needle); if (!p) return 0;
    p += strlen(needle);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p++ != ':') return 0;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p++ != '"') return 0;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) {
        if (*p == '\\' && p[1]) p++;
        out[n++] = *p++;
    }
    out[n] = 0; return *p == '"';
}
static int newer_version(const char *tag) {
    int a=0,b=0,c=0,x=0,y=0,z=0;
    if (sscanf(tag, "v%d.%d.%d", &a,&b,&c) != 3 && sscanf(tag, "V%d.%d.%d", &a,&b,&c) != 3) return 0;
    if (sscanf(WUPCLIENT_VERSION, "%d.%d.%d", &x,&y,&z) != 3) return 0;
    return a != x ? a > x : b != y ? b > y : c > z;
}
static int sha256_file(const char *path, char hex[65]) {
    BCRYPT_ALG_HANDLE alg=NULL; BCRYPT_HASH_HANDLE hash=NULL;
    DWORD objlen=0, got=0; BYTE *obj=NULL, digest[32], buf[65536]; int ok=0;
    HANDLE f=CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f==INVALID_HANDLE_VALUE) return 0;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0) goto done;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&objlen, sizeof objlen, &got, 0) < 0) goto done;
    obj=(BYTE*)malloc(objlen); if (!obj) goto done;
    if (BCryptCreateHash(alg,&hash,obj,objlen,NULL,0,0)<0) goto done;
    for (;;) { DWORD n=0; if (!ReadFile(f,buf,sizeof buf,&n,NULL)) goto done; if (!n) break;
        if (BCryptHashData(hash,buf,n,0)<0) goto done; }
    if (BCryptFinishHash(hash,digest,sizeof digest,0)<0) goto done;
    for (int i=0;i<32;i++) sprintf(hex+i*2,"%02x",digest[i]); hex[64]=0; ok=1;
done:
    if (hash) BCryptDestroyHash(hash); if (alg) BCryptCloseAlgorithmProvider(alg,0);
    free(obj); CloseHandle(f); return ok;
}
static DWORD WINAPI install_update_thread(LPVOID param) {
    update_info *u=(update_info*)param;
    char tmp[MAX_PATH], exe[MAX_PATH], bat[MAX_PATH], jsonpath[MAX_PATH], actual[65];
    GetTempPathA(MAX_PATH,tmp);
    snprintf(exe,sizeof exe,"%swupclient-update.exe",tmp);
    snprintf(bat,sizeof bat,"%swupclient-update.bat",tmp);
    snprintf(jsonpath,sizeof jsonpath,"%swupclient-update.json",tmp);
    DeleteFileA(exe);
    if (URLDownloadToFileA(NULL,u->url,exe,0,NULL)!=S_OK || !sha256_file(exe,actual) ||
        _stricmp(actual,u->digest+7)!=0) {
        DeleteFileA(exe);
        MessageBoxA(g_win,"The update download failed verification. Your installed copy was not changed.",
                    "WUP Stream Client updater",MB_OK|MB_ICONERROR);
        free(u); return 0;
    }
    GetModuleFileNameA(NULL, jsonpath, sizeof jsonpath); /* reuse buffer for the installed executable path */
    FILE *f=fopen(bat,"w");
    if (!f) { DeleteFileA(exe); free(u); return 0; }
    fprintf(f,"@echo off\r\n"
              "ping 127.0.0.1 -n 10 >nul\r\n"
              "copy /y \"%s\" \"%s\" >nul\r\n"
              "if errorlevel 1 exit /b 1\r\n"
              "start \"\" \"%s\"\r\n"
              "del \"%%~f0\"\r\n",exe,jsonpath,jsonpath);
    fclose(f);
    HINSTANCE launched=ShellExecuteA(NULL,"open",bat,NULL,NULL,SW_HIDE);
    if ((INT_PTR)launched<=32) {
        DeleteFileA(bat); DeleteFileA(exe);
        MessageBoxA(g_win,"The update was downloaded and verified, but Windows could not start the installer helper. The installed copy was not changed.",
                    "WUP Stream Client updater",MB_OK|MB_ICONERROR);
        free(u); return 0;
    }
    free(u);
    PostMessageA(g_win,WM_CLOSE,0,0);
    return 0;
}
static DWORD WINAPI check_updates_thread(LPVOID unused) {
    (void)unused;
    char tmp[MAX_PATH], path[MAX_PATH], *json=NULL; DWORD size=0; update_info *u=NULL;
    status("Checking GitHub for updates...");
    if (!GetTempPathA(MAX_PATH,tmp)) { status("Update check failed: cannot access the Windows temp folder."); return 0; }
    snprintf(path,sizeof path,"%swupclient-release.json",tmp);
    if (URLDownloadToFileA(NULL,RELEASE_API,path,0,NULL)!=S_OK) {
        status("Update check failed: GitHub could not be reached. Check your internet connection.");
        return 0;
    }
    FILE *f=fopen(path,"rb"); if (!f) { DeleteFileA(path); status("Update check failed: could not read GitHub's response."); return 0; }
    fseek(f,0,SEEK_END); long len=ftell(f); rewind(f);
    if (len<=0 || len>1024*1024) { fclose(f); DeleteFileA(path); status("Update check failed: GitHub returned an invalid response."); return 0; }
    json=(char*)malloc((size_t)len+1); if (!json) { fclose(f); DeleteFileA(path); status("Update check failed: not enough memory."); return 0; }
    size=(DWORD)fread(json,1,(size_t)len,f); fclose(f); DeleteFileA(path); json[size]=0;
    u=(update_info*)calloc(1,sizeof *u);
    if (u && json_string(json,"tag_name",u->tag,sizeof u->tag)) {
        if (newer_version(u->tag)) {
            if (json_string(json,"browser_download_url",u->url,sizeof u->url) &&
                json_string(json,"digest",u->digest,sizeof u->digest) &&
                strncmp(u->digest,"sha256:",7)==0) {
                if (PostMessageA(g_win,WM_UPDATE_AVAILABLE,0,(LPARAM)u)) u=NULL;
                else status("Update found, but the notification could not be displayed.");
            } else status("Update found, but its release asset is missing a download URL or SHA-256 digest.");
        } else {
            char msg[160]; snprintf(msg,sizeof msg,"No newer release found. Installed %s; latest GitHub release %s.",WUPCLIENT_VERSION,u->tag);
            status("%s",msg);
        }
    } else status("Update check failed: could not parse GitHub's release information.");
    free(u); free(json); return 0;
}

#define ID_START 101
#define ID_AUDIO 102
#define ID_IP    103
#define ID_CTRL  104
#define ID_PTR   105
#define WM_SETTINGS_CHANGED (WM_APP + 7)
#define WM_STATUS (WM_APP + 1)
#define WM_STOPPED (WM_APP + 2)
#define WM_NEEDDRIVER (WM_APP + 4)   /* the ViGEmBus driver is missing: offer to install it */
#define WM_SETMODE (WM_APP + 3)   /* wParam = new MODE_*, sent from the stream thread when the GamePad cycles profiles */

static const GUID MY_CLSID_Enum = {0xBCDE0395,0xE52F,0x467C,{0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E}};
static const GUID MY_IID_Enum   = {0xA95664D2,0x9614,0x4F35,{0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6}};
static const GUID MY_IID_Client = {0x1CB9AD4C,0xDBFA,0x4C32,{0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2}};
static const GUID MY_IID_Capture= {0xC8ADBD64,0xE71E,0x48A0,{0xA4,0xDE,0x18,0x5C,0x39,0x5C,0xD3,0x17}};

static HWND g_btn, g_audio, g_ip, g_status, g_ctrl, g_ptr;
static volatile LONG g_drawptr = 1;   /* draw our own mouse pointer on the frame (off if the capture already contains one) */
static volatile LONG g_run;         /* 1 while the user wants streaming */
static HANDLE g_thread;
static char g_ipstr[64];
static int g_useaudio;
static volatile LONG g_settings_menu, g_mic_enabled;
static volatile LONG g_stream_kbps = 4500;
static volatile LONG g_cable_setup_running;
static int g_settings_sel;

#define VBCABLE_URL "https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack46.zip"

/* Check for VB-CABLE's playback endpoint before offering installation. */
static int vbcable_installed(void) {
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = NULL; IMMDeviceCollection *devices = NULL;
    int found = 0;
    if (FAILED(CoCreateInstance(&MY_CLSID_Enum, NULL, CLSCTX_ALL, &MY_IID_Enum, (void **)&en))) goto done;
    if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &devices))) goto done;
    UINT count = 0; IMMDeviceCollection_GetCount(devices, &count);
    for (UINT i = 0; i < count && !found; i++) {
        IMMDevice *dev = NULL; IPropertyStore *store = NULL; PROPVARIANT name;
        PropVariantInit(&name);
        if (SUCCEEDED(IMMDeviceCollection_Item(devices, i, &dev)) &&
            SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &store)) &&
            SUCCEEDED(IPropertyStore_GetValue(store, &PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR) {
            WCHAR lower[256]; wcsncpy(lower, name.pwszVal, 255); lower[255] = 0;
            CharLowerBuffW(lower, (DWORD)wcslen(lower));
            found = wcsstr(lower, L"cable input") != NULL;
        }
        PropVariantClear(&name);
        if (store) IPropertyStore_Release(store);
        if (dev) IMMDevice_Release(dev);
    }
done:
    if (devices) IMMDeviceCollection_Release(devices);
    if (en) IMMDeviceEnumerator_Release(en);
    if (SUCCEEDED(co)) CoUninitialize();
    return found;
}

static DWORD WINAPI install_vbcable_thread(LPVOID unused) {
    (void)unused;
    if (vbcable_installed()) {
        status("VB-CABLE is already installed.");
        InterlockedExchange(&g_cable_setup_running, 0);
        return 0;
    }
    char tmp[MAX_PATH], script[MAX_PATH];
    if (!GetTempPathA(sizeof tmp, tmp) || snprintf(script, sizeof script, "%sWUPClient_VBCABLE_Setup.ps1", tmp) < 0 || strlen(script) >= sizeof script) {
        status("Could not prepare VB-CABLE setup.");
        InterlockedExchange(&g_cable_setup_running, 0);
        return 0;
    }
    const char *ps =
        "$ErrorActionPreference = 'Stop'\r\n"
        "$ProgressPreference = 'SilentlyContinue'\r\n"
        "$zip = Join-Path $env:TEMP 'WUPClient_VBCABLE_Driver_Pack46.zip'\r\n"
        "$dir = Join-Path $env:TEMP 'WUPClient_VBCABLE_Driver_Pack46'\r\n"
        "Invoke-WebRequest -UseBasicParsing -Uri '" VBCABLE_URL "' -OutFile $zip\r\n"
        "if (Test-Path $dir) { Remove-Item -LiteralPath $dir -Recurse -Force }\r\n"
        "Expand-Archive -LiteralPath $zip -DestinationPath $dir -Force\r\n"
        "$setup = Get-ChildItem -LiteralPath $dir -Filter 'VBCABLE_Setup_x64.exe' -Recurse | Select-Object -First 1\r\n"
        "if (-not $setup) { throw 'The VB-CABLE x64 setup program was not found in the official package.' }\r\n"
        "Start-Process -FilePath $setup.FullName -Verb RunAs -Wait\r\n";
    HANDLE file = CreateFileA(script, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        status("Could not create VB-CABLE setup script.");
        InterlockedExchange(&g_cable_setup_running, 0);
        return 0;
    }
    DWORD written = 0; BOOL wrote = WriteFile(file, ps, (DWORD)strlen(ps), &written, NULL);
    CloseHandle(file);
    if (!wrote || written != strlen(ps)) {
        status("Could not write VB-CABLE setup script.");
        InterlockedExchange(&g_cable_setup_running, 0);
        return 0;
    }
    char params[MAX_PATH + 100];
    snprintf(params, sizeof params, "-NoProfile -ExecutionPolicy Bypass -File \"%s\"", script);
    SHELLEXECUTEINFOA sei; ZeroMemory(&sei, sizeof sei);
    sei.cbSize = sizeof sei; sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = "open"; sei.lpFile = "powershell.exe"; sei.lpParameters = params;
    sei.nShow = SW_HIDE;
    status("Downloading official VB-CABLE package. Windows will ask before installing the driver...");
    if (!ShellExecuteExA(&sei) || !sei.hProcess) {
        status("Could not start VB-CABLE setup. Check PowerShell and Windows permissions.");
    } else {
        WaitForSingleObject(sei.hProcess, INFINITE);
        DWORD code = 1; GetExitCodeProcess(sei.hProcess, &code); CloseHandle(sei.hProcess);
        if (code == 0) status("VB-CABLE setup finished. Follow its prompts, then restart Windows if requested.");
        else status("VB-CABLE setup did not complete. Check the installer window and try again.");
    }
    InterlockedExchange(&g_cable_setup_running, 0);
    return 0;
}

static void status(const char *fmt, ...) {
    char buf[200]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    PostMessageA(g_win, WM_STATUS, 0, (LPARAM)_strdup(buf));
}

/* ---------------- audio: WASAPI loopback of the default output -> GamePad ---------------- */
typedef struct { wupstream *pad; volatile LONG *stop; } audio_arg;
typedef struct { wupstream *pad; volatile LONG *stop; } mic_arg;

static DWORD WINAPI audio_thread(LPVOID p) {
    audio_arg *a = p;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = NULL; IMMDevice *dev = NULL; IAudioClient *cl = NULL; IAudioCaptureClient *cap = NULL;
    WAVEFORMATEX *fmt = NULL;
    static int16_t out[48000 * 2];      /* up to one second per push */
    if (FAILED(CoCreateInstance(&MY_CLSID_Enum, NULL, CLSCTX_ALL, &MY_IID_Enum, (void **)&en))) goto done;
    if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev))) goto done;
    if (FAILED(IMMDevice_Activate(dev, &MY_IID_Client, CLSCTX_ALL, NULL, (void **)&cl))) goto done;
    if (FAILED(IAudioClient_GetMixFormat(cl, &fmt))) goto done;
    if (FAILED(IAudioClient_Initialize(cl, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, fmt, NULL))) goto done;
    if (FAILED(IAudioClient_GetService(cl, &MY_IID_Capture, (void **)&cap))) goto done;
    int ch = fmt->nChannels, rate = fmt->nSamplesPerSec, bits = fmt->wBitsPerSample;
    int isfloat = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && bits == 32 && ((WAVEFORMATEXTENSIBLE *)fmt)->SubFormat.Data1 == 3);
    IAudioClient_Start(cl);
    LARGE_INTEGER fq, last, now; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&last);
    double pos = 0;                    /* resampler position */
    float prevL = 0, prevR = 0;
    while (!*a->stop) {
        Sleep(8);
        UINT32 n = 0;
        if (FAILED(IAudioCaptureClient_GetNextPacketSize(cap, &n))) break;
        if (n == 0) {                  /* nothing is playing: keep the pad's sound buffer fed with silence */
            QueryPerformanceCounter(&now);
            int frames = (int)((now.QuadPart - last.QuadPart) * 48000 / fq.QuadPart);
            if (frames >= 480) { if (frames > 48000) frames = 48000; memset(out, 0, frames * 4);
                { if (wupstream_audio(a->pad, out, frames)) break; last = now; } }
            continue;
        }
        while (n > 0) {
            BYTE *data; UINT32 got; DWORD flags;
            if (FAILED(IAudioCaptureClient_GetBuffer(cap, &data, &got, &flags, NULL, NULL))) goto done;
            int o = 0;
            double step = (double)rate / 48000.0;
            for (UINT32 i = 0; i < got;) {
                float l = 0, r = 0;
                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                    if (isfloat) { const float *f = (const float *)data + (size_t)i * ch; l = f[0]; r = ch > 1 ? f[1] : f[0]; }
                    else if (bits == 16) { const int16_t *s = (const int16_t *)data + (size_t)i * ch; l = s[0] / 32768.f; r = (ch > 1 ? s[1] : s[0]) / 32768.f; }
                    else if (bits == 32) { const int32_t *s = (const int32_t *)data + (size_t)i * ch; l = s[0] / 2147483648.f; r = (ch > 1 ? s[1] : s[0]) / 2147483648.f; }
                    else if (bits == 24) { const BYTE *s = data + (size_t)i * ch * 3; int v = (s[0] << 8 | s[1] << 16 | s[2] << 24) >> 8;
                        int v2 = ch > 1 ? ((s[3] << 8 | s[4] << 16 | s[5] << 24) >> 8) : v; l = v / 8388608.f; r = v2 / 8388608.f; }
                }
                /* linear resample to 48 kHz (a no-op for the usual 48 kHz) */
                while (pos < 1.0 && o < 48000 - 1) {
                    float lv = prevL + (l - prevL) * (float)pos, rv = prevR + (r - prevR) * (float)pos;
                    lv = lv > 1 ? 1 : lv < -1 ? -1 : lv; rv = rv > 1 ? 1 : rv < -1 ? -1 : rv;
                    out[o * 2] = (int16_t)(lv * 32767); out[o * 2 + 1] = (int16_t)(rv * 32767); o++;
                    pos += step;
                }
                pos -= 1.0; prevL = l; prevR = r; i++;
                if (o >= 48000 - 2) break;
            }
            IAudioCaptureClient_ReleaseBuffer(cap, got);
            if (o > 0 && wupstream_audio(a->pad, out, o)) goto done;
            QueryPerformanceCounter(&last);
            if (FAILED(IAudioCaptureClient_GetNextPacketSize(cap, &n))) break;
        }
    }
done:
    if (cl) { IAudioClient_Stop(cl); IAudioClient_Release(cl); }
    if (cap) IAudioCaptureClient_Release(cap);
    if (fmt) CoTaskMemFree(fmt);
    if (dev) IMMDevice_Release(dev);
    if (en) IMMDeviceEnumerator_Release(en);
    CoUninitialize();
    return 0;
}


/* GamePad microphone -> VB-CABLE virtual input. Windows apps select "CABLE Output" as their microphone. */
static DWORD WINAPI mic_thread(LPVOID p) {
    mic_arg *a = (mic_arg *)p;
    HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    IMMDeviceEnumerator *en = NULL; IMMDeviceCollection *devices = NULL;
    IMMDevice *dev = NULL; IAudioClient *cl = NULL; IAudioRenderClient *render = NULL;
    WAVEFORMATEX *fmt = NULL; HANDLE audio_event = NULL; int ok = 0;
    int16_t fifo[48000], incoming[480];
    UINT fifo_read = 0, fifo_count = 0;
    double src_phase = 0.0;
    if (FAILED(CoCreateInstance(&MY_CLSID_Enum, NULL, CLSCTX_ALL, &MY_IID_Enum, (void **)&en))) goto done;
    if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &devices))) goto done;
    UINT count = 0; IMMDeviceCollection_GetCount(devices, &count);
    for (UINT i = 0; i < count && !dev; i++) {
        IMMDevice *candidate = NULL; IPropertyStore *store = NULL; PROPVARIANT name;
        PropVariantInit(&name);
        if (SUCCEEDED(IMMDeviceCollection_Item(devices, i, &candidate)) &&
            SUCCEEDED(IMMDevice_OpenPropertyStore(candidate, STGM_READ, &store)) &&
            SUCCEEDED(IPropertyStore_GetValue(store, &PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR) {
            WCHAR lower[256]; wcsncpy(lower, name.pwszVal, 255); lower[255] = 0;
            CharLowerBuffW(lower, (DWORD)wcslen(lower));
            if (wcsstr(lower, L"cable input")) { dev = candidate; candidate = NULL; }
        }
        PropVariantClear(&name);
        if (store) IPropertyStore_Release(store);
        if (candidate) IMMDevice_Release(candidate);
    }
    if (!dev) { status("GamePad mic: install VB-CABLE and use its CABLE Input playback device"); goto done; }
    if (FAILED(IMMDevice_Activate(dev, &MY_IID_Client, CLSCTX_ALL, NULL, (void **)&cl))) goto done;
    if (FAILED(IAudioClient_GetMixFormat(cl, &fmt))) goto done;
    int isfloat = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
        (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && ((WAVEFORMATEXTENSIBLE *)fmt)->SubFormat.Data1 == 3);
    if (fmt->nSamplesPerSec < 8000 || fmt->nSamplesPerSec > 192000 ||
        !(isfloat || fmt->wFormatTag == WAVE_FORMAT_PCM ||
          (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && ((WAVEFORMATEXTENSIBLE *)fmt)->SubFormat.Data1 == 1)) ||
        (fmt->wBitsPerSample != 16 && fmt->wBitsPerSample != 32) ||
        fmt->nChannels < 1 || fmt->nChannels > 8) {
        status("GamePad mic: unsupported VB-CABLE audio format"); goto done;
    }
    audio_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!audio_event) goto done;
    if (FAILED(IAudioClient_Initialize(cl, AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 1000000, 0, fmt, NULL))) goto done;
    if (FAILED(IAudioClient_SetEventHandle(cl, audio_event))) goto done;
    if (FAILED(IAudioClient_GetService(cl, &IID_IAudioRenderClient, (void **)&render))) goto done;

    /* wupstream_mic is a non-blocking dequeue, not a read that fills the requested
       amount. Poll it independently and buffer network jitter before rendering. */
    DWORD prime_start = GetTickCount();
    while (!*a->stop && fifo_count < 4800 && GetTickCount() - prime_start < 2500) {
        int got = wupstream_mic(a->pad, incoming, 480);
        if (got > 0) {
            for (int i = 0; i < got; i++) fifo[(fifo_read + fifo_count + i) % 48000] = incoming[i];
            fifo_count += (UINT)got;
        }
        if (got <= 0) Sleep(3);
    }
    if (*a->stop) goto done;
    if (fifo_count < 4800) {
        status("GamePad mic: no audio received from the GamePad; check its mic/network connection");
        goto done;
    }
    if (FAILED(IAudioClient_Start(cl))) goto done;
    ok = 1;
    {
        char msg[160];
        snprintf(msg, sizeof msg, "GamePad mic active - CABLE Input, %u Hz, %u-bit, %u ch",
                 (unsigned)fmt->nSamplesPerSec, (unsigned)fmt->wBitsPerSample, (unsigned)fmt->nChannels);
        status("%s", msg);
    }

    while (!*a->stop) {
        DWORD wait = WaitForSingleObject(audio_event, 4);
        /* Keep draining the network at a steady cadence even if the audio device
           has a different period or rate. The old implementation only read once
           per render block and replaced missing network samples with silence. */
        int got = wupstream_mic(a->pad, incoming, 480);
        if (got > 0) {
            if (fifo_count + (UINT)got > 24000) {
                UINT drop = fifo_count + (UINT)got - 24000;
                fifo_read = (fifo_read + drop) % 48000;
                fifo_count -= drop;
                if (src_phase >= drop) src_phase -= drop; else src_phase = 0.0;
            }
            for (int i = 0; i < got; i++) fifo[(fifo_read + fifo_count + i) % 48000] = incoming[i];
            fifo_count += (UINT)got;
        }
        if (wait == WAIT_FAILED) break;
        if (wait != WAIT_OBJECT_0) continue;

        UINT32 padding = 0;
        if (FAILED(IAudioClient_GetCurrentPadding(cl, &padding))) break;
        UINT32 bufferFrames = 0;
        if (FAILED(IAudioClient_GetBufferSize(cl, &bufferFrames))) break;
        UINT32 frames = bufferFrames > padding ? bufferFrames - padding : 0;
        if (!frames) continue;
        BYTE *dst = NULL;
        if (FAILED(IAudioRenderClient_GetBuffer(render, frames, &dst))) break;
        int ch = fmt->nChannels, bits = fmt->wBitsPerSample;
        /* The source is fixed at 48 kHz. Keep the exact rate ratio; do not
           modulate playback speed to chase FIFO fill, which adds audible
           pitch jitter. If the network briefly underruns, output silence
           without advancing past samples that have not arrived. */
        double step = 48000.0 / (double)fmt->nSamplesPerSec;
        for (UINT32 i = 0; i < frames; i++) {
            UINT sample_index = (UINT)src_phase;
            int16_t sample = 0;
            if (sample_index < fifo_count) {
                sample = fifo[(fifo_read + sample_index) % 48000];
                src_phase += step;
            } else {
                src_phase = 0.0;
            }
            for (int c = 0; c < ch; c++) {
                size_t at = ((size_t)i * ch + c) * (bits / 8);
                if (isfloat) { float v = sample / 32768.0f; memcpy(dst + at, &v, 4); }
                else if (bits == 16) memcpy(dst + at, &sample, 2);
                else { int32_t v = (int32_t)sample * 65536; memcpy(dst + at, &v, 4); }
            }
        }
        UINT consumed = (UINT)src_phase;
        src_phase -= consumed;
        if (consumed > fifo_count) { consumed = fifo_count; src_phase = 0.0; }
        fifo_read = (fifo_read + consumed) % 48000;
        fifo_count -= consumed;
        if (FAILED(IAudioRenderClient_ReleaseBuffer(render, frames, 0))) break;
    }
done:
    if (!ok && !*a->stop) status("GamePad mic could not start CABLE Input - check VB-CABLE installation and audio format");
    if (cl) { IAudioClient_Stop(cl); IAudioClient_Release(cl); }
    if (render) IAudioRenderClient_Release(render);
    if (audio_event) CloseHandle(audio_event);
    if (fmt) CoTaskMemFree(fmt);
    if (dev) IMMDevice_Release(dev);
    if (devices) IMMDeviceCollection_Release(devices);
    if (en) IMMDeviceEnumerator_Release(en);
    if (SUCCEEDED(co)) CoUninitialize();
    return 0;
}

/* ---------------- input: GamePad -> this PC ----------------
 * Four modes (pick one in the window):
 *  Keyboard + mouse (games):  left stick = W A S D, right stick = mouse look, ZR = fire (left click), ZL = aim (right click),
 *      A Space, B Ctrl (crouch), X E, Y R, L Q, R F, L3 Shift (sprint), R3 V (melee), D-pad = 1 2 3 4,
 *      + Esc, - Tab, HOME Enter. The touch screen is still a mouse (menus).
 *  Controller (Xbox 360): a virtual Xbox 360 pad (needs the ViGEmBus driver). Face buttons by position (the GamePad's bottom
 *      button is Xbox A), ZL/ZR = triggers, L/R = bumpers, sticks, D-pad, + Start, - Back, HOME = Esc key (pause in any game). Rumble comes back to the pad.
 *  Desktop: touch = mouse, left stick = mouse, right stick = scroll, ZR/ZL/L3 = clicks, D-pad = arrows, A Enter, B Esc, X Space,
 *      Y Tab, - Backspace, + Windows key, L/R browser back/forward, HOME play/pause, R3 mute.
 *  Off. */
enum { MODE_KM, MODE_PAD, MODE_DESK, MODE_OFF };
typedef struct { uint32_t mask; WORD vk; int scan; int ext; } keymap;   /* scan: send as a scancode (best for games) */
#define VW 0x100000u        /* virtual buttons made from the left stick */
#define VA 0x200000u
#define VS 0x400000u
#define VD 0x800000u
static const keymap DESK_KEYS[] = {
    { WUPSTREAM_UP, VK_UP, 1, 1 }, { WUPSTREAM_DOWN, VK_DOWN, 1, 1 }, { WUPSTREAM_LEFT, VK_LEFT, 1, 1 }, { WUPSTREAM_RIGHT, VK_RIGHT, 1, 1 },
    { WUPSTREAM_A, VK_RETURN, 1, 0 }, { WUPSTREAM_B, VK_ESCAPE, 1, 0 }, { WUPSTREAM_X, VK_SPACE, 1, 0 }, { WUPSTREAM_Y, VK_TAB, 1, 0 },
    { WUPSTREAM_MINUS, VK_BACK, 1, 0 }, { WUPSTREAM_PLUS, VK_LWIN, 1, 1 },
    { WUPSTREAM_L, VK_BROWSER_BACK, 0, 0 }, { WUPSTREAM_R, VK_BROWSER_FORWARD, 0, 0 },
    { WUPSTREAM_HOME, VK_MEDIA_PLAY_PAUSE, 0, 0 }, { WUPSTREAM_R3, VK_VOLUME_MUTE, 0, 0 },
};
static const keymap KM_KEYS[] = {
    { VW, 'W', 1, 0 }, { VA, 'A', 1, 0 }, { VS, 'S', 1, 0 }, { VD, 'D', 1, 0 },
    { WUPSTREAM_A, VK_SPACE, 1, 0 }, { WUPSTREAM_B, VK_LCONTROL, 1, 0 }, { WUPSTREAM_X, 'E', 1, 0 }, { WUPSTREAM_Y, 'R', 1, 0 },
    { WUPSTREAM_L, 'Q', 1, 0 }, { WUPSTREAM_R, 'F', 1, 0 }, { WUPSTREAM_L3, VK_LSHIFT, 1, 0 }, { WUPSTREAM_R3, 'V', 1, 0 },
    { WUPSTREAM_UP, '1', 1, 0 }, { WUPSTREAM_RIGHT, '2', 1, 0 }, { WUPSTREAM_DOWN, '3', 1, 0 }, { WUPSTREAM_LEFT, '4', 1, 0 },
    { WUPSTREAM_PLUS, VK_ESCAPE, 1, 0 }, { WUPSTREAM_MINUS, VK_TAB, 1, 0 }, { WUPSTREAM_HOME, VK_RETURN, 1, 0 },
};
static const keymap NO_KEYS[1] = { { 0, 0, 0, 0 } };
static const keymap PAD_KEYS[] = { { WUPSTREAM_HOME, VK_ESCAPE, 1, 0 } };   /* Controller mode: tap HOME = Esc (pause/back in any game) */
#define NDESK (sizeof DESK_KEYS / sizeof *DESK_KEYS)
#define NKM (sizeof KM_KEYS / sizeof *KM_KEYS)
typedef struct { uint32_t mask; DWORD down, up; } mousemap;
static const mousemap DESK_CLICKS[] = {
    { WUPSTREAM_ZR, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP },
    { WUPSTREAM_ZL, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP },
    { WUPSTREAM_L3, MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP },
};
static const mousemap KM_CLICKS[] = {
    { WUPSTREAM_ZR, MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP },
    { WUPSTREAM_ZL, MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP },
};
static const keymap *g_tab = NO_KEYS; static size_t g_ntab = 0;
static const mousemap *g_clk = DESK_CLICKS; static size_t g_nclk = 0;
static int g_sw, g_sh, g_dx, g_dy, g_dw, g_dh;      /* screen size and where it sits on the GamePad's 854x480 */
static uint32_t g_prev;                              /* (virtual) buttons held last time */
static int g_prevtouch;
static double g_scroll, g_lookx, g_looky;
static double g_looksens = 12.0, g_decksens = 9.0;   /* mouse speeds (wupclient.ini: look, cursor) */
static volatile LONG g_mode;                         /* MODE_* */

/* the virtual Xbox 360 controller (ViGEmBus) */
static PVIGEM_CLIENT g_vg; static PVIGEM_TARGET g_pad; static DWORD g_vgtry;
static volatile LONG g_vgasked, g_vginstalling;      /* offered the driver install already / installer running */
static wupstream *volatile g_curpad;                 /* the GamePad, for rumble */
static XUSB_REPORT g_lastrep; static int g_haverep;
static volatile LONG g_gamerumble;                   /* the game is asking for rumble */
static void CALLBACK pad_rumble(PVIGEM_CLIENT c, PVIGEM_TARGET t, UCHAR large, UCHAR small_, UCHAR led, LPVOID user) {
    (void)c; (void)t; (void)led; (void)user;
    g_gamerumble = (large || small_) ? 1 : 0;
    wupstream *p = g_curpad;
    if (p) wupstream_rumble(p, large || small_);
}
static void pad_open(void) {
    if (g_vgtry && GetTickCount() - g_vgtry < 3000) return;      /* (not every frame while the driver is missing) */
    g_vgtry = GetTickCount();
    if (!g_vg) {
        g_vg = vigem_alloc();
        if (!g_vg) return;
        VIGEM_ERROR e = vigem_connect(g_vg);
        if (!VIGEM_SUCCESS(e)) {
            vigem_free(g_vg); g_vg = NULL;
            if (e == VIGEM_ERROR_BUS_NOT_FOUND && !g_vginstalling && !g_vgasked) { g_vgasked = 1; PostMessage(g_win, WM_NEEDDRIVER, 0, 0); }
            return;
        }
    }
    PVIGEM_TARGET t = vigem_target_x360_alloc();
    if (!t) return;
    if (!VIGEM_SUCCESS(vigem_target_add(g_vg, t))) { vigem_target_free(t); return; }
    vigem_target_x360_register_notification(g_vg, t, pad_rumble, NULL);
    g_pad = t; g_haverep = 0;
}
static void pad_close(void) {
    if (!g_pad) return;
    vigem_target_x360_unregister_notification(g_pad);
    vigem_target_remove(g_vg, g_pad);
    vigem_target_free(g_pad);
    g_pad = NULL; g_gamerumble = 0;
    wupstream *p = g_curpad; if (p) wupstream_rumble(p, 0);
}
static void pad_shutdown(void) {
    pad_close();
    if (g_vg) { vigem_disconnect(g_vg); vigem_free(g_vg); g_vg = NULL; }
}
static SHORT axis(int v) { v = v * 32767 / 1000; return (SHORT)(v > 32767 ? 32767 : v < -32767 ? -32767 : v); }
static void pad_update(const wupstream_input_t *in) {
    if (!g_pad) return;
    uint32_t b = in->buttons; USHORT w = 0;
    if (b & WUPSTREAM_UP) w |= XUSB_GAMEPAD_DPAD_UP;
    if (b & WUPSTREAM_DOWN) w |= XUSB_GAMEPAD_DPAD_DOWN;
    if (b & WUPSTREAM_LEFT) w |= XUSB_GAMEPAD_DPAD_LEFT;
    if (b & WUPSTREAM_RIGHT) w |= XUSB_GAMEPAD_DPAD_RIGHT;
    if (b & WUPSTREAM_B) w |= XUSB_GAMEPAD_A;        /* by position: the bottom button is Xbox A, the right one B, ... */
    if (b & WUPSTREAM_A) w |= XUSB_GAMEPAD_B;
    if (b & WUPSTREAM_Y) w |= XUSB_GAMEPAD_X;
    if (b & WUPSTREAM_X) w |= XUSB_GAMEPAD_Y;
    if (b & WUPSTREAM_L) w |= XUSB_GAMEPAD_LEFT_SHOULDER;
    if (b & WUPSTREAM_R) w |= XUSB_GAMEPAD_RIGHT_SHOULDER;
    if (b & WUPSTREAM_PLUS) w |= XUSB_GAMEPAD_START;
    if (b & WUPSTREAM_MINUS) w |= XUSB_GAMEPAD_BACK;
    if (b & WUPSTREAM_L3) w |= XUSB_GAMEPAD_LEFT_THUMB;
    if (b & WUPSTREAM_R3) w |= XUSB_GAMEPAD_RIGHT_THUMB;
    XUSB_REPORT r; memset(&r, 0, sizeof r);
    r.wButtons = w;
    r.bLeftTrigger = (b & WUPSTREAM_ZL) ? 255 : 0;
    r.bRightTrigger = (b & WUPSTREAM_ZR) ? 255 : 0;
    r.sThumbLX = axis(in->lx); r.sThumbLY = axis(in->ly); r.sThumbRX = axis(in->rx); r.sThumbRY = axis(in->ry);
    if (!g_haverep || memcmp(&r, &g_lastrep, sizeof r)) { vigem_target_x360_update(g_vg, g_pad, r); g_lastrep = r; g_haverep = 1; }
}

static void send_key(const keymap *k, int down) {
    INPUT i; memset(&i, 0, sizeof i); i.type = INPUT_KEYBOARD;
    i.ki.wVk = k->vk;
    if (k->scan) { i.ki.wScan = (WORD)MapVirtualKey(k->vk, MAPVK_VK_TO_VSC); i.ki.dwFlags |= KEYEVENTF_SCANCODE; i.ki.wVk = 0; }
    if (k->ext) i.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    if (!down) i.ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(1, &i, sizeof i);
}
static void send_mouse(DWORD flags, int dx, int dy, DWORD data) {
    INPUT i; memset(&i, 0, sizeof i); i.type = INPUT_MOUSE;
    i.mi.dwFlags = flags; i.mi.dx = dx; i.mi.dy = dy; i.mi.mouseData = data;
    SendInput(1, &i, sizeof i);
}
static int dead(int v) { return v > 150 ? v - 150 : v < -150 ? v + 150 : 0; }

/* ---------------- on-screen keyboard ----------------
 * HOME + X shows a keyboard on the bottom of the GamePad screen; tap keys on the touch screen. Shift / Ctrl / Alt / Win
 * latch (tap once, they stay down for the next key), everything else is pressed while you touch it. */
typedef struct { const char *lab; WORD vk; double w; int ext; } oskdef;
#define OK(l, v) { l, v, 1.0, 0 }
#define OKW(l, v, w) { l, v, w, 0 }
#define OKE(l, v) { l, v, 1.0, 1 }
static const oskdef OSK_R0[] = { OK("`", VK_OEM_3), OK("1", '1'), OK("2", '2'), OK("3", '3'), OK("4", '4'), OK("5", '5'), OK("6", '6'), OK("7", '7'),
    OK("8", '8'), OK("9", '9'), OK("0", '0'), OK("-", VK_OEM_MINUS), OK("=", VK_OEM_PLUS), OKW("Bksp", VK_BACK, 2), { NULL, 0, 0, 0 } };
static const oskdef OSK_R1[] = { OKW("Tab", VK_TAB, 1.5), OK("q", 'Q'), OK("w", 'W'), OK("e", 'E'), OK("r", 'R'), OK("t", 'T'), OK("y", 'Y'), OK("u", 'U'),
    OK("i", 'I'), OK("o", 'O'), OK("p", 'P'), OK("[", VK_OEM_4), OK("]", VK_OEM_6), OKW("\\", VK_OEM_5, 1.5), { NULL, 0, 0, 0 } };
static const oskdef OSK_R2[] = { OKW("Caps", VK_CAPITAL, 1.75), OK("a", 'A'), OK("s", 'S'), OK("d", 'D'), OK("f", 'F'), OK("g", 'G'), OK("h", 'H'),
    OK("j", 'J'), OK("k", 'K'), OK("l", 'L'), OK(";", VK_OEM_1), OK("'", VK_OEM_7), OKW("Enter", VK_RETURN, 2.25), { NULL, 0, 0, 0 } };
static const oskdef OSK_R3[] = { OKW("Shift", VK_LSHIFT, 2.25), OK("z", 'Z'), OK("x", 'X'), OK("c", 'C'), OK("v", 'V'), OK("b", 'B'), OK("n", 'N'),
    OK("m", 'M'), OK(",", VK_OEM_COMMA), OK(".", VK_OEM_PERIOD), OK("/", VK_OEM_2), OKW("Shift", VK_LSHIFT, 2.75), { NULL, 0, 0, 0 } };
static const oskdef OSK_R4[] = { OKW("Esc", VK_ESCAPE, 1.5), OKW("Ctrl", VK_LCONTROL, 1.5), OKE("Win", VK_LWIN), OK("Alt", VK_LMENU),
    OKW("Space", VK_SPACE, 5.5), OKE("<", VK_LEFT), OKE("v", VK_DOWN), OKE("^", VK_UP), OKE(">", VK_RIGHT), { NULL, 0, 0, 0 } };
#define OSK_RH 40
#define OSK_Y (WUPSTREAM_H - 5 * OSK_RH - 6)
typedef struct { RECT r; const char *lab; keymap k; int mod; } oskkey;
static oskkey g_osk_keys[80]; static int g_osk_n;
static unsigned char g_osk_latched[80];
static int g_osk_held = -1;       /* key held by the finger: -1 none, -2 a latch key or a miss, else an index */
static int g_prevraw;             /* raw touch state last time */
static volatile LONG g_osk;
static void osk_build(void) {
    const oskdef *rows[5] = { OSK_R0, OSK_R1, OSK_R2, OSK_R3, OSK_R4 };
    double u = (double)WUPSTREAM_W / 15.0; g_osk_n = 0;
    for (int r = 0; r < 5; r++) {
        double x = 0;
        for (const oskdef *d = rows[r]; d->lab; d++) {
            oskkey *k = &g_osk_keys[g_osk_n++];
            k->r.left = (LONG)(x * u) + 1; k->r.right = (LONG)((x + d->w) * u) - 1;
            k->r.top = OSK_Y + r * OSK_RH + 1; k->r.bottom = k->r.top + OSK_RH - 2;
            k->lab = d->lab; k->k.mask = 0; k->k.vk = d->vk; k->k.scan = 1; k->k.ext = d->ext;
            k->mod = d->vk == VK_LSHIFT || d->vk == VK_LCONTROL || d->vk == VK_LMENU || d->vk == VK_LWIN;
            x += d->w;
        }
    }
}
static int osk_busy(void) { if (g_osk_held != -1) return 1; for (int i = 0; i < g_osk_n; i++) if (g_osk_latched[i]) return 1; return 0; }
static void osk_release_latched(void) {
    for (int i = 0; i < g_osk_n; i++) if (g_osk_latched[i]) { send_key(&g_osk_keys[i].k, 0); g_osk_latched[i] = 0; }
}
static void osk_release(void) {
    if (g_osk_held >= 0) send_key(&g_osk_keys[g_osk_held].k, 0);
    g_osk_held = -1; osk_release_latched();
}
/* returns 1 if this touch belongs to the keyboard (so it must not also move/click the mouse) */
static int osk_touch(const wupstream_input_t *in) {
    if (!g_osk_n) osk_build();
    int consumed = 0;
    if (in->touched) {
        if (g_osk_held != -1) consumed = 1;
        else if (!g_prevraw && in->ty >= OSK_Y - 4) {
            consumed = 1; g_osk_held = -2;
            for (int i = 0; i < g_osk_n; i++) {
                oskkey *k = &g_osk_keys[i];
                if (in->tx >= k->r.left && in->tx < k->r.right && in->ty >= k->r.top && in->ty < k->r.bottom) {
                    if (k->mod) { g_osk_latched[i] ^= 1; send_key(&k->k, g_osk_latched[i]); }
                    else { send_key(&k->k, 1); g_osk_held = i; }
                    break;
                }
            }
        }
    } else if (g_osk_held != -1) {
        if (g_osk_held >= 0) { send_key(&g_osk_keys[g_osk_held].k, 0); osk_release_latched(); }
        g_osk_held = -1; consumed = 1;
    }
    return consumed;
}
static void draw_osk(HDC dc, HFONT f) {
    if (!g_osk) return;
    if (!g_osk_n) osk_build();
    RECT bg = { 0, OSK_Y - 4, WUPSTREAM_W, WUPSTREAM_H };
    HBRUSH b = CreateSolidBrush(RGB(20, 20, 24)); FillRect(dc, &bg, b); DeleteObject(b);
    HGDIOBJ old = SelectObject(dc, f); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
    for (int i = 0; i < g_osk_n; i++) {
        oskkey *k = &g_osk_keys[i];
        b = CreateSolidBrush(i == g_osk_held ? RGB(30, 110, 210) : g_osk_latched[i] ? RGB(200, 120, 10) : RGB(62, 62, 70));
        FillRect(dc, &k->r, b); DeleteObject(b);
        RECT t = k->r; DrawTextA(dc, k->lab, -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, old);
}

static void release_all(void) {
    osk_release();
    for (size_t n = 0; n < g_ntab; n++) if (g_prev & g_tab[n].mask) send_key(&g_tab[n], 0);
    for (size_t n = 0; n < g_nclk; n++) if (g_prev & g_clk[n].mask) send_mouse(g_clk[n].up, 0, 0, 0);
    if (g_prevtouch) send_mouse(MOUSEEVENTF_LEFTUP, 0, 0, 0);
    g_prev = 0; g_prevtouch = 0; g_scroll = g_lookx = g_looky = 0;
}

static void apply_input(const wupstream_input_t *in, int mode) {
    const keymap *tab = mode == MODE_KM ? KM_KEYS : mode == MODE_DESK ? DESK_KEYS : mode == MODE_PAD ? PAD_KEYS : NO_KEYS;
    if (tab != g_tab) {                              /* the mode changed: let go of everything the old one held */
        release_all();
        g_tab = tab; g_ntab = mode == MODE_KM ? NKM : mode == MODE_DESK ? NDESK : mode == MODE_PAD ? 1 : 0;
        g_clk = mode == MODE_KM ? KM_CLICKS : DESK_CLICKS; g_nclk = mode == MODE_KM ? 2 : mode == MODE_DESK ? 3 : 0;
    }
    uint32_t b = in->buttons;
    if (mode == MODE_PAD) { pad_update(in); b &= WUPSTREAM_HOME; }
    else if (mode == MODE_KM) {                      /* the left stick is W A S D */
        if (in->ly > 450) b |= VW;
        if (in->ly < -450) b |= VS;
        if (in->lx < -450) b |= VA;
        if (in->lx > 450) b |= VD;
    }
    uint32_t ch = b ^ g_prev;
    for (size_t n = 0; n < g_ntab; n++) if (ch & g_tab[n].mask) send_key(&g_tab[n], (b & g_tab[n].mask) != 0);
    for (size_t n = 0; n < g_nclk; n++)
        if (ch & g_clk[n].mask) send_mouse((b & g_clk[n].mask) ? g_clk[n].down : g_clk[n].up, 0, 0, 0);
    g_prev = b;

    if (!g_osk && osk_busy()) osk_release();
    int consumed = g_osk ? osk_touch(in) : 0;        /* a touch on the on-screen keyboard is not a mouse touch */
    g_prevraw = in->touched;
    int mt = in->touched && !consumed;
    if (mt) {                                        /* the picture sits in a box on the pad's screen: map back to the PC screen */
        int x = (in->tx - g_dx) * g_sw / g_dw, y = (in->ty - g_dy) * g_sh / g_dh;
        x = x < 0 ? 0 : x >= g_sw ? g_sw - 1 : x; y = y < 0 ? 0 : y >= g_sh ? g_sh - 1 : y;
        send_mouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, x * 65535 / (g_sw - 1 ? g_sw - 1 : 1), y * 65535 / (g_sh - 1 ? g_sh - 1 : 1), 0);
        if (!g_prevtouch) send_mouse(MOUSEEVENTF_LEFTDOWN, 0, 0, 0);
    } else if (g_prevtouch) send_mouse(MOUSEEVENTF_LEFTUP, 0, 0, 0);
    g_prevtouch = mt;

    if (mode == MODE_KM) {                           /* right stick: mouse look (a curve: small pushes are precise, fractions are kept) */
        int rx = dead(in->rx), ry = dead(in->ry);
        double fx = rx / 950.0, fy = ry / 950.0;
        g_lookx += fx * (fx < 0 ? -fx : fx) * g_looksens; g_looky -= fy * (fy < 0 ? -fy : fy) * g_looksens;
        int mx = (int)g_lookx, my = (int)g_looky;
        g_lookx -= mx; g_looky -= my;
        if (mx || my) send_mouse(MOUSEEVENTF_MOVE, mx, my, 0);
    } else if (mode == MODE_DESK) {
        int lx = dead(in->lx), ly = dead(in->ly);    /* left stick: mouse */
        if (lx || ly) {
            double fx = lx / 950.0, fy = ly / 950.0;
            int mx = (int)(fx * (fx < 0 ? -fx : fx) * g_decksens + (fx > 0 ? 1 : fx < 0 ? -1 : 0));
            int my = (int)(-fy * (fy < 0 ? -fy : fy) * g_decksens - (fy > 0 ? 1 : fy < 0 ? -1 : 0));
            send_mouse(MOUSEEVENTF_MOVE, mx, my, 0);
        }
        int ry = dead(in->ry);                       /* right stick: scroll (push up = scroll up) */
        if (ry) {
            g_scroll += ry / 950.0 * 30.0;
            if (g_scroll >= 120 || g_scroll <= -120) { int notches = (int)(g_scroll / 120); send_mouse(MOUSEEVENTF_WHEEL, 0, 0, (DWORD)(notches * 120)); g_scroll -= notches * 120; }
        } else g_scroll = 0;
    }
}

/* ---------------- screen capture ----------------
 * DXGI Desktop Duplication reads what the display really shows, so full-screen games (which bypass the desktop
 * compositor and left a GDI grab frozen) come through. GDI stays as the fallback (old Windows, HDR, remote desktop). */
typedef struct { ID3D11Device *dev; ID3D11DeviceContext *ctx; IDXGIOutputDuplication *dup; ID3D11Texture2D *stage; int w, h; } dda;

static int xs[WUPSTREAM_W * 3], ys[WUPSTREAM_H * 3];   /* 3x3 sample positions for every output pixel */
static void geometry(int sw, int sh) {
    int dw = WUPSTREAM_W, dh = sh * WUPSTREAM_W / sw;  /* fit, keep the aspect ratio */
    if (dh > WUPSTREAM_H) { dh = WUPSTREAM_H; dw = sw * WUPSTREAM_H / sh; }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    g_sw = sw; g_sh = sh; g_dw = dw; g_dh = dh; g_dx = (WUPSTREAM_W - dw) / 2; g_dy = (WUPSTREAM_H - dh) / 2;
    for (int x = 0; x < dw; x++) for (int k = 0; k < 3; k++) { int v = (int)((x + (k + 0.5) / 3.0) * sw / dw); xs[x * 3 + k] = v >= sw ? sw - 1 : v; }
    for (int y = 0; y < dh; y++) for (int k = 0; k < 3; k++) { int v = (int)((y + (k + 0.5) / 3.0) * sh / dh); ys[y * 3 + k] = v >= sh ? sh - 1 : v; }
}
static void scale_into(uint32_t *dst, const uint8_t *src, int pitch) {
    for (int y = 0; y < g_dh; y++) {
        uint32_t *o = dst + (size_t)(g_dy + y) * WUPSTREAM_W + g_dx;
        const uint32_t *rows[3];
        for (int j = 0; j < 3; j++) rows[j] = (const uint32_t *)(src + (size_t)ys[y * 3 + j] * pitch);
        for (int x = 0; x < g_dw; x++) {
            const int *xx = xs + x * 3; unsigned b = 0, g = 0, r = 0;
            for (int j = 0; j < 3; j++) for (int k = 0; k < 3; k++) { uint32_t p = rows[j][xx[k]]; b += p & 255; g += (p >> 8) & 255; r += (p >> 16) & 255; }
            o[x] = ((b * 57) >> 9) | (((g * 57) >> 9) << 8) | (((r * 57) >> 9) << 16);
        }
    }
}

static void dda_close(dda *d) {
    if (d->dup) IDXGIOutputDuplication_Release(d->dup);
    if (d->stage) ID3D11Texture2D_Release(d->stage);
    if (d->ctx) ID3D11DeviceContext_Release(d->ctx);
    if (d->dev) ID3D11Device_Release(d->dev);
    memset(d, 0, sizeof *d);
}
static int dda_open(dda *d) {
    memset(d, 0, sizeof *d);
    IDXGIFactory1 *fac = NULL; IDXGIAdapter1 *ad = NULL; IDXGIOutput *out = NULL; IDXGIOutput1 *out1 = NULL; int ok = 0;
    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&fac))) return 0;
    for (UINT a = 0; !out && IDXGIFactory1_EnumAdapters1(fac, a, &ad) == S_OK; a++) {      /* the primary monitor sits at 0,0 */
        for (UINT o = 0; IDXGIAdapter1_EnumOutputs(ad, o, &out) == S_OK; o++) {
            DXGI_OUTPUT_DESC od; IDXGIOutput_GetDesc(out, &od);
            if (od.DesktopCoordinates.left == 0 && od.DesktopCoordinates.top == 0) break;
            IDXGIOutput_Release(out); out = NULL;
        }
        if (!out) { IDXGIAdapter1_Release(ad); ad = NULL; }
    }
    if (!out) goto end;
    if (FAILED(D3D11CreateDevice((IDXGIAdapter *)ad, D3D_DRIVER_TYPE_UNKNOWN, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &d->dev, NULL, &d->ctx))) goto end;
    if (FAILED(IDXGIOutput_QueryInterface(out, &IID_IDXGIOutput1, (void **)&out1))) goto end;
    if (FAILED(IDXGIOutput1_DuplicateOutput(out1, (IUnknown *)d->dev, &d->dup))) goto end;
    DXGI_OUTDUPL_DESC dd; IDXGIOutputDuplication_GetDesc(d->dup, &dd);
    d->w = (int)dd.ModeDesc.Width; d->h = (int)dd.ModeDesc.Height;
    D3D11_TEXTURE2D_DESC td; memset(&td, 0, sizeof td);
    td.Width = d->w; td.Height = d->h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(d->dev, &td, NULL, &d->stage))) goto end;
    ok = 1;
end:
    if (out1) IDXGIOutput1_Release(out1);
    if (out) IDXGIOutput_Release(out);
    if (ad) IDXGIAdapter1_Release(ad);
    IDXGIFactory1_Release(fac);
    if (!ok) dda_close(d);
    return ok;
}
/* 1: a new picture is in scene; 0: nothing new; -1: lost (display mode changed, ...): reopen; -2: unusable (HDR, rotated) */
static int dda_grab(dda *d, uint32_t *scene) {
    DXGI_OUTDUPL_FRAME_INFO fi; IDXGIResource *res = NULL;
    HRESULT hr = IDXGIOutputDuplication_AcquireNextFrame(d->dup, 4, &fi, &res);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return 0;
    if (FAILED(hr)) return -1;
    int r = 0;
    if (fi.LastPresentTime.QuadPart != 0) {            /* (0: only the mouse pointer moved) */
        ID3D11Texture2D *tex = NULL;
        if (SUCCEEDED(IDXGIResource_QueryInterface(res, &IID_ID3D11Texture2D, (void **)&tex))) {
            D3D11_TEXTURE2D_DESC td; ID3D11Texture2D_GetDesc(tex, &td);
            if (td.Format != DXGI_FORMAT_B8G8R8A8_UNORM || (int)td.Width != d->w || (int)td.Height != d->h) r = -2;
            else {
                ID3D11DeviceContext_CopyResource(d->ctx, (ID3D11Resource *)d->stage, (ID3D11Resource *)tex);
                D3D11_MAPPED_SUBRESOURCE m;
                if (SUCCEEDED(ID3D11DeviceContext_Map(d->ctx, (ID3D11Resource *)d->stage, 0, D3D11_MAP_READ, 0, &m))) {
                    scale_into(scene, (const uint8_t *)m.pData, (int)m.RowPitch);
                    ID3D11DeviceContext_Unmap(d->ctx, (ID3D11Resource *)d->stage, 0);
                    r = 1;
                } else r = -1;
            }
            ID3D11Texture2D_Release(tex);
        }
    }
    IDXGIResource_Release(res);
    IDXGIOutputDuplication_ReleaseFrame(d->dup);
    return r;
}

/* the mouse pointer is not part of a duplicated picture: draw it on the small frame */
static void draw_cursor_small(HDC dc) {
    CURSORINFO ci = { sizeof ci };
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING)) return;
    ICONINFO ii;
    if (!GetIconInfo(ci.hCursor, &ii)) return;
    int cs = GetSystemMetrics(SM_CXCURSOR) * g_dw / g_sw; if (cs < 12) cs = 12;
    int x = g_dx + ci.ptScreenPos.x * g_dw / g_sw - (int)ii.xHotspot * cs / 32;
    int y = g_dy + ci.ptScreenPos.y * g_dh / g_sh - (int)ii.yHotspot * cs / 32;
    DrawIconEx(dc, x, y, ci.hCursor, cs, cs, 0, NULL, DI_NORMAL);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
}

/* ---------------- on-screen mode indicator ----------------
 * A small badge in the corner of the GamePad always names the active control mode; a big banner appears for a couple
 * of seconds whenever the mode changes (from the GamePad combo or the PC window). */
static const char *SHORT_NAMES[4] = { "KEYBOARD + MOUSE", "CONTROLLER", "DESKTOP", "INPUT OFF" };
static const COLORREF MODE_COL[4] = { RGB(30, 110, 210), RGB(30, 150, 60), RGB(200, 120, 10), RGB(150, 40, 40) };
static void text_box(HDC dc, HFONT f, const char *t, int x, int y, COLORREF bg, int center) {
    HGDIOBJ old = SelectObject(dc, f);
    SIZE sz; GetTextExtentPoint32A(dc, t, (int)strlen(t), &sz);
    int pad = 6;
    if (center) x -= (sz.cx + 2 * pad) / 2;
    RECT r = { x, y, x + sz.cx + 2 * pad, y + sz.cy + pad };
    HBRUSH b = CreateSolidBrush(bg); FillRect(dc, &r, b); DeleteObject(b);
    SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
    TextOutA(dc, x + pad, y + pad / 2, t, (int)strlen(t));
    SelectObject(dc, old);
}
static void draw_overlay(HDC dc, HFONT fs, HFONT fl, int mode, int show) {      /* only while HOME is held / right after a switch */
    if (!show || mode < 0 || mode > 3) return;
    text_box(dc, fl, SHORT_NAMES[mode], WUPSTREAM_W / 2, WUPSTREAM_H - 172, MODE_COL[mode], 1);
    text_box(dc, fs, "HOME + PLUS / MINUS = next / previous mode", WUPSTREAM_W / 2, WUPSTREAM_H - 122, RGB(40, 40, 40), 1);
    text_box(dc, fs, "HOME + D-PAD: UP keyboard, RIGHT controller, DOWN desktop, LEFT off", WUPSTREAM_W / 2, WUPSTREAM_H - 94, RGB(40, 40, 40), 1);
    text_box(dc, fs, "HOME + X = keyboard | HOME + Y = settings", WUPSTREAM_W / 2, WUPSTREAM_H - 66, RGB(40, 40, 40), 1);
}
static void draw_settings_menu(HDC dc, HFONT f) {
    static const char *names[7] = { "GamePad microphone", "Stream quality", "Draw mouse pointer", "Control mode", "Disconnect GamePad", "Set up VB-CABLE", "Close settings" };
    char line[96];
    RECT bg = { 70, 70, WUPSTREAM_W - 70, WUPSTREAM_H - 70 };
    HBRUSH b = CreateSolidBrush(RGB(18, 22, 30)); FillRect(dc, &bg, b); DeleteObject(b);
    text_box(dc, f, "WUP STREAM SETTINGS", WUPSTREAM_W / 2, 88, RGB(34, 78, 130), 1);
    HGDIOBJ old = SelectObject(dc, f); SetBkMode(dc, TRANSPARENT);
    for (int i = 0; i < 7; i++) {
        RECT r = { 112, 112 + i * 35, WUPSTREAM_W - 112, 143 + i * 35 };
        b = CreateSolidBrush(i == g_settings_sel ? RGB(38, 105, 180) : RGB(48, 54, 64));
        FillRect(dc, &r, b); DeleteObject(b);
        SetTextColor(dc, RGB(255,255,255));
        const char *value = i == 0 ? (g_mic_enabled ? "ON" : "OFF") :
            i == 1 ? (g_stream_kbps <= 3500 ? "LOW (3.5 Mbps)" : "BALANCED (4.5 Mbps)") :
            i == 2 ? (g_drawptr ? "ON" : "OFF") :
            i == 3 ? SHORT_NAMES[(int)g_mode] :
            i == 5 ? (g_cable_setup_running ? "WORKING..." : "DOWNLOAD + INSTALL") : "";
        snprintf(line, sizeof line, "%s%s%s", names[i], value[0] ? "  -  " : "", value);
        RECT t = r; DrawTextA(dc, line, -1, &t, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, old);
    text_box(dc, f, "UP/DOWN select   A choose   B close", WUPSTREAM_W / 2, WUPSTREAM_H - 80, RGB(34, 78, 130), 1);
}

/* ---------------- the streaming thread ---------------- */
static uint32_t g_scene[WUPSTREAM_W * WUPSTREAM_H];
static DWORD WINAPI stream_thread(LPVOID unused) {
    (void)unused;

    /* Keep each encoded frame on one side of the GamePad refresh boundary.
       The libwupstream default phase is 3 ms, while it starts sending up to
       5 ms before that point; on a busy Wi-Fi link this can straddle refresh
       and produce a horizontal tear / wrapped-looking lower half. */
    _putenv_s("WUPSTREAM_PHASE_MS", "8");
    /* Leave headroom for retransmits and other traffic on shared Wi-Fi. */
    _putenv_s("WUPSTREAM_KBPS", g_stream_kbps <= 3500 ? "3500" : "4500");
    HDC scr = GetDC(NULL);
    int gsw = GetSystemMetrics(SM_CXSCREEN), gsh = GetSystemMetrics(SM_CYSCREEN);

    HDC full = CreateCompatibleDC(scr);                /* GDI fallback */
    HBITMAP fullbmp = CreateCompatibleBitmap(scr, gsw, gsh);
    SelectObject(full, fullbmp);

    BITMAPINFO bi; memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof bi.bmiHeader; bi.bmiHeader.biWidth = WUPSTREAM_W; bi.bmiHeader.biHeight = -WUPSTREAM_H;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *px = NULL;
    HDC small = CreateCompatibleDC(scr);
    HBITMAP smallbmp = CreateDIBSection(scr, &bi, DIB_RGB_COLORS, &px, NULL, 0);
    SelectObject(small, smallbmp);
    SetStretchBltMode(small, HALFTONE); SetBrushOrgEx(small, 0, 0, NULL);
    RECT all = {0, 0, WUPSTREAM_W, WUPSTREAM_H};

    HFONT fs = CreateFontA(-16, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    HFONT fl = CreateFontA(-38, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, "Segoe UI");
    dda d; int usegdi = !dda_open(&d); DWORD lastretry = 0;
    geometry(usegdi ? gsw : d.w, usegdi ? gsh : d.h);
    memset(g_scene, 0, sizeof g_scene);

    while (g_run) {
        status("Looking for a GamePad (open WUP Stream from the Homebrew Menu)...");
        wupstream *pad = NULL;
        while (g_run && !pad) { _putenv_s("WUPSTREAM_KBPS", g_stream_kbps <= 3500 ? "3500" : "4500"); pad = wupstream_open(g_ipstr[0] ? g_ipstr : NULL, 2000); }
        if (!pad) break;
        status("Streaming to the GamePad");

        volatile LONG astop = 0, mstop = 0; HANDLE ath = NULL, mth = NULL; DWORD micretry = 0;
        audio_arg aa = { pad, &astop }; mic_arg ma = { pad, &mstop };
        if (g_useaudio) ath = CreateThread(NULL, 0, audio_thread, &aa, 0, NULL);

        g_curpad = pad;
        wupstream_input_t in; int tick = 0; DWORD t0 = GetTickCount(); int frames = 0;
        uint32_t cyclemod_prev = 0;      /* +/- state last tick, for edge-detecting the profile-cycle combo */
        uint32_t chordheld = 0;          /* +/- buttons used in a combo: swallowed until released */
        int homeheld = 0, chord = 0, shown = -1, disconnect_requested = 0; uint32_t dpad_prev = 0, xprev = 0, yprev = 0, settings_prev = 0;
        DWORD hometap_until = 0, banner_until = 0, rumble_off = 0;
        while (g_run && wupstream_input(pad, &in) == 0) {
            DWORD now = GetTickCount();
            if (g_mic_enabled && !mth && (DWORD)(now - micretry) >= 2000) { micretry = now; mstop = 0; mth = CreateThread(NULL, 0, mic_thread, &ma, 0, NULL); }
            if (!g_mic_enabled && mth) { mstop = 1; WaitForSingleObject(mth, INFINITE); CloseHandle(mth); mth = NULL; mstop = 0; }
            /* HOME + PLUS/MINUS cycles the control profile: hold HOME and tap PLUS (next) or MINUS (previous).
             * HOME itself is only sent on a clean tap (so the combo never fires Enter/Guide), and the +/- used in a
             * combo are swallowed, so switching never presses anything in the game. */
            uint32_t raw = in.buttons, cyclemod = raw & (WUPSTREAM_PLUS | WUPSTREAM_MINUS);
            int home = (raw & WUPSTREAM_HOME) != 0;
            if (home && cyclemod && !(cyclemod_prev & cyclemod)) {
                int dir = (cyclemod & WUPSTREAM_PLUS) ? 1 : -1;
                int next = ((int)g_mode + dir + 4) % 4;
                g_mode = next;
                PostMessage(g_win, WM_SETMODE, (WPARAM)next, 0);
                chord = 1; chordheld |= cyclemod;
            }
            uint32_t dmod = raw & (WUPSTREAM_UP | WUPSTREAM_DOWN | WUPSTREAM_LEFT | WUPSTREAM_RIGHT);
            if (home && dmod && !(dpad_prev & dmod)) {       /* HOME + D-pad picks a mode directly */
                int next = (dmod & WUPSTREAM_UP) ? MODE_KM : (dmod & WUPSTREAM_RIGHT) ? MODE_PAD : (dmod & WUPSTREAM_DOWN) ? MODE_DESK : MODE_OFF;
                g_mode = next;
                PostMessage(g_win, WM_SETMODE, (WPARAM)next, 0);
                chord = 1; chordheld |= dmod;
            }
            dpad_prev = home ? dmod : 0;
            uint32_t xmod = raw & WUPSTREAM_X;
            if (home && xmod && !xprev) { g_osk = !g_osk; chord = 1; chordheld |= xmod; }     /* HOME + X: on-screen keyboard */
            xprev = home ? xmod : 0;
            uint32_t ymod = raw & WUPSTREAM_Y;
            if (home && ymod && !yprev) { g_settings_menu = !g_settings_menu; g_settings_sel = 0; chord = 1; chordheld |= ymod; }
            yprev = home ? ymod : 0;
            if (g_settings_menu) {
                uint32_t nav = raw & (WUPSTREAM_UP | WUPSTREAM_DOWN | WUPSTREAM_A | WUPSTREAM_B);
                if ((nav & WUPSTREAM_UP) && !(settings_prev & WUPSTREAM_UP)) g_settings_sel = (g_settings_sel + 6) % 7;
                if ((nav & WUPSTREAM_DOWN) && !(settings_prev & WUPSTREAM_DOWN)) g_settings_sel = (g_settings_sel + 1) % 7;
                if ((nav & WUPSTREAM_A) && !(settings_prev & WUPSTREAM_A)) {
                    if (g_settings_sel == 0) g_mic_enabled = !g_mic_enabled;
                    else if (g_settings_sel == 1) g_stream_kbps = g_stream_kbps <= 3500 ? 4500 : 3500;
                    else if (g_settings_sel == 2) g_drawptr = !g_drawptr;
                    else if (g_settings_sel == 3) { int next = ((int)g_mode + 1) % 4; g_mode = next; PostMessage(g_win, WM_SETMODE, (WPARAM)next, 0); }
                    else if (g_settings_sel == 4) { disconnect_requested = 1; g_settings_menu = 0; }
                    else if (g_settings_sel == 5) {
                        if (InterlockedCompareExchange(&g_cable_setup_running, 1, 0) == 0) {
                            HANDLE setup = CreateThread(NULL, 0, install_vbcable_thread, NULL, 0, NULL);
                            if (setup) CloseHandle(setup); else { InterlockedExchange(&g_cable_setup_running, 0); status("Could not start VB-CABLE setup."); }
                        }
                    } else g_settings_menu = 0;
                    if (g_settings_sel <= 3) PostMessage(g_win, WM_SETTINGS_CHANGED, 0, 0);
                }
                if ((nav & WUPSTREAM_B) && !(settings_prev & WUPSTREAM_B)) g_settings_menu = 0;
                settings_prev = nav;
                in.buttons = 0; in.lx = in.ly = in.rx = in.ry = 0; in.touched = 0;
            } else settings_prev = 0;
            if (disconnect_requested) break;
            cyclemod_prev = home ? cyclemod : 0;
            if (home) homeheld = 1;
            else { if (homeheld && !chord) hometap_until = now + 80; homeheld = 0; chord = 0; }
            chordheld &= raw;
            in.buttons &= ~chordheld;
            if (home) { in.buttons = 0; in.lx = in.ly = in.rx = in.ry = 0; }      /* HOME is an fn key: nothing else reaches the game while it is held */
            else if ((int)(hometap_until - now) > 0) in.buttons |= WUPSTREAM_HOME;

            int mode = (int)g_mode;
            if (mode != shown) {         /* mode changed (or first frame): banner, and a short buzz as confirmation */
                banner_until = now + 1500;
                if (shown != -1) { wupstream_rumble(pad, 1); rumble_off = now + 200; }
                shown = mode;
            }
            if (rumble_off && (int)(now - rumble_off) >= 0) { wupstream_rumble(pad, mode == MODE_PAD ? (int)g_gamerumble : 0); rumble_off = 0; }
            if (mode == MODE_PAD) { if (!g_pad) pad_open(); } else if (g_pad) pad_close();
            if (mode != MODE_OFF) apply_input(&in, mode); else if (g_prev || g_prevtouch || osk_busy()) release_all();

            if (!usegdi) {
                if (d.dup) {
                    int r = dda_grab(&d, g_scene);
                    if (r == -1) dda_close(&d);                                   /* lost: reopen below */
                    else if (r == -2) { dda_close(&d); usegdi = 1; geometry(gsw, gsh); }
                } else if (GetTickCount() - lastretry > 250) {                   /* e.g. a game changed the resolution */
                    lastretry = GetTickCount();
                    if (dda_open(&d)) { geometry(d.w, d.h); memset(g_scene, 0, sizeof g_scene); }
                }
            }
            if (usegdi) {
                BitBlt(full, 0, 0, gsw, gsh, scr, 0, 0, SRCCOPY | CAPTUREBLT);
                CURSORINFO ci = { sizeof ci };
                if (g_drawptr && GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
                    ICONINFO ii;
                    if (GetIconInfo(ci.hCursor, &ii)) {
                        DrawIconEx(full, ci.ptScreenPos.x - ii.xHotspot, ci.ptScreenPos.y - ii.yHotspot, ci.hCursor, 0, 0, 0, NULL, DI_NORMAL);
                        if (ii.hbmMask) DeleteObject(ii.hbmMask);
                        if (ii.hbmColor) DeleteObject(ii.hbmColor);
                    }
                }
                FillRect(small, &all, (HBRUSH)GetStockObject(BLACK_BRUSH));
                StretchBlt(small, g_dx, g_dy, g_dw, g_dh, full, 0, 0, gsw, gsh, SRCCOPY);
            } else {
                memcpy(px, g_scene, sizeof g_scene);
                if (g_drawptr) draw_cursor_small(small);
            }
            if (mode != MODE_OFF) draw_osk(small, fs);
            draw_overlay(small, fs, fl, mode, home || (int)(banner_until - now) > 0);
            if (g_settings_menu) draw_settings_menu(small, fs);
            GdiFlush();
            if (wupstream_frame(pad, px, WUPSTREAM_W * 4, WUPSTREAM_BGRA)) break;
            frames++;
            if (++tick >= 60) {
                DWORD t = GetTickCount();
                status("Streaming - %.0f fps, GamePad battery %d%%%s%s", frames * 1000.0 / (t - t0 ? t - t0 : 1), in.battery, usegdi ? " (basic capture)" : "",
                       g_mode == MODE_PAD ? (g_pad ? " - virtual controller active" : " - controller driver missing: install ViGEmBus") : "");
                tick = 0; frames = 0; t0 = t;
            }
        }
        release_all(); pad_close(); g_curpad = NULL;
        mstop = 1; if (mth) { WaitForSingleObject(mth, INFINITE); CloseHandle(mth); }
        astop = 1; if (ath) { WaitForSingleObject(ath, INFINITE); CloseHandle(ath); }
        wupstream_close(pad);
        if (g_run) status(disconnect_requested ? "GamePad disconnected. PC capture stays on; waiting for GamePad..." : "GamePad disconnected - reconnecting...");
    }
    pad_shutdown();
    dda_close(&d);
    DeleteObject(fs); DeleteObject(fl);
    DeleteObject(smallbmp); DeleteDC(small); DeleteObject(fullbmp); DeleteDC(full); ReleaseDC(NULL, scr);
    PostMessage(g_win, WM_STOPPED, 0, 0);
    return 0;
}

/* ---------------- window ---------------- */
static int g_dpi = 96;
#define S(x) MulDiv((x), g_dpi, 96)
static HWND g_help;
static const char *MODE_NAMES[4] = { "Keyboard + mouse", "Controller", "Desktop", "Off" };
static const char *HELP[4] = {
    "Left stick = W A S D, right stick = mouse look\nZR = fire (left click), ZL = aim (right click)\nA Space, B Ctrl, X E, Y R, L Q, R F\nL3 Shift (sprint), R3 V, D-pad = 1 2 3 4\n+ Esc, - Tab, HOME Enter\nTouch screen = mouse (for menus)",
    "Acts as an Xbox 360 controller in your games. Needs the free ViGEmBus driver installed (github.com/nefarius/ViGEmBus). Buttons match by position (bottom button = Xbox A), ZL/ZR are the triggers, and game rumble is sent to the GamePad. The touch screen still works as a mouse.",
    "Touch screen = mouse (drag to drag)\nLeft stick = move mouse, right stick = scroll\nZR = left click, ZL = right click, L3 = middle click\nD-pad = arrow keys, A Enter, B Esc, X Space, Y Tab\n- Backspace, + Windows key, L / R browser back / forward\nHOME = play/pause, R3 = mute",
    "The GamePad's controls are not sent to the PC.",
};

/* ---------------- fetch the controller driver (ViGEmBus) when it is missing ---------------- */
#define VIGEM_URL "https://github.com/nefarius/ViGEmBus/releases/download/v1.22.0/ViGEmBus_1.22.0_x64_x86_arm64.exe"
static int driver_present(void) {
    PVIGEM_CLIENT c = vigem_alloc();
    if (!c) return 0;
    int ok = VIGEM_SUCCESS(vigem_connect(c));
    if (ok) vigem_disconnect(c);
    vigem_free(c);
    return ok;
}
static DWORD WINAPI install_driver(LPVOID unused) {
    (void)unused;
    char tmp[MAX_PATH], exe[MAX_PATH];
    GetTempPathA(sizeof tmp, tmp); snprintf(exe, sizeof exe, "%sViGEmBus_setup.exe", tmp);
    status("Downloading the ViGEmBus controller driver...");
    WIN32_FILE_ATTRIBUTE_DATA fa;
    int got = URLDownloadToFileA(NULL, VIGEM_URL, exe, 0, NULL) == S_OK && GetFileAttributesExA(exe, GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 500000;
    if (!got) {
        DeleteFileA(exe);
        status("Could not download ViGEmBus - install it from github.com/nefarius/ViGEmBus/releases");
    } else {
        status("Installing ViGEmBus - click Yes on the Windows prompt, then finish its installer...");
        SHELLEXECUTEINFOA se; memset(&se, 0, sizeof se);
        se.cbSize = sizeof se; se.fMask = SEE_MASK_NOCLOSEPROCESS; se.lpVerb = "runas"; se.lpFile = exe; se.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExA(&se) && se.hProcess) { WaitForSingleObject(se.hProcess, 10 * 60 * 1000); CloseHandle(se.hProcess); }
        DeleteFileA(exe);
        status(driver_present() ? "ViGEmBus installed - Controller mode is ready" : "ViGEmBus is not active yet - finish its installer (a restart may be needed)");
    }
    g_vgtry = 0; g_vginstalling = 0;
    return 0;
}
/* Controller mode was picked: if the driver is missing, offer to fetch it (once per start) */
static void check_driver(int mode) {
    if (mode == MODE_PAD && !g_vginstalling && !g_vgasked && !driver_present()) { g_vgasked = 1; PostMessage(g_win, WM_NEEDDRIVER, 0, 0); }
}

/* settings are remembered in wupclient.ini next to the exe */
static char g_ini[MAX_PATH];
static void save_cfg(void) {
    char ip[64], v[16];
    if (!g_ini[0]) return;
    GetWindowTextA(g_ip, ip, sizeof ip);
    WritePrivateProfileStringA("wup", "ip", ip, g_ini);
    WritePrivateProfileStringA("wup", "audio", SendMessage(g_audio, BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0", g_ini);
    WritePrivateProfileStringA("wup", "pointer", g_drawptr ? "1" : "0", g_ini);
    WritePrivateProfileStringA("wup", "mic", g_mic_enabled ? "1" : "0", g_ini);
    WritePrivateProfileStringA("wup", "kbps", g_stream_kbps <= 3500 ? "3500" : "4500", g_ini);
    snprintf(v, sizeof v, "%d", (int)g_mode);
    WritePrivateProfileStringA("wup", "mode", v, g_ini);
    snprintf(v, sizeof v, "%d", (int)g_looksens); WritePrivateProfileStringA("wup", "look", v, g_ini);
    snprintf(v, sizeof v, "%d", (int)g_decksens); WritePrivateProfileStringA("wup", "cursor", v, g_ini);
}
static void load_cfg(void) {
    char ip[64];
    GetModuleFileNameA(NULL, g_ini, sizeof g_ini);
    char *dot = strrchr(g_ini, '.'); if (dot) strcpy(dot, ".ini"); else strcat(g_ini, ".ini");
    GetPrivateProfileStringA("wup", "ip", "", ip, sizeof ip, g_ini);
    SetWindowTextA(g_ip, ip);
    g_mic_enabled = GetPrivateProfileIntA("wup", "mic", 0, g_ini) ? 1 : 0;
    g_stream_kbps = GetPrivateProfileIntA("wup", "kbps", 4500, g_ini) <= 3500 ? 3500 : 4500;
    SendMessage(g_audio, BM_SETCHECK, GetPrivateProfileIntA("wup", "audio", 1, g_ini) ? BST_CHECKED : BST_UNCHECKED, 0);
    int p = GetPrivateProfileIntA("wup", "pointer", 1, g_ini);
    SendMessage(g_ptr, BM_SETCHECK, p ? BST_CHECKED : BST_UNCHECKED, 0); g_drawptr = p;
    g_looksens = GetPrivateProfileIntA("wup", "look", 12, g_ini);        /* 1-100: right-stick mouse look speed */
    g_decksens = GetPrivateProfileIntA("wup", "cursor", 9, g_ini);         /* 1-100: left-stick desktop cursor speed */
    int m = GetPrivateProfileIntA("wup", "mode", 0, g_ini);
    if (m < 0 || m > 3) m = 0;
    g_mode = m; SendMessage(g_ctrl, CB_SETCURSEL, (WPARAM)m, 0); SetWindowTextA(g_help, HELP[m]);
}

static void start(void) {
    GetWindowTextA(g_ip, g_ipstr, sizeof g_ipstr);
    g_useaudio = SendMessage(g_audio, BM_GETCHECK, 0, 0) == BST_CHECKED;
    save_cfg();
    g_vgasked = 0;
    g_run = 1;
    EnableWindow(g_ip, FALSE); EnableWindow(g_audio, FALSE);
    SetWindowTextA(g_btn, "Stop");
    g_thread = CreateThread(NULL, 0, stream_thread, NULL, 0, NULL);
}
static void stop(void) {
    g_run = 0;
    EnableWindow(g_btn, FALSE);
    SetWindowTextA(g_status, "Stopping...");
}

static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND:
        if (LOWORD(w) == ID_START) { if (g_run) stop(); else start(); }
        else if (LOWORD(w) == ID_PTR) { g_drawptr = SendMessage(g_ptr, BM_GETCHECK, 0, 0) == BST_CHECKED; save_cfg(); }
        else if (LOWORD(w) == ID_CTRL && HIWORD(w) == CBN_SELCHANGE) {
            int i = (int)SendMessage(g_ctrl, CB_GETCURSEL, 0, 0);
            if (i >= 0 && i < 4) { g_mode = i; SetWindowTextA(g_help, HELP[i]); save_cfg(); check_driver(i); }
        }
        return 0;
    case WM_NEEDDRIVER:
        if (MessageBoxA(h, "Controller mode needs the free ViGEmBus driver, and it is not installed.\n\nDownload and install it now? (Windows will ask for permission.)",
                        "WUP Stream Client", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            g_vginstalling = 1;
            HANDLE t = CreateThread(NULL, 0, install_driver, NULL, 0, NULL);
            if (t) CloseHandle(t); else g_vginstalling = 0;
        }
        return 0;
    case WM_UPDATE_AVAILABLE: {
        update_info *u=(update_info*)l;
        char msg[256]; snprintf(msg,sizeof msg,"Version %s is available. Download and install it now?",u->tag);
        if (MessageBoxA(h,msg,"WUP Stream Client update",MB_YESNO|MB_ICONINFORMATION)==IDYES) {
            HANDLE t=CreateThread(NULL,0,install_update_thread,u,0,NULL);
            if (t) CloseHandle(t); else free(u);
        } else free(u);
        return 0;
    }
    case WM_STATUS: SetWindowTextA(g_status, (char *)l); free((void *)l); return 0;
    case WM_SETTINGS_CHANGED:
        SendMessage(g_ptr, BM_SETCHECK, g_drawptr ? BST_CHECKED : BST_UNCHECKED, 0);
        save_cfg();
        status("Settings saved. Mic needs VB-CABLE; quality applies after reconnect.");
        return 0;
    case WM_SETMODE: {
        int i = (int)w;
        if (i >= 0 && i < 4) {
            g_mode = i;
            SendMessage(g_ctrl, CB_SETCURSEL, (WPARAM)i, 0);
            SetWindowTextA(g_help, HELP[i]);
            save_cfg(); check_driver(i);
            status("Control profile: %s (GamePad HOME + +/-)", MODE_NAMES[i]);
        }
        return 0;
    }
    case WM_STOPPED:
        if (g_thread) { WaitForSingleObject(g_thread, 5000); CloseHandle(g_thread); g_thread = NULL; }
        SetWindowTextA(g_btn, "Start streaming"); EnableWindow(g_btn, TRUE);
        EnableWindow(g_ip, TRUE); EnableWindow(g_audio, TRUE);
        SetWindowTextA(g_status, "Stopped");
        return 0;
    case WM_CLOSE:
        if (g_run) { g_run = 0; if (g_thread) { WaitForSingleObject(g_thread, INFINITE); CloseHandle(g_thread); g_thread = NULL; } }
        DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

static HWND mk(const char *cls, const char *text, DWORD style, int x, int y, int w, int h, HMENU id, HFONT f, HINSTANCE inst) {
    HWND c = CreateWindowA(cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), g_win, id, inst, NULL);
    SendMessage(c, WM_SETFONT, (WPARAM)f, 1);
    return c;
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    (void)prev; (void)cmd;
    SetProcessDPIAware();
    { HDC dc = GetDC(NULL); g_dpi = GetDeviceCaps(dc, LOGPIXELSX); ReleaseDC(NULL, dc); }
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    WNDCLASSA wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = proc; wc.hInstance = inst; wc.lpszClassName = "WupClient";
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1); wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = {0, 0, S(420), S(410)}; AdjustWindowRect(&r, style, FALSE);
    g_win = CreateWindowA("WupClient", "WUP Stream Client", style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    HFONT f = CreateFontA(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    HFONT fb = CreateFontA(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    mk("STATIC", "Mirror this PC's screen and sound to your Wii U GamePad.\nOpen \"WUP Stream\" on the GamePad first (same Wi-Fi).", 0, 16, 12, 388, 36, NULL, f, inst);
    mk("STATIC", "GamePad IP (leave empty to auto-detect)", 0, 16, 58, 388, 18, NULL, f, inst);
    g_ip = mk("EDIT", "", WS_BORDER | ES_AUTOHSCROLL, 16, 78, 388, 24, (HMENU)ID_IP, f, inst);
    g_audio = mk("BUTTON", "Stream PC sound", BS_AUTOCHECKBOX, 16, 112, 150, 22, (HMENU)ID_AUDIO, f, inst);
    g_ptr = mk("BUTTON", "Draw mouse pointer (untick if you see two)", BS_AUTOCHECKBOX, 170, 112, 234, 22, (HMENU)ID_PTR, f, inst);
    SendMessage(g_ptr, BM_SETCHECK, BST_CHECKED, 0);
    SendMessage(g_audio, BM_SETCHECK, BST_CHECKED, 0);
    mk("STATIC", "GamePad controls:", 0, 16, 150, 110, 22, NULL, f, inst);
    g_ctrl = mk("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL, 126, 146, 278, 120, (HMENU)ID_CTRL, f, inst);
    SendMessageA(g_ctrl, CB_ADDSTRING, 0, (LPARAM)"Keyboard + mouse (games)");
    SendMessageA(g_ctrl, CB_ADDSTRING, 0, (LPARAM)"Controller (virtual Xbox 360)");
    SendMessageA(g_ctrl, CB_ADDSTRING, 0, (LPARAM)"Desktop (mouse + keys)");
    SendMessageA(g_ctrl, CB_ADDSTRING, 0, (LPARAM)"Off");
    SendMessage(g_ctrl, CB_SETCURSEL, 0, 0);
    g_btn = mk("BUTTON", "Start streaming", BS_DEFPUSHBUTTON, 16, 184, 388, 38, (HMENU)ID_START, fb, inst);
    g_status = mk("STATIC", "Ready", 0, 16, 232, 388, 20, NULL, fb, inst);
    g_help = mk("STATIC", HELP[0], 0, 16, 260, 388, 130, NULL, f, inst);
    load_cfg();
    ShowWindow(g_win, show); UpdateWindow(g_win);
    { HANDLE t=CreateThread(NULL,0,check_updates_thread,NULL,0,NULL); if (t) CloseHandle(t); }
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessage(&msg); }
    WSACleanup();
    return 0;
}

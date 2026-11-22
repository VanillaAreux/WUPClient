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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wupstream.h"
#include "ViGEm/Client.h"

#define ID_START 101
#define ID_AUDIO 102
#define ID_IP    103
#define ID_CTRL  104
#define WM_STATUS (WM_APP + 1)
#define WM_STOPPED (WM_APP + 2)

static const GUID MY_CLSID_Enum = {0xBCDE0395,0xE52F,0x467C,{0x8E,0x3D,0xC4,0x57,0x92,0x91,0x69,0x2E}};
static const GUID MY_IID_Enum   = {0xA95664D2,0x9614,0x4F35,{0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6}};
static const GUID MY_IID_Client = {0x1CB9AD4C,0xDBFA,0x4C32,{0xB1,0x78,0xC2,0xF5,0x68,0xA7,0x03,0xB2}};
static const GUID MY_IID_Capture= {0xC8ADBD64,0xE71E,0x48A0,{0xA4,0xDE,0x18,0x5C,0x39,0x5C,0xD3,0x17}};

static HWND g_win, g_btn, g_audio, g_ip, g_status, g_ctrl;
static volatile LONG g_run;         /* 1 while the user wants streaming */
static HANDLE g_thread;
static char g_ipstr[64];
static int g_useaudio;

static void status(const char *fmt, ...) {
    char buf[200]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    PostMessageA(g_win, WM_STATUS, 0, (LPARAM)_strdup(buf));
}

/* ---------------- audio: WASAPI loopback of the default output -> GamePad ---------------- */
typedef struct { wupstream *pad; volatile LONG *stop; } audio_arg;

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


/* ---------------- input: GamePad -> this PC ----------------
 * Four modes (pick one in the window):
 *  Keyboard + mouse (games):  left stick = W A S D, right stick = mouse look, ZR = fire (left click), ZL = aim (right click),
 *      A Space, B Ctrl (crouch), X E, Y R, L Q, R F, L3 Shift (sprint), R3 V (melee), D-pad = 1 2 3 4,
 *      + Esc, - Tab, HOME Enter. The touch screen is still a mouse (menus).
 *  Controller (Xbox 360): a virtual Xbox 360 pad (needs the ViGEmBus driver). Face buttons by position (the GamePad's bottom
 *      button is Xbox A), ZL/ZR = triggers, L/R = bumpers, sticks, D-pad, + Start, - Back, HOME Guide. Rumble comes back to the pad.
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
static volatile LONG g_mode;                         /* MODE_* */

/* the virtual Xbox 360 controller (ViGEmBus) */
static PVIGEM_CLIENT g_vg; static PVIGEM_TARGET g_pad; static DWORD g_vgtry;
static wupstream *volatile g_curpad;                 /* the GamePad, for rumble */
static XUSB_REPORT g_lastrep; static int g_haverep;
static void CALLBACK pad_rumble(PVIGEM_CLIENT c, PVIGEM_TARGET t, UCHAR large, UCHAR small_, UCHAR led, LPVOID user) {
    (void)c; (void)t; (void)led; (void)user;
    wupstream *p = g_curpad;
    if (p) wupstream_rumble(p, large || small_);
}
static void pad_open(void) {
    if (g_vgtry && GetTickCount() - g_vgtry < 3000) return;      /* (not every frame while the driver is missing) */
    g_vgtry = GetTickCount();
    if (!g_vg) {
        g_vg = vigem_alloc();
        if (!g_vg) return;
        if (!VIGEM_SUCCESS(vigem_connect(g_vg))) { vigem_free(g_vg); g_vg = NULL; return; }
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
    g_pad = NULL;
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
    if (b & WUPSTREAM_HOME) w |= XUSB_GAMEPAD_GUIDE;
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

static void release_all(void) {
    for (size_t n = 0; n < g_ntab; n++) if (g_prev & g_tab[n].mask) send_key(&g_tab[n], 0);
    for (size_t n = 0; n < g_nclk; n++) if (g_prev & g_clk[n].mask) send_mouse(g_clk[n].up, 0, 0, 0);
    if (g_prevtouch) send_mouse(MOUSEEVENTF_LEFTUP, 0, 0, 0);
    g_prev = 0; g_prevtouch = 0; g_scroll = g_lookx = g_looky = 0;
}

static void apply_input(const wupstream_input_t *in, int mode) {
    const keymap *tab = mode == MODE_KM ? KM_KEYS : mode == MODE_DESK ? DESK_KEYS : NO_KEYS;
    if (tab != g_tab) {                              /* the mode changed: let go of everything the old one held */
        release_all();
        g_tab = tab; g_ntab = mode == MODE_KM ? NKM : mode == MODE_DESK ? NDESK : 0;
        g_clk = mode == MODE_KM ? KM_CLICKS : DESK_CLICKS; g_nclk = mode == MODE_KM ? 2 : mode == MODE_DESK ? 3 : 0;
    }
    uint32_t b = in->buttons;
    if (mode == MODE_PAD) { pad_update(in); b = 0; }
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

    if (in->touched) {                               /* the picture sits in a box on the pad's screen: map back to the PC screen */
        int x = (in->tx - g_dx) * g_sw / g_dw, y = (in->ty - g_dy) * g_sh / g_dh;
        x = x < 0 ? 0 : x >= g_sw ? g_sw - 1 : x; y = y < 0 ? 0 : y >= g_sh ? g_sh - 1 : y;
        send_mouse(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE, x * 65535 / (g_sw - 1 ? g_sw - 1 : 1), y * 65535 / (g_sh - 1 ? g_sh - 1 : 1), 0);
        if (!g_prevtouch) send_mouse(MOUSEEVENTF_LEFTDOWN, 0, 0, 0);
    } else if (g_prevtouch) send_mouse(MOUSEEVENTF_LEFTUP, 0, 0, 0);
    g_prevtouch = in->touched;

    if (mode == MODE_KM) {                           /* right stick: mouse look (a curve: small pushes are precise, fractions are kept) */
        int rx = dead(in->rx), ry = dead(in->ry);
        double fx = rx / 950.0, fy = ry / 950.0;
        g_lookx += fx * (fx < 0 ? -fx : fx) * 30.0; g_looky -= fy * (fy < 0 ? -fy : fy) * 30.0;
        int mx = (int)g_lookx, my = (int)g_looky;
        g_lookx -= mx; g_looky -= my;
        if (mx || my) send_mouse(MOUSEEVENTF_MOVE, mx, my, 0);
    } else if (mode == MODE_DESK) {
        int lx = dead(in->lx), ly = dead(in->ly);    /* left stick: mouse */
        if (lx || ly) {
            double fx = lx / 950.0, fy = ly / 950.0;
            int mx = (int)(fx * (fx < 0 ? -fx : fx) * 24 + (fx > 0 ? 1 : fx < 0 ? -1 : 0));
            int my = (int)(-fy * (fy < 0 ? -fy : fy) * 24 - (fy > 0 ? 1 : fy < 0 ? -1 : 0));
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
    _putenv_s("WUPSTREAM_KBPS", "6000");
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

    dda d; int usegdi = !dda_open(&d); DWORD lastretry = 0;
    geometry(usegdi ? gsw : d.w, usegdi ? gsh : d.h);
    memset(g_scene, 0, sizeof g_scene);

    while (g_run) {
        status("Looking for a GamePad (open WUP Stream from the Homebrew Menu)...");
        wupstream *pad = NULL;
        while (g_run && !(pad = wupstream_open(g_ipstr[0] ? g_ipstr : NULL, 2000))) {}
        if (!pad) break;
        status("Streaming to the GamePad");

        volatile LONG astop = 0; HANDLE ath = NULL; audio_arg aa = { pad, &astop };
        if (g_useaudio) ath = CreateThread(NULL, 0, audio_thread, &aa, 0, NULL);

        g_curpad = pad;
        wupstream_input_t in; int tick = 0; DWORD t0 = GetTickCount(); int frames = 0;
        while (g_run && wupstream_input(pad, &in) == 0) {
            int mode = (int)g_mode;
            if (mode == MODE_PAD) { if (!g_pad) pad_open(); } else if (g_pad) pad_close();
            if (mode != MODE_OFF) apply_input(&in, mode); else if (g_prev || g_prevtouch) release_all();

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
                if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
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
                draw_cursor_small(small);
            }
            GdiFlush();
            if (wupstream_frame(pad, px, WUPSTREAM_W * 4, WUPSTREAM_BGRA)) break;
            frames++;
            if (++tick >= 60) {
                DWORD t = GetTickCount();
                status("Streaming - %.0f fps, GamePad battery %d%%%s%s", frames * 1000.0 / (t - t0 ? t - t0 : 1), in.battery, usegdi ? " (basic capture)" : "",
                       (g_mode == MODE_PAD && !g_pad) ? " - controller driver missing: install ViGEmBus" : "");
                tick = 0; frames = 0; t0 = t;
            }
        }
        release_all(); pad_close(); g_curpad = NULL;
        astop = 1; if (ath) { WaitForSingleObject(ath, 3000); CloseHandle(ath); }
        wupstream_close(pad);
        if (g_run) status("GamePad disconnected - reconnecting...");
    }
    pad_shutdown();
    dda_close(&d);
    DeleteObject(smallbmp); DeleteDC(small); DeleteObject(fullbmp); DeleteDC(full); ReleaseDC(NULL, scr);
    PostMessage(g_win, WM_STOPPED, 0, 0);
    return 0;
}

/* ---------------- window ---------------- */
static int g_dpi = 96;
#define S(x) MulDiv((x), g_dpi, 96)
static HWND g_help;
static const char *HELP[4] = {
    "Left stick = W A S D, right stick = mouse look\nZR = fire (left click), ZL = aim (right click)\nA Space, B Ctrl, X E, Y R, L Q, R F\nL3 Shift (sprint), R3 V, D-pad = 1 2 3 4\n+ Esc, - Tab, HOME Enter\nTouch screen = mouse (for menus)",
    "Acts as an Xbox 360 controller in your games. Needs the free ViGEmBus driver installed (github.com/nefarius/ViGEmBus). Buttons match by position (bottom button = Xbox A), ZL/ZR are the triggers, and game rumble is sent to the GamePad. The touch screen still works as a mouse.",
    "Touch screen = mouse (drag to drag)\nLeft stick = move mouse, right stick = scroll\nZR = left click, ZL = right click, L3 = middle click\nD-pad = arrow keys, A Enter, B Esc, X Space, Y Tab\n- Backspace, + Windows key, L / R browser back / forward\nHOME = play/pause, R3 = mute",
    "The GamePad's controls are not sent to the PC.",
};

static void start(void) {
    GetWindowTextA(g_ip, g_ipstr, sizeof g_ipstr);
    g_useaudio = SendMessage(g_audio, BM_GETCHECK, 0, 0) == BST_CHECKED;
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
        else if (LOWORD(w) == ID_CTRL && HIWORD(w) == CBN_SELCHANGE) {
            int i = (int)SendMessage(g_ctrl, CB_GETCURSEL, 0, 0);
            if (i >= 0 && i < 4) { g_mode = i; SetWindowTextA(g_help, HELP[i]); }
        }
        return 0;
    case WM_STATUS: SetWindowTextA(g_status, (char *)l); free((void *)l); return 0;
    case WM_STOPPED:
        if (g_thread) { WaitForSingleObject(g_thread, 5000); CloseHandle(g_thread); g_thread = NULL; }
        SetWindowTextA(g_btn, "Start streaming"); EnableWindow(g_btn, TRUE);
        EnableWindow(g_ip, TRUE); EnableWindow(g_audio, TRUE);
        SetWindowTextA(g_status, "Stopped");
        return 0;
    case WM_CLOSE:
        if (g_run) { g_run = 0; if (g_thread) WaitForSingleObject(g_thread, 6000); }
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
    RECT r = {0, 0, S(420), S(400)}; AdjustWindowRect(&r, style, FALSE);
    g_win = CreateWindowA("WupClient", "WUP Stream Client", style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
    HFONT f = CreateFontA(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    HFONT fb = CreateFontA(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
    mk("STATIC", "Mirror this PC's screen and sound to your Wii U GamePad.\nOpen \"WUP Stream\" on the GamePad first (same Wi-Fi).", 0, 16, 12, 388, 36, NULL, f, inst);
    mk("STATIC", "GamePad IP (leave empty to auto-detect)", 0, 16, 58, 388, 18, NULL, f, inst);
    g_ip = mk("EDIT", "", WS_BORDER | ES_AUTOHSCROLL, 16, 78, 388, 24, (HMENU)ID_IP, f, inst);
    g_audio = mk("BUTTON", "Stream PC sound to the GamePad", BS_AUTOCHECKBOX, 16, 112, 388, 22, (HMENU)ID_AUDIO, f, inst);
    SendMessage(g_audio, BM_SETCHECK, BST_CHECKED, 0);
    mk("STATIC", "GamePad controls:", 0, 16, 146, 110, 22, NULL, f, inst);
    g_ctrl = mk("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL, 126, 142, 278, 120, (HMENU)ID_CTRL, f, inst);
    SendMessageA(g_ctrl, CB_ADDSTRING, 0, (LPARAM)"Keyboard + mouse (games)");
    SendMessageA(g_ctrl, CB_ADDSTRING, 0, (LPARAM)"Controller (virtual Xbox 360)");
    SendMessageA(g_ctrl, CB_ADDSTRING, 0, (LPARAM)"Desktop (mouse + keys)");
    SendMessageA(g_ctrl, CB_ADDSTRING, 0, (LPARAM)"Off");
    SendMessage(g_ctrl, CB_SETCURSEL, 0, 0);
    g_btn = mk("BUTTON", "Start streaming", BS_DEFPUSHBUTTON, 16, 178, 388, 38, (HMENU)ID_START, fb, inst);
    g_status = mk("STATIC", "Ready", 0, 16, 226, 388, 20, NULL, fb, inst);
    g_help = mk("STATIC", HELP[0], 0, 16, 256, 388, 130, NULL, f, inst);
    ShowWindow(g_win, show); UpdateWindow(g_win);
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessage(&msg); }
    WSACleanup();
    return 0;
}

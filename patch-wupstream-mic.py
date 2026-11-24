#!/usr/bin/env python3
"""Patch the bundled wup-stream host receiver to account for UDP mic packet loss/reordering."""
from pathlib import Path
import sys

p = Path(sys.argv[1])
s = p.read_text()

old = "int16_t micbuf[48000]; int mic_w, mic_n;                 /* the last second of the microphone */"
new = """int16_t micbuf[48000]; int mic_w, mic_n;                 /* the last second of the microphone */
    uint32_t mic_seq_next;
    int mic_seq_valid;"""
if old not in s:
    raise SystemExit("Could not find upstream microphone buffer declaration")
s = s.replace(old, new, 1)

anchor = "static void pump(wupstream *w) {"
helper = r'''static void mic_push_sample(wupstream *w, int16_t sample) {
    w->micbuf[w->mic_w] = sample;
    w->mic_w = (w->mic_w + 1) % 48000;
    if (w->mic_n < 48000) w->mic_n++;
}

/* UDP microphone packets carry their first sample index in bytes 4..7.
   Reconstruct small gaps smoothly and discard duplicate/late samples instead
   of concatenating discontinuous packet payloads (which sounds like crackle). */
static void mic_packet(wupstream *w, const uint8_t *b, int n) {
    if (n <= 8) return;
    uint32_t seq;
    memcpy(&seq, b + 4, sizeof seq);
    int count = (n - 8) / 2;
    int skip = 0;

    if (!w->mic_seq_valid) {
        w->mic_seq_next = seq;
        w->mic_seq_valid = 1;
    }

    int32_t delta = (int32_t)(seq - w->mic_seq_next);
    if (delta < 0) {
        skip = (int)(-delta);
        if (skip >= count) return;       /* duplicate or entirely late packet */
        seq += (uint32_t)skip;
    } else if (delta > 0) {
        int gap = delta > 4800 ? 4800 : (int)delta; /* cap concealment to 100 ms */
        int16_t first = (int16_t)(b[8] | ((uint16_t)b[9] << 8));
        int16_t last = w->mic_n ? w->micbuf[(w->mic_w + 47999) % 48000] : first;
        for (int i = 0; i < gap; i++) {
            int32_t v = (int32_t)last + ((int32_t)first - last) * (i + 1) / (gap + 1);
            mic_push_sample(w, (int16_t)v);
        }
    }

    for (int i = skip; i < count; i++) {
        int at = 8 + i * 2;
        mic_push_sample(w, (int16_t)(b[at] | ((uint16_t)b[at + 1] << 8)));
    }
    w->mic_seq_next = seq + (uint32_t)(count - skip);
}

static void pump(wupstream *w) {'''
if anchor not in s:
    raise SystemExit("Could not find upstream receive pump")
s = s.replace(anchor, helper, 1)

old = """        if (b[0] == 'M' && n > 8) for (int i = 8; i + 1 < n; i += 2) {   /* the microphone: 16-bit mono, 48 kHz */
            w->micbuf[w->mic_w] = (int16_t)(b[i] | b[i + 1] << 8); w->mic_w = (w->mic_w + 1) % 48000;
            if (w->mic_n < 48000) w->mic_n++;
        }"""
new = """        if (b[0] == 'M' && n > 8) mic_packet(w, b, n);"""
if old not in s:
    raise SystemExit("Could not find upstream microphone packet handler")
s = s.replace(old, new, 1)

old = "if (!w->mic) { w->mic = 1; w->last_hello = 0; }"
new = "if (!w->mic) { w->mic = 1; w->mic_seq_valid = 0; w->last_hello = 0; }"
if old not in s:
    raise SystemExit("Could not find upstream microphone enable logic")
s = s.replace(old, new, 1)

p.write_text(s)

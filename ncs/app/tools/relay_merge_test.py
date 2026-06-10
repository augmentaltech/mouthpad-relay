#!/usr/bin/env python3
"""SFP-667 merge test: stream MouthPad mouse_state sensor data AND the relay's
Opus mic at the same time, through the relay, and verify both.

  - Sensor: count true SensorData/mouse_state packets and check the rate matches
    the firmware default (~20 Hz).
  - Audio:  decode the relay_audio_frame Opus (libopus) and confirm a ~1 kHz tone
    (played into the mic with voice-memo-scripts/gen_tone.py) is dominant.

Runs on the Pi (BLE central). Requires the Pi bonded to the relay and libopus.so
(present at /usr/lib/aarch64-linux-gnu/libopus.so.0). The MouthPad must be
advertising so the relay connects to it — reset it (nrfjprog) right before.

  ~/blenv/bin/python relay_merge_test.py [window_s] [tone_hz]
"""
import asyncio, ctypes, struct, sys, math
from bleak import BleakClient, BleakScanner

RELAY = "DE:B7:53:18:22:29"
RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

WINDOW_S  = int(sys.argv[1]) if len(sys.argv) > 1 else 10
TONE_HZ   = float(sys.argv[2]) if len(sys.argv) > 2 else 1000.0
EXPECT_HZ = 20.0          # firmware default sensor rate
RATE_TOL  = 0.25          # ±25%

# AppToRelayMessage{dest=MOUTHPAD, pass_through{MouthwareMessage{
#   DataStreamConfigWrite{sensor_type=[MOUSE=7], stream_period_ms=50}}}}  -> 20 Hz
START_MOUSE = bytes([0x08,0x02,0x1A,0x0B,0x0A,0x09,0x08,0x01,0x1A,0x05,0x0A,0x01,0x07,0x10,0x32])
MIC_ENABLE  = bytes([0x08,0x01,0x42,0x04,0x08,0x14,0x10,0x01])
MIC_DISABLE = bytes([0x08,0x01,0x42,0x02,0x10,0x00])

def walk(buf):
    """Yield (field_number, wire_type, value_bytes)."""
    i = 0
    while i < len(buf):
        key = buf[i]; i += 1
        fnum, wt = key >> 3, key & 7
        if wt == 0:
            s = i
            while i < len(buf) and buf[i] & 0x80: i += 1
            i += 1; yield fnum, wt, buf[s:i]
        elif wt == 2:
            ln = 0; sh = 0
            while buf[i] & 0x80: ln |= (buf[i]&0x7f)<<sh; sh += 7; i += 1
            ln |= (buf[i]&0x7f)<<sh; i += 1
            yield fnum, wt, buf[i:i+ln]; i += ln
        elif wt == 5:
            yield fnum, wt, buf[i:i+4]; i += 4
        else:
            return

def field(buf, num):
    for fn, wt, val in walk(buf):
        if fn == num: return bytes(val)
    return None

# counters (windowed)
cnt = {"sensor":0, "mouse":0, "audio":0, "other":0}
sensor_indices = []   # SensorData.index per packet (to dedup double-delivery)
audio_frames = []
measuring = False

def varint(buf):
    v = 0; sh = 0
    for x in buf:
        v |= (x & 0x7f) << sh; sh += 7
    return v

def handler(_, data):
    b = bytes(data)
    if not b: return
    if b[0] == 0x1A:  # RelayToAppMessage.pass_through_to_app
        pta = field(b, 3)                 # PassThroughToApp
        m2a = field(pta, 1) if pta else None  # MouthpadToAppMessage bytes
        sd = field(m2a, 2) if m2a else None   # SensorData (field 2)
        if sd is not None and measuring:
            cnt["sensor"] += 1
            idx = field(sd, 1)              # SensorData.index (varint)
            sensor_indices.append(varint(idx) if idx is not None else -1)
            if field(sd, 10) is not None:  # mouse_state
                cnt["mouse"] += 1
    elif b[0] == 0x42:  # relay_audio_frame
        raf = field(b, 8)
        ch0 = field(raf, 4) if raf else None  # spectral_coeffs_ch0
        if ch0 and measuring:
            cnt["audio"] += 1
            audio_frames.append(ch0)
    else:
        if measuring: cnt["other"] += 1

def goertzel(pcm, fs, f):
    w = 2*math.cos(2*math.pi*f/fs); s1 = s2 = 0.0
    for x in pcm: s0 = x + w*s1 - s2; s2 = s1; s1 = s0
    return s1*s1 + s2*s2 - w*s1*s2

def verify_tone():
    lib = ctypes.CDLL("/usr/lib/aarch64-linux-gnu/libopus.so.0")
    lib.opus_decoder_create.restype = ctypes.c_void_p
    lib.opus_decoder_create.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int)]
    lib.opus_decode.restype = ctypes.c_int
    lib.opus_decode.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(ctypes.c_int16), ctypes.c_int, ctypes.c_int]
    err = ctypes.c_int(0); dec = lib.opus_decoder_create(16000, 1, ctypes.byref(err))
    pcm = []
    for fr in audio_frames:
        buf = (ctypes.c_int16*320)(); n = lib.opus_decode(dec, fr, len(fr), buf, 320, 0)
        if n > 0: pcm += list(buf[:n])
    if not pcm: return 0, 0.0, {}
    cands = [TONE_HZ*0.5, TONE_HZ*0.9, TONE_HZ, TONE_HZ*1.1, 200, 4000]
    powers = {f: goertzel(pcm, 16000, f) for f in cands}
    dom = max(powers, key=powers.get)
    return len(pcm), dom, powers

async def main():
    print("scanning for relay...", flush=True)
    dev = await BleakScanner.find_device_by_address(RELAY, timeout=25)
    if not dev: print("RELAY NOT FOUND"); return
    async with BleakClient(dev, timeout=30) as c:
        print("connected:", c.is_connected, flush=True)
        await asyncio.sleep(1)
        await c.start_notify(TX, handler)
        print("start mouse_state stream (default 20 Hz)...", flush=True)
        await c.write_gatt_char(RX, START_MOUSE, response=False)
        await asyncio.sleep(4)   # let the relay link to the MouthPad + stream settle
        print("enable relay mic...", flush=True)
        await c.write_gatt_char(RX, MIC_ENABLE, response=False)
        await asyncio.sleep(1)
        global measuring
        print(f"measuring {WINDOW_S}s (both streams)...", flush=True)
        measuring = True
        await asyncio.sleep(WINDOW_S)
        measuring = False
        await c.write_gatt_char(RX, MIC_DISABLE, response=False)
        await c.stop_notify(TX)

    uniq = len(set(i for i in sensor_indices if i >= 0))
    sensor_hz = cnt["sensor"]/WINDOW_S
    uniq_hz   = uniq/WINDOW_S
    audio_hz  = cnt["audio"]/WINDOW_S
    dup = cnt["sensor"]/uniq if uniq else 0
    print(f"\n--- results over {WINDOW_S}s ---")
    print(f"sensor packets={cnt['sensor']} ({sensor_hz:.1f} Hz raw), unique index={uniq} ({uniq_hz:.1f} Hz), dup x{dup:.2f}, mouse_state={cnt['mouse']}")
    print(f"audio frames={cnt['audio']} ({audio_hz:.1f} Hz), other={cnt['other']}")

    rate_ok = abs(uniq_hz - EXPECT_HZ) <= EXPECT_HZ*RATE_TOL and cnt["mouse"] > 0
    print(f"[{'PASS' if rate_ok else 'FAIL'}] sensor rate (unique) {uniq_hz:.1f} Hz vs {EXPECT_HZ:.0f} Hz (±{int(RATE_TOL*100)}%), mouse_state present={cnt['mouse']>0}")

    nsamp, dom, powers = verify_tone()
    tone_ok = abs(dom - TONE_HZ) < TONE_HZ*0.1 and cnt["audio"] > 0
    print(f"[{'PASS' if tone_ok else 'FAIL'}] audio tone dominant={dom:.0f} Hz (expect {TONE_HZ:.0f}), {nsamp} samples decoded")
    if powers: print("  goertzel:", {int(k): round(v/max(powers.values()),3) for k,v in powers.items()})

    print(f"\nOVERALL: {'PASS' if (rate_ok and tone_ok) else 'FAIL'}")

asyncio.run(main())

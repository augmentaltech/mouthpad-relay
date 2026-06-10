#!/usr/bin/env python3
"""SFP-667 (c) relay-mic test: drive the relay from the Pi as the NUS host,
gate its Opus mic via SetMicrophoneParams, and verify the audio stream merges
with the relayed MouthPad sensor stream.

Phases:
  A: start MouthPad sensor stream, 5 s  -> expect stream>0, audio==0 (gated off)
  B: enable relay mic, 10 s             -> expect audio>0 AND stream still flowing
  C: disable relay mic, 3 s             -> expect audio stops

Saves the Opus frames (spectral_coeffs_ch0) length-prefixed to relay_audio.bin
for offline libopus decode.

Run on the Pi: ~/blenv/bin/python relay_mic_test.py
Requires the Pi already bonded to the relay (bluetoothctl pair) and the relay
untrusted+disconnected so it advertises. See sfp-667-companion-stream-e2e memory.
"""
import asyncio
import struct
from bleak import BleakClient, BleakScanner

RELAY = "DE:B7:53:18:22:29"
RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"

# AppToRelayMessage{dest=MOUTHPAD, pass_through_to_mouthpad{DataStreamConfigWrite[CAP,BARO,THERM,VBAT,MOUSE]}}
START_STREAM = bytes([0x08,0x02,0x1A,0x0D,0x0A,0x0B,0x08,0x01,0x1A,0x07,0x0A,0x05,0x02,0x04,0x05,0x06,0x07])
# AppToRelayMessage{dest=RELAY, set_microphone_params{gain=20, enable_channel_0=true}}
MIC_ENABLE  = bytes([0x08,0x01,0x42,0x04,0x08,0x14,0x10,0x01])
# AppToRelayMessage{dest=RELAY, set_microphone_params{enable_channel_0=false}}
MIC_DISABLE = bytes([0x08,0x01,0x42,0x02,0x10,0x00])

counts = {"stream":0,"status":0,"audio":0,"other":0}
audio_frames = []  # list of opus ch0 byte strings

def _walk_fields(buf):
    """Yield (field_number, wire_type, value_bytes) for a protobuf buffer."""
    i = 0
    while i < len(buf):
        key = buf[i]; i += 1
        fnum = key >> 3; wt = key & 0x07
        if wt == 0:  # varint
            start = i
            while buf[i] & 0x80: i += 1
            i += 1
            yield fnum, wt, buf[start:i]
        elif wt == 2:  # length-delimited
            ln = 0; shift = 0
            while buf[i] & 0x80:
                ln |= (buf[i] & 0x7f) << shift; shift += 7; i += 1
            ln |= (buf[i] & 0x7f) << shift; i += 1
            yield fnum, wt, buf[i:i+ln]; i += ln
        elif wt == 5:  # 32-bit
            yield fnum, wt, buf[i:i+4]; i += 4
        else:
            return

def extract_opus_ch0(relay_audio_frame_bytes):
    """relay_audio_frame is an AudioDataFrame; field 4 = spectral_coeffs_ch0."""
    for fnum, wt, val in _walk_fields(relay_audio_frame_bytes):
        if fnum == 4 and wt == 2:
            return bytes(val)
    return b""

def handler(_, data):
    b = bytes(data)
    if not b:
        counts["other"] += 1; return
    if b[0] == 0x1A: counts["stream"] += 1
    elif b[0] == 0x0A: counts["status"] += 1
    elif b[0] == 0x42:
        counts["audio"] += 1
        # b = RelayToAppMessage; field 8 (relay_audio_frame) is the only field
        for fnum, wt, val in _walk_fields(b):
            if fnum == 8 and wt == 2:
                ch0 = extract_opus_ch0(bytes(val))
                if ch0: audio_frames.append(ch0)
    else: counts["other"] += 1

async def phase(c, label, secs):
    base = dict(counts)
    for _ in range(secs):
        await asyncio.sleep(1)
    d = {k: counts[k]-base[k] for k in counts}
    print(f"[{label}] +{secs}s  stream={d['stream']} status={d['status']} audio={d['audio']} other={d['other']}", flush=True)

async def main():
    print("scanning for relay...", flush=True)
    dev = await BleakScanner.find_device_by_address(RELAY, timeout=25)
    if not dev: print("RELAY NOT FOUND"); return
    async with BleakClient(dev, timeout=30) as c:
        print("connected:", c.is_connected, flush=True)
        await asyncio.sleep(1)
        await c.start_notify(TX, handler)
        print("A: start MouthPad stream", flush=True)
        await c.write_gatt_char(RX, START_STREAM, response=False)
        await phase(c, "A stream-only", 5)
        print("B: enable relay mic", flush=True)
        await c.write_gatt_char(RX, MIC_ENABLE, response=False)
        await phase(c, "B mic+stream", 10)
        print("C: disable relay mic", flush=True)
        await c.write_gatt_char(RX, MIC_DISABLE, response=False)
        await phase(c, "C mic-off", 3)
        await c.stop_notify(TX)
    print("TOT:", counts, flush=True)
    if audio_frames:
        sizes = [len(f) for f in audio_frames]
        print(f"audio frames: {len(audio_frames)}, sizes min/max={min(sizes)}/{max(sizes)}", flush=True)
        with open("relay_audio.bin", "wb") as f:
            for fr in audio_frames:
                f.write(struct.pack("<H", len(fr))); f.write(fr)
        print("wrote relay_audio.bin (u16-len-prefixed opus frames)", flush=True)

asyncio.run(main())

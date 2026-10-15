import asyncio
import colorsys
import io
import json
import socket
import threading
import time
import unicodedata

import numpy as np
import soundcard as sc

from PIL import Image, ImageOps
from websockets.asyncio.server import serve

from winrt.windows.media.control import (
    GlobalSystemMediaTransportControlsSessionManager as MediaManager
)

from winrt.windows.storage.streams import DataReader


# ============================================================
# NETWORK
# ============================================================

HOST = "0.0.0.0"
PORT = 8765

MEDIA_POLL = 0.5


# ============================================================
# ART
# ============================================================

ART_W = 72
ART_H = 72

MAX_THUMBNAIL = 8 * 1024 * 1024


# ============================================================
# VISUALIZER
# ============================================================

VIS_BANDS = 12

SAMPLE_RATE = 48000
FFT_SIZE = 2048
VIS_FPS = 20


BAND_EDGES = np.array([
    55,
    90,
    145,
    230,
    360,
    560,
    900,
    1400,
    2200,
    3400,
    5200,
    8000,
    14000,
], dtype=float)


# Lower frequencies get slightly more weight.
# Upper frequencies intentionally reduced.

BAND_GAIN = np.array([
    1.28,
    1.22,
    1.15,

    1.08,
    1.04,
    1.00,
    0.97,
    0.93,

    0.88,
    0.82,
    0.76,
    0.70,
], dtype=np.float32)


MIN_DB = -72.0
MAX_DB = -16.0


FFT_WINDOW = np.hanning(
    FFT_SIZE
).astype(np.float32)


FFT_FREQ = np.fft.rfftfreq(
    FFT_SIZE,
    1.0 / SAMPLE_RATE
)


# ============================================================
# STATE
# ============================================================

clients = set()

send_lock = None

last_metadata = None
last_art = None
last_progress = None

latest_vis = bytes(
    [0] * VIS_BANDS
)


# ============================================================
# IP
# ============================================================

def get_local_ip():

    sock = socket.socket(
        socket.AF_INET,
        socket.SOCK_DGRAM
    )

    try:

        sock.connect(
            ("8.8.8.8", 80)
        )

        return sock.getsockname()[0]

    except Exception:

        return socket.gethostbyname(
            socket.gethostname()
        )

    finally:

        sock.close()


# ============================================================
# TEXT CLEANING
# ============================================================

def clean_text(value, fallback=""):

    if value is None:
        return fallback

    text = str(value).strip()

    if not text:
        return fallback

    text = unicodedata.normalize(
        "NFKD",
        text
    )

    text = text.encode(
        "ascii",
        "ignore"
    ).decode("ascii")

    text = " ".join(
        text.split()
    )

    return text or fallback


# ============================================================
# WINDOWS STATE
# ============================================================

def playback_state(status):

    try:

        value = int(
            getattr(
                status,
                "value",
                status
            )
        )

        return {
            0: "stopped",
            1: "stopped",
            2: "changing",
            3: "stopped",
            4: "playing",
            5: "paused",
        }.get(value, "unknown")

    except Exception:

        text = str(status).lower()

        if "playing" in text:
            return "playing"

        if "paused" in text:
            return "paused"

        if "stopped" in text:
            return "stopped"

        return "unknown"


def timespan_ms(value):

    try:

        return int(
            value.total_seconds()
            * 1000
        )

    except Exception:

        return 0


# ============================================================
# THUMBNAIL
# ============================================================

async def read_thumbnail(properties):

    thumb = getattr(
        properties,
        "thumbnail",
        None
    )

    if thumb is None:
        return None


    stream = None
    reader = None

    try:

        stream = await thumb.open_read_async()

        size = int(stream.size)

        if size <= 0 or size > MAX_THUMBNAIL:
            return None


        input_stream = stream.get_input_stream_at(0)

        reader = DataReader(
            input_stream
        )


        loaded = int(
            await reader.load_async(size)
        )


        if loaded <= 0:
            return None


        data = bytearray(
            loaded
        )

        reader.read_bytes(
            data
        )


        return bytes(data)


    except Exception as error:

        print(
            "[ART ERROR]",
            repr(error)
        )

        return None


    finally:

        if reader:

            try:
                reader.close()
            except Exception:
                pass


        if stream:

            try:
                stream.close()
            except Exception:
                pass


# ============================================================
# THEME COLORS
# ============================================================

def improve_color(rgb):

    r, g, b = rgb

    h, s, v = colorsys.rgb_to_hsv(
        r / 255,
        g / 255,
        b / 255
    )


    s = max(
        0.35,
        min(s, 0.88)
    )

    v = max(
        0.70,
        min(v, 0.95)
    )


    r, g, b = colorsys.hsv_to_rgb(
        h,
        s,
        v
    )


    return (
        int(r * 255),
        int(g * 255),
        int(b * 255)
    )


def color_distance(a, b):

    return np.sqrt(
        (a[0] - b[0]) ** 2
        +
        (a[1] - b[1]) ** 2
        +
        (a[2] - b[2]) ** 2
    )


def get_theme_colors(image):

    tiny = image.resize(
        (40, 40),
        Image.Resampling.BILINEAR
    )


    quantized = tiny.quantize(
        colors=12,
        method=Image.Quantize.MEDIANCUT
    )


    palette = quantized.getpalette()

    colors = quantized.getcolors() or []


    candidates = []


    for count, index in colors:

        r = palette[index * 3]
        g = palette[index * 3 + 1]
        b = palette[index * 3 + 2]


        h, s, v = colorsys.rgb_to_hsv(
            r / 255,
            g / 255,
            b / 255
        )


        # Ignore nearly-black pixels.

        if v < 0.13:
            continue


        score = (
            np.sqrt(count)
            *
            (0.25 + s)
            *
            (0.45 + v)
        )


        candidates.append(
            (
                score,
                (r, g, b)
            )
        )


    candidates.sort(
        reverse=True,
        key=lambda x: x[0]
    )


    if not candidates:

        return (
            (190, 190, 205),
            (110, 130, 180)
        )


    primary = improve_color(
        candidates[0][1]
    )


    secondary = None
    best_score = -1


    for score, rgb in candidates[1:]:

        candidate = improve_color(rgb)

        distance = color_distance(
            primary,
            candidate
        )


        # Prefer a genuinely different second color.

        combined = (
            score
            *
            (1 + distance / 120)
        )


        if (
            distance > 45
            and
            combined > best_score
        ):

            secondary = candidate
            best_score = combined


    if secondary is None:

        r, g, b = primary

        h, s, v = colorsys.rgb_to_hsv(
            r / 255,
            g / 255,
            b / 255
        )

        h = (h + 0.10) % 1.0

        r, g, b = colorsys.hsv_to_rgb(
            h,
            s * 0.90,
            v
        )

        secondary = (
            int(r * 255),
            int(g * 255),
            int(b * 255)
        )


    return primary, secondary


# ============================================================
# ART -> RGB565
# ============================================================

def convert_art(raw):

    try:

        with Image.open(
            io.BytesIO(raw)
        ) as source:


            image = source.convert("RGB")


            image = ImageOps.fit(
                image,

                (ART_W, ART_H),

                method=Image.Resampling.LANCZOS,

                centering=(0.5, 0.5)
            )


            theme1, theme2 = (
                get_theme_colors(image)
            )


            rgb565 = bytearray(
                ART_W * ART_H * 2
            )


            pos = 0


            for r, g, b in image.getdata():

                value = (
                    ((r & 0xF8) << 8)
                    |
                    ((g & 0xFC) << 3)
                    |
                    (b >> 3)
                )


                rgb565[pos] = (
                    value & 0xFF
                )

                rgb565[pos + 1] = (
                    value >> 8
                ) & 0xFF


                pos += 2


            payload = (
                b"ART1"
                +
                bytes([
                    ART_W,
                    ART_H
                ])
                +
                rgb565
            )


            return (
                payload,
                theme1,
                theme2
            )


    except Exception as error:

        print(
            "[ART DECODE]",
            repr(error)
        )

        return (
            None,
            None,
            None
        )


# ============================================================
# FFT RAW BANDS
# ============================================================

def fft_bands(audio):

    audio = np.asarray(
        audio,
        dtype=np.float32
    )


    if audio.size == 0:

        return np.zeros(
            VIS_BANDS,
            dtype=np.float32
        )


    if audio.ndim == 2:

        mono = np.mean(
            audio,
            axis=1
        )

    else:

        mono = audio


    if len(mono) < FFT_SIZE:

        mono = np.pad(
            mono,
            (
                0,
                FFT_SIZE - len(mono)
            )
        )

    else:

        mono = mono[-FFT_SIZE:]


    # Silence check.

    rms = np.sqrt(
        np.mean(
            mono * mono
        )
    )


    if rms < 0.00003:

        return np.zeros(
            VIS_BANDS,
            dtype=np.float32
        )


    mono -= np.mean(mono)


    spectrum = np.abs(
        np.fft.rfft(
            mono * FFT_WINDOW
        )
    )


    spectrum *= (
        2.0
        /
        np.sum(FFT_WINDOW)
    )


    result = np.zeros(
        VIS_BANDS,
        dtype=np.float32
    )


    for i in range(VIS_BANDS):

        mask = (
            (FFT_FREQ >= BAND_EDGES[i])
            &
            (FFT_FREQ < BAND_EDGES[i + 1])
        )


        values = spectrum[mask]


        if values.size == 0:

            db = MIN_DB

        else:

            magnitude = np.sqrt(
                np.mean(
                    values * values
                )
            )


            db = (
                20
                *
                np.log10(
                    magnitude + 1e-9
                )
            )


        value = (
            (db - MIN_DB)
            /
            (MAX_DB - MIN_DB)
        )


        result[i] = np.clip(
            value,
            0,
            1
        )


    return result


# ============================================================
# AUDIO THREAD
# ============================================================

def audio_worker():

    global latest_vis


    print()
    print("[FFT] Starting WASAPI loopback...")


    while True:

        try:

            speaker = sc.default_speaker()


            print(
                "[FFT] Output:",
                speaker.name
            )


            loopback = sc.get_microphone(
                id=speaker.name,
                include_loopback=True
            )


            smoothed = np.zeros(
                VIS_BANDS,
                dtype=np.float32
            )


            adaptive_gain = 1.0
            bass_average = 0.0


            with loopback.recorder(
                samplerate=SAMPLE_RATE,
                blocksize=FFT_SIZE
            ) as recorder:


                print(
                    "[FFT] Visualizer running"
                )


                while True:

                    audio = recorder.record(
                        numframes=FFT_SIZE
                    )


                    raw = fft_bands(
                        audio
                    )


                    # ------------------------------------------
                    # ADAPTIVE SENSITIVITY
                    # ------------------------------------------

                    frame_level = float(
                        np.percentile(
                            raw,
                            75
                        )
                    )


                    target_gain = (
                        0.62
                        /
                        max(
                            frame_level,
                            0.12
                        )
                    )


                    target_gain = np.clip(
                        target_gain,
                        0.70,
                        1.75
                    )


                    adaptive_gain = (
                        adaptive_gain * 0.94
                        +
                        target_gain * 0.06
                    )


                    levels = (
                        raw
                        *
                        adaptive_gain
                        *
                        BAND_GAIN
                    )


                    # ------------------------------------------
                    # BASS PUNCH
                    # ------------------------------------------

                    bass = float(
                        np.mean(
                            levels[:3]
                        )
                    )


                    bass_average = (
                        bass_average * 0.93
                        +
                        bass * 0.07
                    )


                    punch = max(
                        0.0,
                        bass - bass_average
                    )


                    levels[0] += (
                        punch * 0.45
                    )

                    levels[1] += (
                        punch * 0.30
                    )

                    levels[2] += (
                        punch * 0.16
                    )


                    levels = np.clip(
                        levels,
                        0,
                        1
                    )


                    # Small perceptual curve.

                    levels = (
                        levels ** 0.82
                    )


                    # ------------------------------------------
                    # SMOOTHING
                    # ------------------------------------------

                    for i in range(
                        VIS_BANDS
                    ):


                        if levels[i] > smoothed[i]:

                            factor = 0.68

                        else:

                            factor = 0.14


                        smoothed[i] += (
                            levels[i]
                            -
                            smoothed[i]
                        ) * factor


                    latest_vis = bytes(

                        np.clip(
                            smoothed * 255,
                            0,
                            255
                        ).astype(
                            np.uint8
                        )
                    )


        except Exception as error:

            print()
            print(
                "[FFT ERROR]",
                repr(error)
            )


            latest_vis = bytes(
                [0] * VIS_BANDS
            )


            time.sleep(2)


# ============================================================
# BROADCAST
# ============================================================

async def broadcast(data):

    if not clients:
        return


    dead = []


    async with send_lock:

        for client in list(
            clients
        ):

            try:

                await client.send(
                    data
                )

            except Exception:

                dead.append(
                    client
                )


    for client in dead:

        clients.discard(
            client
        )


# ============================================================
# ESP CLIENT
# ============================================================

async def websocket_handler(
    websocket
):

    clients.add(
        websocket
    )


    print(
        "[ESP32] Connected:",
        websocket.remote_address
    )


    try:

        if last_metadata:

            await websocket.send(
                last_metadata
            )


        if last_art:

            await websocket.send(
                last_art
            )


        if last_progress:

            await websocket.send(
                last_progress
            )


        async for message in websocket:

            if isinstance(
                message,
                str
            ):

                print(
                    "[ESP32 -> PC]",
                    message
                )


    finally:

        clients.discard(
            websocket
        )


# ============================================================
# VIS SEND LOOP
# ============================================================

async def visualizer_loop():

    delay = 1.0 / VIS_FPS


    while True:

        await broadcast(
            b"VIS1"
            +
            latest_vis
        )


        await asyncio.sleep(
            delay
        )


# ============================================================
# MEDIA LOOP
# ============================================================

async def media_loop():

    global last_metadata
    global last_art
    global last_progress


    manager = (
        await MediaManager.request_async()
    )


    print(
        "[MEDIA] Ready"
    )


    previous_track = None


    while True:

        try:

            session = (
                manager.get_current_session()
            )


            if session is None:

                await asyncio.sleep(
                    MEDIA_POLL
                )

                continue


            props = (
                await session
                .try_get_media_properties_async()
            )


            title = clean_text(
                getattr(
                    props,
                    "title",
                    ""
                ),
                "Unknown title"
            )


            artist = clean_text(
                getattr(
                    props,
                    "artist",
                    ""
                ),
                ""
            )


            if not artist:

                artist = clean_text(
                    getattr(
                        props,
                        "album_artist",
                        ""
                    ),
                    "Unknown artist"
                )


            album = clean_text(
                getattr(
                    props,
                    "album_title",
                    ""
                ),
                ""
            )


            playback = (
                session.get_playback_info()
            )


            state = (
                playback_state(
                    playback.playback_status
                )
                if playback
                else
                "unknown"
            )


            # -----------------------------------------------
            # PROGRESS
            # -----------------------------------------------

            try:

                timeline = (
                    session
                    .get_timeline_properties()
                )


                start = timespan_ms(
                    timeline.start_time
                )


                end = timespan_ms(
                    timeline.end_time
                )


                current = timespan_ms(
                    timeline.position
                )


                duration = max(
                    0,
                    end - start
                )


                position = max(
                    0,
                    current - start
                )


                if duration > 0:

                    position = min(
                        position,
                        duration
                    )


            except Exception:

                position = 0
                duration = 0


            track = (
                title,
                artist,
                album
            )


            # -----------------------------------------------
            # NEW TRACK
            # -----------------------------------------------

            if track != previous_track:

                previous_track = track


                print()
                print(
                    "TRACK:",
                    title
                )

                print(
                    "ARTIST:",
                    artist
                )


                raw = (
                    await read_thumbnail(
                        props
                    )
                )


                theme1 = (
                    190,
                    190,
                    205
                )

                theme2 = (
                    110,
                    130,
                    180
                )

                art = None


                if raw:

                    (
                        art,
                        theme1,
                        theme2
                    ) = convert_art(
                        raw
                    )


                last_art = art


                c1 = (
                    f"{theme1[0]:02X}"
                    f"{theme1[1]:02X}"
                    f"{theme1[2]:02X}"
                )


                c2 = (
                    f"{theme2[0]:02X}"
                    f"{theme2[1]:02X}"
                    f"{theme2[2]:02X}"
                )


                metadata = {

                    "type":
                        "media",

                    "title":
                        title,

                    "artist":
                        artist,

                    "state":
                        state,

                    "theme1":
                        c1,

                    "theme2":
                        c2,
                }


                last_metadata = (
                    json.dumps(
                        metadata,
                        separators=(
                            ",",
                            ":"
                        )
                    )
                )


                # Metadata goes first.
                # ESP stores it as pending data.

                await broadcast(
                    last_metadata
                )


                if art:

                    # Artwork arrival triggers cube animation.

                    await broadcast(
                        art
                    )

                else:

                    await broadcast(
                        '{"type":"art_clear"}'
                    )


            # -----------------------------------------------
            # PROGRESS
            # -----------------------------------------------

            progress = {

                "type":
                    "progress",

                "position":
                    position,

                "duration":
                    duration,

                "state":
                    state,
            }


            last_progress = (
                json.dumps(
                    progress,
                    separators=(
                        ",",
                        ":"
                    )
                )
            )


            await broadcast(
                last_progress
            )


        except Exception as error:

            print(
                "[MEDIA ERROR]",
                repr(error)
            )


        await asyncio.sleep(
            MEDIA_POLL
        )


# ============================================================
# MAIN
# ============================================================

async def main():

    global send_lock

    send_lock = asyncio.Lock()


    ip = get_local_ip()


    print()
    print(
        "======================================="
    )

    print(
        " ESP32 MUSIC COMPANION V4"
    )

    print(
        " Premium UI + Cube Transition"
    )

    print(
        "======================================="
    )

    print(
        "PC IP:",
        ip
    )


    threading.Thread(
        target=audio_worker,
        daemon=True
    ).start()


    async with serve(
        websocket_handler,
        HOST,
        PORT,
        ping_interval=20,
        ping_timeout=20,
        max_size=2 * 1024 * 1024,
    ) as server:


        await asyncio.gather(
            media_loop(),
            visualizer_loop(),
            server.serve_forever(),
        )


if __name__ == "__main__":

    try:

        asyncio.run(
            main()
        )

    except KeyboardInterrupt:

        print("Stopped.")
#include <WiFi.h>
#include <SPI.h>
#include <math.h>

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Fonts/FreeSansBold9pt7b.h>

#include <ArduinoJson.h>
#include <ArduinoWebsockets.h>

using namespace websockets;


// ============================================================
// TFT - EXISTING PINS
// ============================================================

#define TFT_SCK   18
#define TFT_MOSI  23
#define TFT_MISO  19
#define TFT_CS    5
#define TFT_DC    2
#define TFT_RST   4


// ============================================================
// SD - EXISTING PINS
// NOT USED IN THIS VERSION
// ============================================================

#define SD_SCK    25
#define SD_MISO   26
#define SD_MOSI   27
#define SD_CS     14


// ============================================================
// WIFI
// ============================================================

const char* WIFI_SSID = "Airtel_Air6969";
const char* WIFI_PASSWORD = "Chirag@69";


// ============================================================
// PC
// ============================================================

const char* PC_IP =
    "192.168.1.11";

const uint16_t WS_PORT =
    8765;

const char* WS_PATH =
    "/";


// ============================================================
// TFT
// ============================================================

Adafruit_ST7735 tft(
    TFT_CS,
    TFT_DC,
    TFT_RST
);


// ============================================================
// SCREEN
// ============================================================

#define SCREEN_W 128
#define SCREEN_H 160


// ============================================================
// ALBUM ART
// ============================================================

#define ART_W 72
#define ART_H 72

#define ART_X 28
#define ART_Y 3

#define ART_PIXELS (ART_W * ART_H)


// Region around art including glow

#define ART_REGION_X 22
#define ART_REGION_Y 0

#define ART_REGION_W 84
#define ART_REGION_H 78


// ============================================================
// TEXT / PROGRESS / VIS REGIONS
// ============================================================

#define TITLE_Y 78
#define TITLE_H 30

#define PROGRESS_Y 108
#define PROGRESS_H 24

#define VIS_Y 132
#define VIS_H_REGION 28


// ============================================================
// OFF-SCREEN FRAMEBUFFERS
// ============================================================
//
// Draw everything into RAM first,
// then push whole region to TFT.
//
// This greatly reduces visible painting/flickering.
// ============================================================

GFXcanvas16 artCanvas(
    ART_REGION_W,
    ART_REGION_H
);

GFXcanvas16 titleCanvas(
    SCREEN_W,
    TITLE_H
);

GFXcanvas16 progressCanvas(
    SCREEN_W,
    PROGRESS_H
);

GFXcanvas16 visCanvas(
    SCREEN_W,
    VIS_H_REGION
);


// ============================================================
// WEBSOCKET
// ============================================================

WebsocketsClient wsClient;

bool wsConnected = false;

unsigned long lastWsAttempt = 0;

const unsigned long WS_RECONNECT =
    3000;


// ============================================================
// COLORS
// ============================================================

#define C_BLACK 0x0000
#define C_WHITE 0xFFFF
#define C_MUTED 0x9CF3


uint16_t theme1 =
    0xBDF7;

uint16_t theme2 =
    0x741F;


// ============================================================
// ART BUFFERS
// ============================================================

uint16_t albumArt[
    ART_PIXELS
];

uint16_t previousArt[
    ART_PIXELS
];


bool artValid =
    false;

bool previousArtValid =
    false;

bool firstTrack =
    true;


// ============================================================
// CURRENT MEDIA
// ============================================================

String songTitle =
    "Waiting for music";

String songArtist =
    "";

String playState =
    "stopped";


// ============================================================
// PENDING NEW TRACK
// ============================================================

String pendingTitle =
    "";

String pendingArtist =
    "";

String pendingState =
    "stopped";


uint16_t pendingTheme1 =
    0xBDF7;

uint16_t pendingTheme2 =
    0x741F;


bool pendingMedia =
    false;


// ============================================================
// PROGRESS
// ============================================================
//
// PC may send the same timeline position repeatedly.
//
// ESP therefore runs its own clock while playing,
// but performs hard sync when a real seek is detected.
// ============================================================

uint32_t basePositionMs =
    0;

uint32_t durationMs =
    0;


unsigned long basePositionClock =
    0;


uint32_t lastIncomingPosition =
    0;


bool progressHardSync =
    true;


float displayedPosition =
    0;


unsigned long lastProgressUpdate =
    0;


int lastProgressPixel =
    -1;


uint32_t lastDisplayedSecond =
    0xFFFFFFFF;

uint32_t lastDurationSecond =
    0xFFFFFFFF;


// ============================================================
// VISUALIZER
// ============================================================

#define VIS_BANDS 12

#define VIS_BASE_Y 27

#define BAR_W 5
#define BAR_GAP 4

#define BAR_START_X 12

#define MAX_BAR_H 25


float visTarget[
    VIS_BANDS
];

float visCurrent[
    VIS_BANDS
];

float visPeak[
    VIS_BANDS
];


uint8_t peakHold[
    VIS_BANDS
];


int lastRenderedBar[
    VIS_BANDS
];

int lastRenderedPeak[
    VIS_BANDS
];


unsigned long lastVisFrame =
    0;


const unsigned long VIS_FRAME_MS =
    40;


// ============================================================
// TITLE MARQUEE
// ============================================================
//
// Long titles:
//
// Song Title Song Title Song Title...
//       ← ← ← ← ←
//
// Always right -> left.
// No bouncing.
// ============================================================

float marqueePhase =
    0;


unsigned long lastMarqueeFrame =
    0;


unsigned long marqueeDelayUntil =
    0;


const int MARQUEE_GAP =
    32;


const unsigned long MARQUEE_FRAME_MS =
    45;


// ============================================================
// COLOR HELPERS
// ============================================================

void unpack565(
    uint16_t color,
    int &r,
    int &g,
    int &b
)
{
    r =
        ((color >> 11) & 0x1F)
        * 255 / 31;


    g =
        ((color >> 5) & 0x3F)
        * 255 / 63;


    b =
        (color & 0x1F)
        * 255 / 31;
}


// ------------------------------------------------------------

uint16_t scaleColor(
    uint16_t color,
    float scale
)
{
    int r;
    int g;
    int b;


    unpack565(
        color,
        r,
        g,
        b
    );


    r =
        constrain(
            (int)(r * scale),
            0,
            255
        );


    g =
        constrain(
            (int)(g * scale),
            0,
            255
        );


    b =
        constrain(
            (int)(b * scale),
            0,
            255
        );


    return tft.color565(
        r,
        g,
        b
    );
}


// ------------------------------------------------------------

uint16_t blendColor(
    uint16_t a,
    uint16_t b,
    float mix
)
{
    mix =
        constrain(
            mix,
            0.0f,
            1.0f
        );


    int ar;
    int ag;
    int ab;

    int br;
    int bg;
    int bb;


    unpack565(
        a,
        ar,
        ag,
        ab
    );


    unpack565(
        b,
        br,
        bg,
        bb
    );


    return tft.color565(

        ar + (br - ar) * mix,

        ag + (bg - ag) * mix,

        ab + (bb - ab) * mix
    );
}


// ------------------------------------------------------------

uint16_t parseColor(
    String hex
)
{
    if (
        hex.length() != 6
    )
    {
        return 0xBDF7;
    }


    uint32_t rgb =
        strtoul(
            hex.c_str(),
            nullptr,
            16
        );


    return tft.color565(

        (rgb >> 16) & 0xFF,

        (rgb >> 8) & 0xFF,

        rgb & 0xFF
    );
}


// ============================================================
// DYNAMIC BACKGROUND
// ============================================================

uint16_t backgroundColor(
    int y
)
{
    // Visualizer area stays pure black.

    if (
        y >= VIS_Y
    )
    {
        return C_BLACK;
    }


    // --------------------------------------------------------
    // ART AREA
    // theme1 -> theme2 + very subtle glow
    // --------------------------------------------------------

    if (
        y < 82
    )
    {
        float p =
            (float)y
            /
            82.0f;


        uint16_t mixed =
            blendColor(
                theme1,
                theme2,
                p
            );


        float glow =
            sinf(
                p * PI
            );


        float brightness =
            0.08f
            +
            glow
            *
            0.045f;


        return scaleColor(
            mixed,
            brightness
        );
    }


    // --------------------------------------------------------
    // TEXT AREA
    // fade theme2 toward black
    // --------------------------------------------------------

    float fade =
        1.0f
        -
        (
            (float)(y - 82)
            /
            50.0f
        );


    fade =
        constrain(
            fade,
            0.0f,
            1.0f
        );


    return scaleColor(
        theme2,
        0.07f * fade
    );
}


// ============================================================
// FILL CANVAS WITH BACKGROUND
// ============================================================

void fillCanvasBackground(
    GFXcanvas16 &canvas,
    int globalY
)
{
    for (
        int y = 0;
        y < canvas.height();
        y++
    )
    {
        canvas.drawFastHLine(

            0,
            y,

            canvas.width(),

            backgroundColor(
                globalY + y
            )
        );
    }
}


// ============================================================
// DRAW FULL BACKGROUND
// ============================================================

void drawBackground()
{
    for (
        int y = 0;
        y < VIS_Y;
        y++
    )
    {
        tft.drawFastHLine(
            0,
            y,
            SCREEN_W,
            backgroundColor(y)
        );
    }


    tft.fillRect(
        0,
        VIS_Y,
        SCREEN_W,
        SCREEN_H - VIS_Y,
        C_BLACK
    );
}


// ============================================================
// ROUND ART CORNER MASK TEST
// ============================================================

bool artPixelVisible(
    int x,
    int y
)
{
    const int radius =
        7;


    // TOP LEFT

    if (
        x < radius
        &&
        y < radius
    )
    {
        int dx =
            radius - 1 - x;

        int dy =
            radius - 1 - y;


        if (
            dx * dx
            +
            dy * dy
            >
            radius * radius
        )
        {
            return false;
        }
    }


    // TOP RIGHT

    if (
        x >= ART_W - radius
        &&
        y < radius
    )
    {
        int dx =
            x
            -
            (
                ART_W - radius
            );


        int dy =
            radius - 1 - y;


        if (
            dx * dx
            +
            dy * dy
            >
            radius * radius
        )
        {
            return false;
        }
    }


    // BOTTOM LEFT

    if (
        x < radius
        &&
        y >= ART_H - radius
    )
    {
        int dx =
            radius - 1 - x;


        int dy =
            y
            -
            (
                ART_H - radius
            );


        if (
            dx * dx
            +
            dy * dy
            >
            radius * radius
        )
        {
            return false;
        }
    }


    // BOTTOM RIGHT

    if (
        x >= ART_W - radius
        &&
        y >= ART_H - radius
    )
    {
        int dx =
            x
            -
            (
                ART_W - radius
            );


        int dy =
            y
            -
            (
                ART_H - radius
            );


        if (
            dx * dx
            +
            dy * dy
            >
            radius * radius
        )
        {
            return false;
        }
    }


    return true;
}


// ============================================================
// PREPARE ART CANVAS
// ============================================================

void prepareArtCanvas()
{
    fillCanvasBackground(
        artCanvas,
        ART_REGION_Y
    );


    uint16_t glow =
        blendColor(
            theme1,
            theme2,
            0.45f
        );


    // Outer glow.

    artCanvas.drawRoundRect(

        2,
        0,

        80,
        77,

        11,

        scaleColor(
            glow,
            0.16f
        )
    );


    // Inner glow.

    artCanvas.drawRoundRect(

        4,
        1,

        76,
        75,

        9,

        scaleColor(
            glow,
            0.25f
        )
    );


    // Tiny shadow.

    artCanvas.fillRoundRect(

        8,
        5,

        ART_W,
        ART_H,

        8,

        scaleColor(
            glow,
            0.035f
        )
    );
}


// ============================================================
// BUILD STATIC ART
// ============================================================

void buildStaticArtCanvas()
{
    prepareArtCanvas();


    const int localX =
        ART_X
        -
        ART_REGION_X;


    const int localY =
        ART_Y;


    // ========================================================
    // REAL ART
    // ========================================================

    if (
        artValid
    )
    {
        for (
            int y = 0;
            y < ART_H;
            y++
        )
        {
            for (
                int x = 0;
                x < ART_W;
                x++
            )
            {
                if (
                    artPixelVisible(
                        x,
                        y
                    )
                )
                {
                    artCanvas.drawPixel(

                        localX + x,
                        localY + y,

                        albumArt[
                            y
                            *
                            ART_W
                            +
                            x
                        ]
                    );
                }
            }
        }


        artCanvas.drawRoundRect(

            localX,
            localY,

            ART_W,
            ART_H,

            8,

            scaleColor(

                blendColor(
                    theme1,
                    theme2,
                    0.5f
                ),

                0.38f
            )
        );
    }


    // ========================================================
    // NO ART PLACEHOLDER
    // ========================================================

    else
    {
        uint16_t icon =
            blendColor(
                theme1,
                theme2,
                0.5f
            );


        uint16_t panel =
            scaleColor(
                icon,
                0.13f
            );


        artCanvas.fillRoundRect(

            localX,
            localY,

            ART_W,
            ART_H,

            8,

            panel
        );


        // Music note.

        artCanvas.fillCircle(
            34,
            49,
            5,
            icon
        );


        artCanvas.fillCircle(
            51,
            44,
            5,
            icon
        );


        artCanvas.fillRect(
            38,
            25,
            3,
            25,
            icon
        );


        artCanvas.fillRect(
            55,
            21,
            3,
            24,
            icon
        );


        artCanvas.fillRect(
            41,
            21,
            17,
            4,
            icon
        );
    }
}


// ============================================================
// PUSH ART CANVAS
// ============================================================

void pushArtCanvas()
{
    tft.drawRGBBitmap(

        ART_REGION_X,
        ART_REGION_Y,

        artCanvas.getBuffer(),

        ART_REGION_W,
        ART_REGION_H
    );
}


// ============================================================
// TITLE WIDTH
// ============================================================

int getTitleWidth(
    String text
)
{
    titleCanvas.setFont(
        &FreeSansBold9pt7b
    );


    int16_t x1;
    int16_t y1;

    uint16_t w;
    uint16_t h;


    titleCanvas.getTextBounds(

        text,

        0,
        15,

        &x1,
        &y1,

        &w,
        &h
    );


    titleCanvas.setFont();


    return w;
}


// ============================================================
// DRAW TITLE TO CANVAS AT X
// ============================================================

void canvasTitleAt(
    String text,
    int x
)
{
    titleCanvas.setFont(
        &FreeSansBold9pt7b
    );


    titleCanvas.setTextColor(
        C_WHITE
    );


    titleCanvas.setTextWrap(
        false
    );


    titleCanvas.setCursor(
        x,
        14
    );


    titleCanvas.print(
        text
    );


    titleCanvas.setFont();
}


// ============================================================
// RENDER TITLE + ARTIST
// ============================================================

void renderTitleRegion()
{
    fillCanvasBackground(
        titleCanvas,
        TITLE_Y
    );


    int width =
        getTitleWidth(
            songTitle
        );


    // ========================================================
    // SHORT TITLE
    // ========================================================

    if (
        width <= 118
    )
    {
        titleCanvas.setFont(
            &FreeSansBold9pt7b
        );


        int16_t x1;
        int16_t y1;

        uint16_t w;
        uint16_t h;


        titleCanvas.getTextBounds(

            songTitle,

            0,
            14,

            &x1,
            &y1,

            &w,
            &h
        );


        int x =
            (
                SCREEN_W
                -
                (int)w
            )
            /
            2
            -
            x1;


        canvasTitleAt(
            songTitle,
            x
        );
    }


    // ========================================================
    // LONG TITLE
    // CONTINUOUS RIGHT -> LEFT
    // ========================================================

    else
    {
        int period =
            width
            +
            MARQUEE_GAP;


        while (
            marqueePhase >= period
        )
        {
            marqueePhase -=
                period;
        }


        /*
            IMPORTANT:

            phase increases:

            x = 5 - phase

            So title moves LEFT continuously.

            Second copy follows from the right.
        */


        int firstX =
            5
            -
            (int)marqueePhase;


        int secondX =
            firstX
            +
            period;


        canvasTitleAt(
            songTitle,
            firstX
        );


        canvasTitleAt(
            songTitle,
            secondX
        );
    }


    // ========================================================
    // ARTIST
    // ========================================================

    String artist =
        songArtist;


    if (
        artist.length() > 20
    )
    {
        artist =
            artist.substring(
                0,
                17
            )
            +
            "...";
    }


    titleCanvas.setFont();

    titleCanvas.setTextSize(
        1
    );


    titleCanvas.setTextColor(
        C_MUTED
    );


    int artistWidth =
        artist.length()
        *
        6;


    titleCanvas.setCursor(

        (
            SCREEN_W
            -
            artistWidth
        )
        /
        2,

        22
    );


    titleCanvas.print(
        artist
    );


    // ========================================================
    // PUSH WHOLE TEXT FRAME
    // ========================================================

    tft.drawRGBBitmap(

        0,
        TITLE_Y,

        titleCanvas.getBuffer(),

        SCREEN_W,
        TITLE_H
    );
}


// ============================================================
// RESET TITLE MARQUEE
// ============================================================

void resetMarquee()
{
    marqueePhase =
        0;


    marqueeDelayUntil =
        millis()
        +
        1100;


    lastMarqueeFrame =
        millis();
}


// ============================================================
// ANIMATE TITLE
// ============================================================

void animateMarquee()
{
    int width =
        getTitleWidth(
            songTitle
        );


    if (
        width <= 118
    )
    {
        return;
    }


    unsigned long now =
        millis();


    if (
        now <
        marqueeDelayUntil
    )
    {
        return;
    }


    if (
        now
        -
        lastMarqueeFrame
        <
        MARQUEE_FRAME_MS
    )
    {
        return;
    }


    lastMarqueeFrame =
        now;


    // Increasing phase moves text LEFT.

    marqueePhase +=
        1.0f;


    int period =
        width
        +
        MARQUEE_GAP;


    if (
        marqueePhase >= period
    )
    {
        marqueePhase -=
            period;
    }


    renderTitleRegion();
}


// ============================================================
// FORMAT TIME
// ============================================================

String formatTime(
    uint32_t ms
)
{
    uint32_t totalSeconds =
        ms
        /
        1000;


    uint32_t minutes =
        totalSeconds
        /
        60;


    uint32_t seconds =
        totalSeconds
        %
        60;


    String value =
        String(minutes)
        +
        ":";


    if (
        seconds < 10
    )
    {
        value +=
            "0";
    }


    value +=
        String(seconds);


    return value;
}


// ============================================================
// CURRENT LOCALLY-PREDICTED POSITION
// ============================================================

uint32_t predictedPosition()
{
    uint32_t value =
        basePositionMs;


    if (
        playState ==
        "playing"
    )
    {
        value +=
            millis()
            -
            basePositionClock;
    }


    if (
        durationMs > 0
    )
    {
        value =
            min(
                value,
                durationMs
            );
    }


    return value;
}


// ============================================================
// SYNC WINDOWS PROGRESS
// ============================================================
//
// Fixes BOTH:
//
// 1. stale Windows position while playing
// 2. fast-forward / rewind while still playing
//
// ============================================================

void syncProgress(
    uint32_t incomingPosition,
    uint32_t incomingDuration,
    String newState
)
{
    unsigned long now =
        millis();


    uint32_t localPosition =
        predictedPosition();


    bool stateChanged =
        newState
        !=
        playState;


    // ========================================================
    // UPDATE DURATION
    // ========================================================

    if (
        incomingDuration > 0
    )
    {
        durationMs =
            incomingDuration;
    }


    // ========================================================
    // FIRST SYNC / NEW SONG
    // ========================================================

    if (
        progressHardSync
    )
    {
        basePositionMs =
            incomingPosition;


        displayedPosition =
            incomingPosition;


        basePositionClock =
            now;


        lastIncomingPosition =
            incomingPosition;


        playState =
            newState;


        progressHardSync =
            false;


        // Force redraw.

        lastProgressPixel =
            -1;


        lastDisplayedSecond =
            0xFFFFFFFF;


        return;
    }


    // ========================================================
    // DIFFERENCES
    // ========================================================

    long localDifference =
        (long)incomingPosition
        -
        (long)localPosition;


    long windowsDifference =
        (long)incomingPosition
        -
        (long)lastIncomingPosition;


    // ========================================================
    // DETECT SEEK
    // ========================================================

    bool seekDetected =
        false;


    /*
        If Windows timeline itself jumps by >2.5 sec,
        user almost certainly sought forward/backward.
    */

    if (
        labs(
            windowsDifference
        )
        >
        2500
    )
    {
        seekDetected =
            true;
    }


    /*
        Also detect cases where Windows and ESP differ
        strongly AND Windows itself changed enough.
    */

    if (
        labs(
            localDifference
        )
        >
        4000

        &&

        labs(
            windowsDifference
        )
        >
        500
    )
    {
        seekDetected =
            true;
    }


    // ========================================================
    // SEEK
    // HARD SYNC IMMEDIATELY
    // ========================================================

    if (
        seekDetected
    )
    {
        Serial.print(
            "SEEK -> "
        );


        Serial.print(
            incomingPosition
            /
            1000
        );


        Serial.println(
            " sec"
        );


        basePositionMs =
            incomingPosition;


        displayedPosition =
            incomingPosition;


        basePositionClock =
            now;


        lastIncomingPosition =
            incomingPosition;


        playState =
            newState;


        // Force visual refresh.

        lastProgressPixel =
            -1;


        lastDisplayedSecond =
            0xFFFFFFFF;


        return;
    }


    // ========================================================
    // PLAYING
    // ========================================================

    if (
        newState ==
        "playing"
    )
    {
        // ----------------------------------------------------
        // RESUME
        // ----------------------------------------------------

        if (
            stateChanged
        )
        {
            /*
                If resume packet is reasonably close,
                trust Windows.

                Otherwise keep local value to avoid
                jumping backwards from stale timeline.
            */

            long resumeDifference =
                (long)incomingPosition
                -
                (long)localPosition;


            if (
                labs(
                    resumeDifference
                )
                <
                3000
            )
            {
                basePositionMs =
                    incomingPosition;
            }


            else
            {
                basePositionMs =
                    localPosition;
            }
        }


        // ----------------------------------------------------
        // WINDOWS ACTUALLY ADVANCED
        // ----------------------------------------------------

        else if (
            incomingPosition
            >
            lastIncomingPosition
            +
            100
        )
        {
            long drift =
                (long)incomingPosition
                -
                (long)localPosition;


            // Small drift -> gentle correction.

            if (
                labs(drift)
                <=
                2000
            )
            {
                long correction =
                    drift
                    *
                    15
                    /
                    100;


                long corrected =
                    (long)localPosition
                    +
                    correction;


                if (
                    corrected < 0
                )
                {
                    corrected = 0;
                }


                basePositionMs =
                    corrected;
            }


            // Bigger legitimate drift.

            else
            {
                basePositionMs =
                    incomingPosition;
            }
        }


        // ----------------------------------------------------
        // WINDOWS POSITION STALE
        // ----------------------------------------------------

        else
        {
            /*
                This is the important bit.

                Windows may repeatedly send:

                42.0
                42.0
                42.0

                ESP continues:

                42.5
                43.0
                43.5
            */

            basePositionMs =
                localPosition;
        }
    }


    // ========================================================
    // PAUSED / STOPPED
    // ========================================================

    else
    {
        /*
            While paused Windows is authoritative.

            Also makes paused seeking work instantly.
        */

        basePositionMs =
            incomingPosition;


        displayedPosition =
            incomingPosition;
    }


    // ========================================================
    // SAVE SYNC STATE
    // ========================================================

    basePositionClock =
        now;


    lastIncomingPosition =
        incomingPosition;


    playState =
        newState;
}


// ============================================================
// RENDER PROGRESS REGION
// ============================================================

void renderProgressRegion(
    uint32_t position
)
{
    fillCanvasBackground(
        progressCanvas,
        PROGRESS_Y
    );


    String left =
        formatTime(
            position
        );


    String right =
        formatTime(
            durationMs
        );


    progressCanvas.setFont();

    progressCanvas.setTextSize(
        1
    );


    progressCanvas.setTextColor(
        C_MUTED
    );


    // Current time.

    progressCanvas.setCursor(
        8,
        2
    );


    progressCanvas.print(
        left
    );


    // Duration.

    int rightX =
        120
        -
        right.length()
        *
        6;


    progressCanvas.setCursor(
        rightX,
        2
    );


    progressCanvas.print(
        right
    );


    // ========================================================
    // PROGRESS RAIL
    // ========================================================

    const int railX =
        8;

    const int railY =
        15;

    const int railWidth =
        112;


    uint16_t rail =
        scaleColor(

            blendColor(
                theme1,
                theme2,
                0.5f
            ),

            0.16f
        );


    progressCanvas.fillRoundRect(

        railX,
        railY,

        railWidth,
        2,

        1,

        rail
    );


    if (
        durationMs > 0
    )
    {
        float fraction =
            (float)position
            /
            (float)durationMs;


        fraction =
            constrain(
                fraction,
                0.0f,
                1.0f
            );


        int filled =
            railWidth
            *
            fraction;


        uint16_t progressColor =
            blendColor(
                theme1,
                theme2,
                fraction
            );


        if (
            filled > 0
        )
        {
            progressCanvas.fillRoundRect(

                railX,
                railY,

                filled,
                2,

                1,

                progressColor
            );
        }


        int dot =
            constrain(

                railX
                +
                filled,

                railX + 1,

                railX
                +
                railWidth
                -
                1
            );


        progressCanvas.fillCircle(

            dot,
            railY,

            2,

            C_WHITE
        );
    }


    // ========================================================
    // PUSH WHOLE REGION
    // ========================================================

    tft.drawRGBBitmap(

        0,
        PROGRESS_Y,

        progressCanvas.getBuffer(),

        SCREEN_W,
        PROGRESS_H
    );
}


// ============================================================
// ANIMATE PROGRESS
// ============================================================

void animateProgress()
{
    unsigned long now =
        millis();


    if (
        now
        -
        lastProgressUpdate
        <
        60
    )
    {
        return;
    }


    lastProgressUpdate =
        now;


    float target =
        predictedPosition();


    float difference =
        target
        -
        displayedPosition;


    // ========================================================
    // LARGE DIFFERENCE = SEEK
    // ========================================================

    if (
        fabsf(
            difference
        )
        >
        4000
    )
    {
        displayedPosition =
            target;
    }


    // ========================================================
    // NORMAL SMOOTH MOVEMENT
    // ========================================================

    else
    {
        displayedPosition +=

            difference

            *
            0.35f;
    }


    uint32_t position =
        displayedPosition;


    uint32_t second =
        position
        /
        1000;


    uint32_t durationSecond =
        durationMs
        /
        1000;


    int pixel =
        0;


    if (
        durationMs > 0
    )
    {
        pixel =
            (
                position
                *
                112ULL
            )
            /
            durationMs;
    }


    /*
        Don't redraw unless something visible changed.

        Much less TFT traffic.
    */

    if (
        pixel
        ==
        lastProgressPixel

        &&

        second
        ==
        lastDisplayedSecond

        &&

        durationSecond
        ==
        lastDurationSecond
    )
    {
        return;
    }


    lastProgressPixel =
        pixel;


    lastDisplayedSecond =
        second;


    lastDurationSecond =
        durationSecond;


    renderProgressRegion(
        position
    );
}


// ============================================================
// VISUALIZER CHANGE DETECTION
// ============================================================

bool visualizerNeedsRedraw()
{
    bool changed =
        false;


    for (
        int i = 0;
        i < VIS_BANDS;
        i++
    )
    {
        int height =
            constrain(
                (int)visCurrent[i],
                0,
                MAX_BAR_H
            );


        int peak =
            constrain(
                (int)visPeak[i],
                0,
                MAX_BAR_H
            );


        if (
            height
            !=
            lastRenderedBar[i]

            ||

            peak
            !=
            lastRenderedPeak[i]
        )
        {
            changed =
                true;


            lastRenderedBar[i] =
                height;


            lastRenderedPeak[i] =
                peak;
        }
    }


    return changed;
}


// ============================================================
// RENDER VISUALIZER
// ============================================================

void renderVisualizer()
{
    visCanvas.fillScreen(
        C_BLACK
    );


    for (
        int i = 0;
        i < VIS_BANDS;
        i++
    )
    {
        int x =
            BAR_START_X
            +
            i
            *
            (
                BAR_W
                +
                BAR_GAP
            );


        int height =
            constrain(
                (int)visCurrent[i],
                0,
                MAX_BAR_H
            );


        float colorPosition =
            (float)i
            /
            (
                VIS_BANDS - 1
            );


        uint16_t color =
            blendColor(
                theme1,
                theme2,
                colorPosition
            );


        // ====================================================
        // BAR
        // ====================================================

        if (
            height > 0
        )
        {
            int top =
                VIS_BASE_Y
                -
                height;


            // Dark body.

            visCanvas.fillRect(

                x,
                top,

                BAR_W,
                height,

                scaleColor(
                    color,
                    0.46f
                )
            );


            // Bright top.

            int bright =
                height
                *
                0.42f;


            if (
                bright >= 2
            )
            {
                visCanvas.fillRect(

                    x,
                    top,

                    BAR_W,
                    bright,

                    color
                );
            }
        }


        // ====================================================
        // PEAK
        // ====================================================

        int peak =
            constrain(
                (int)visPeak[i],
                1,
                MAX_BAR_H
            );


        int peakY =
            VIS_BASE_Y
            -
            peak;


        visCanvas.drawFastHLine(

            x + 1,
            peakY,

            BAR_W - 2,

            scaleColor(
                color,
                0.95f
            )
        );
    }


    // Push full visualizer frame at once.

    tft.drawRGBBitmap(

        0,
        VIS_Y,

        visCanvas.getBuffer(),

        SCREEN_W,
        VIS_H_REGION
    );
}


// ============================================================
// ANIMATE VISUALIZER
// ============================================================

void animateVisualizer()
{
    unsigned long now =
        millis();


    if (
        now
        -
        lastVisFrame
        <
        VIS_FRAME_MS
    )
    {
        return;
    }


    lastVisFrame =
        now;


    for (
        int i = 0;
        i < VIS_BANDS;
        i++
    )
    {
        float target =
            (
                playState
                ==
                "playing"
            )
            ?
            visTarget[i]
            :
            0;


        // ====================================================
        // FAST ATTACK
        // ====================================================

        if (
            target
            >
            visCurrent[i]
        )
        {
            visCurrent[i] +=

                (
                    target
                    -
                    visCurrent[i]
                )

                *
                0.67f;
        }


        // ====================================================
        // SLOW DECAY
        // ====================================================

        else
        {
            visCurrent[i] +=

                (
                    target
                    -
                    visCurrent[i]
                )

                *
                0.12f;
        }


        // ====================================================
        // PEAK HOLD
        // ====================================================

        if (
            visCurrent[i]
            >=
            visPeak[i]
        )
        {
            visPeak[i] =
                visCurrent[i];


            peakHold[i] =
                8;
        }


        else if (
            peakHold[i]
            >
            0
        )
        {
            peakHold[i]--;
        }


        else
        {
            visPeak[i] -=
                0.30f;


            if (
                visPeak[i]
                <
                1
            )
            {
                visPeak[i] =
                    1;
            }
        }
    }


    if (
        visualizerNeedsRedraw()
    )
    {
        renderVisualizer();
    }
}


// ============================================================
// RENDER COMPLETE UI
// ============================================================

void renderWholeUI()
{
    drawBackground();


    buildStaticArtCanvas();

    pushArtCanvas();


    renderTitleRegion();


    // Force progress refresh.

    lastProgressPixel =
        -1;


    lastDisplayedSecond =
        0xFFFFFFFF;


    lastDurationSecond =
        0xFFFFFFFF;


    renderProgressRegion(
        displayedPosition
    );


    // Force visualizer refresh.

    for (
        int i = 0;
        i < VIS_BANDS;
        i++
    )
    {
        lastRenderedBar[i] =
            -1;


        lastRenderedPeak[i] =
            -1;
    }


    renderVisualizer();
}


// ============================================================
// BUILD CUBE FRAME
// ============================================================

void buildCubeFrame(
    float p
)
{
    /*
        Cube animation redraws ONLY album region.

        Rest of screen remains untouched.
    */


    prepareArtCanvas();


    const int localX =
        ART_X
        -
        ART_REGION_X;


    const int localY =
        ART_Y;


    int oldWidth =
        ART_W
        *
        (
            1.0f
            -
            p
        );


    oldWidth =
        constrain(
            oldWidth,
            0,
            ART_W
        );


    int newWidth =
        ART_W
        -
        oldWidth;


    // ========================================================
    // OLD FACE
    // ========================================================

    if (
        previousArtValid
        &&
        oldWidth > 0
    )
    {
        for (
            int y = 0;
            y < ART_H;
            y++
        )
        {
            for (
                int dx = 0;
                dx < oldWidth;
                dx++
            )
            {
                int sourceX =
                    dx
                    *
                    ART_W
                    /
                    oldWidth;


                sourceX =
                    constrain(
                        sourceX,
                        0,
                        ART_W - 1
                    );


                uint16_t color =
                    previousArt[
                        y
                        *
                        ART_W
                        +
                        sourceX
                    ];


                color =
                    scaleColor(

                        color,

                        1.0f
                        -
                        p
                        *
                        0.38f
                    );


                artCanvas.drawPixel(

                    localX + dx,
                    localY + y,

                    color
                );
            }
        }
    }


    // ========================================================
    // NEW FACE
    // ========================================================

    if (
        newWidth > 0
    )
    {
        for (
            int y = 0;
            y < ART_H;
            y++
        )
        {
            for (
                int dx = 0;
                dx < newWidth;
                dx++
            )
            {
                int sourceX =
                    dx
                    *
                    ART_W
                    /
                    newWidth;


                sourceX =
                    constrain(
                        sourceX,
                        0,
                        ART_W - 1
                    );


                uint16_t color =
                    albumArt[
                        y
                        *
                        ART_W
                        +
                        sourceX
                    ];


                color =
                    scaleColor(

                        color,

                        0.62f
                        +
                        p
                        *
                        0.38f
                    );


                artCanvas.drawPixel(

                    localX
                    +
                    oldWidth
                    +
                    dx,

                    localY + y,

                    color
                );
            }
        }
    }


    // ========================================================
    // CUBE EDGE
    // ========================================================

    if (
        oldWidth > 0
        &&
        oldWidth < ART_W
    )
    {
        artCanvas.drawFastVLine(

            localX
            +
            oldWidth,

            localY + 2,

            ART_H - 4,

            scaleColor(
                C_WHITE,
                0.25f
            )
        );
    }
}


// ============================================================
// CUBE TRANSITION
// ============================================================

void cubeTransition()
{
    /*
        Only 9 frames.
        Only 84x78 region.

        Much faster than redrawing whole screen.
    */


    const int frames =
        9;


    for (
        int frame = 0;
        frame < frames;
        frame++
    )
    {
        float p =
            (float)frame
            /
            (
                frames - 1
            );


        // Smoothstep.

        p =
            p
            *
            p
            *
            (
                3.0f
                -
                2.0f
                *
                p
            );


        buildCubeFrame(
            p
        );


        pushArtCanvas();


        delay(
            7
        );
    }


    // ========================================================
    // NEW TRACK BECOMES CURRENT
    // ========================================================

    theme1 =
        pendingTheme1;


    theme2 =
        pendingTheme2;


    songTitle =
        pendingTitle;


    songArtist =
        pendingArtist;


    playState =
        pendingState;


    pendingMedia =
        false;


    progressHardSync =
        true;


    resetMarquee();


    renderWholeUI();
}


// ============================================================
// APPLY TRACK WITHOUT TRANSITION
// ============================================================

void applyPendingTrack()
{
    theme1 =
        pendingTheme1;


    theme2 =
        pendingTheme2;


    songTitle =
        pendingTitle;


    songArtist =
        pendingArtist;


    playState =
        pendingState;


    pendingMedia =
        false;


    firstTrack =
        false;


    progressHardSync =
        true;


    resetMarquee();


    renderWholeUI();
}


// ============================================================
// HANDLE JSON TEXT
// ============================================================

void handleText(
    String data
)
{

#if ARDUINOJSON_VERSION_MAJOR >= 7

    JsonDocument doc;

#else

    StaticJsonDocument<512> doc;

#endif


    DeserializationError error =
        deserializeJson(
            doc,
            data
        );


    if (
        error
    )
    {
        return;
    }


    String type =
        doc["type"]
        |
        "";


    // ========================================================
    // NEW MEDIA
    // ========================================================

    if (
        type ==
        "media"
    )
    {
        pendingTitle =
            doc["title"]
            |
            "Unknown title";


        pendingArtist =
            doc["artist"]
            |
            "";


        pendingState =
            doc["state"]
            |
            "unknown";


        pendingTheme1 =
            parseColor(

                doc["theme1"]
                |
                "BDBDCD"
            );


        pendingTheme2 =
            parseColor(

                doc["theme2"]
                |
                "6E82B4"
            );


        pendingMedia =
            true;


        // New song means next position is authoritative.

        progressHardSync =
            true;
    }


    // ========================================================
    // PROGRESS
    // ========================================================

    else if (
        type ==
        "progress"
    )
    {
        uint32_t incomingPosition =
            doc["position"]
            |
            0;


        uint32_t incomingDuration =
            doc["duration"]
            |
            0;


        String newState =
            doc["state"]
            |
            playState;


        if (
            pendingMedia
        )
        {
            pendingState =
                newState;
        }


        syncProgress(

            incomingPosition,

            incomingDuration,

            newState
        );
    }


    // ========================================================
    // NO ART
    // ========================================================

    else if (
        type ==
        "art_clear"
    )
    {
        artValid =
            false;


        if (
            pendingMedia
        )
        {
            applyPendingTrack();
        }
    }
}


// ============================================================
// HANDLE BINARY
// ============================================================

void handleBinary(
    WebsocketsMessage message
)
{
    uint32_t length =
        message.length();


    if (
        length < 4
    )
    {
        return;
    }


    const uint8_t* data =
        reinterpret_cast<
            const uint8_t*
        >(
            message.c_str()
        );


    // ========================================================
    // ART
    // ========================================================

    if (
        data[0] == 'A'
        &&
        data[1] == 'R'
        &&
        data[2] == 'T'
        &&
        data[3] == '1'
    )
    {
        const uint32_t expected =
            6
            +
            ART_PIXELS
            *
            2;


        if (
            length !=
            expected
        )
        {
            Serial.print(
                "Bad artwork size: "
            );


            Serial.println(
                length
            );


            return;
        }


        if (
            data[4] != ART_W
            ||
            data[5] != ART_H
        )
        {
            Serial.println(
                "Bad artwork dimensions"
            );


            return;
        }


        // ====================================================
        // SAVE OLD ART FOR CUBE
        // ====================================================

        if (
            artValid
        )
        {
            memcpy(

                previousArt,

                albumArt,

                sizeof(albumArt)
            );


            previousArtValid =
                true;
        }


        else
        {
            previousArtValid =
                false;
        }


        // ====================================================
        // READ NEW ART
        // ====================================================

        const uint8_t* pixels =
            data
            +
            6;


        for (
            int i = 0;
            i < ART_PIXELS;
            i++
        )
        {
            albumArt[i] =

                pixels[
                    i * 2
                ]

                |

                (
                    pixels[
                        i * 2 + 1
                    ]
                    <<
                    8
                );
        }


        artValid =
            true;


        // ====================================================
        // APPLY TRACK
        // ====================================================

        if (
            pendingMedia
        )
        {
            if (
                firstTrack
                ||
                !previousArtValid
            )
            {
                firstTrack =
                    false;


                applyPendingTrack();
            }


            else
            {
                cubeTransition();
            }
        }


        return;
    }


    // ========================================================
    // VISUALIZER
    // ========================================================

    if (
        data[0] == 'V'
        &&
        data[1] == 'I'
        &&
        data[2] == 'S'
        &&
        data[3] == '1'
    )
    {
        if (
            length
            !=
            VIS_BANDS
            +
            4
        )
        {
            return;
        }


        for (
            int i = 0;
            i < VIS_BANDS;
            i++
        )
        {
            float value =
                data[
                    i + 4
                ]
                /
                255.0f;


            visTarget[i] =
                value
                *
                MAX_BAR_H;
        }


        return;
    }
}


// ============================================================
// WEBSOCKET MESSAGE CALLBACK
// ============================================================

void onMessage(
    WebsocketsMessage message
)
{
    if (
        message.isText()
    )
    {
        handleText(
            message.data()
        );
    }


    else if (
        message.isBinary()
    )
    {
        handleBinary(
            message
        );
    }
}


// ============================================================
// WEBSOCKET EVENTS
// ============================================================

void onEvent(
    WebsocketsEvent event,
    String data
)
{
    if (
        event ==
        WebsocketsEvent::ConnectionOpened
    )
    {
        wsConnected =
            true;


        Serial.println(
            "WebSocket CONNECTED"
        );


        wsClient.send(
            "{\"type\":\"hello\",\"device\":\"music-v6\"}"
        );
    }


    else if (
        event ==
        WebsocketsEvent::ConnectionClosed
    )
    {
        wsConnected =
            false;


        Serial.println(
            "WebSocket DISCONNECTED"
        );


        // Fade visualizer to zero.

        for (
            int i = 0;
            i < VIS_BANDS;
            i++
        )
        {
            visTarget[i] =
                0;
        }
    }
}


// ============================================================
// WIFI
// ============================================================

void connectWiFi()
{
    WiFi.mode(
        WIFI_STA
    );


    WiFi.begin(
        WIFI_SSID,
        WIFI_PASSWORD
    );


    Serial.print(
        "WiFi"
    );


    while (
        WiFi.status()
        !=
        WL_CONNECTED
    )
    {
        Serial.print(
            "."
        );


        delay(
            500
        );
    }


    Serial.println();


    Serial.print(
        "ESP IP: "
    );


    Serial.println(
        WiFi.localIP()
    );
}


// ============================================================
// CONNECT WEBSOCKET
// ============================================================

void connectWebSocket()
{
    if (
        WiFi.status()
        !=
        WL_CONNECTED
    )
    {
        return;
    }


    Serial.print(
        "Connecting to PC: "
    );


    Serial.println(
        PC_IP
    );


    bool success =
        wsClient.connect(

            PC_IP,

            WS_PORT,

            WS_PATH
        );


    if (
        !success
    )
    {
        wsConnected =
            false;


        Serial.println(
            "WebSocket failed"
        );
    }
}


// ============================================================
// SETUP
// ============================================================

void setup()
{
    Serial.begin(
        115200
    );


    delay(
        400
    );


    // ========================================================
    // TFT SPI
    // ========================================================

    SPI.begin(

        TFT_SCK,

        TFT_MISO,

        TFT_MOSI,

        TFT_CS
    );


    tft.initR(
        INITR_BLACKTAB
    );


    // Portrait

    tft.setRotation(
        0
    );


    /*
        Faster screen pushes.

        If your Adafruit library doesn't support this
        function, simply remove this line.
    */

    tft.setSPISpeed(
        27000000
    );


    tft.fillScreen(
        C_BLACK
    );


    // ========================================================
    // VISUALIZER DEFAULTS
    // ========================================================

    for (
        int i = 0;
        i < VIS_BANDS;
        i++
    )
    {
        visTarget[i] =
            0;


        visCurrent[i] =
            0;


        visPeak[i] =
            1;


        peakHold[i] =
            0;


        lastRenderedBar[i] =
            -1;


        lastRenderedPeak[i] =
            -1;
    }


    basePositionClock =
        millis();


    resetMarquee();


    renderWholeUI();


    // ========================================================
    // WS CALLBACKS
    // ========================================================

    wsClient.onMessage(
        onMessage
    );


    wsClient.onEvent(
        onEvent
    );


    // ========================================================
    // NETWORK
    // ========================================================

    connectWiFi();


    connectWebSocket();
}


// ============================================================
// LOOP
// ============================================================

void loop()
{
    // ========================================================
    // WIFI RECOVERY
    // ========================================================

    if (
        WiFi.status()
        !=
        WL_CONNECTED
    )
    {
        wsConnected =
            false;


        WiFi.reconnect();


        delay(
            500
        );


        return;
    }


    // ========================================================
    // WEBSOCKET
    // ========================================================

    if (
        wsConnected
    )
    {
        wsClient.poll();
    }


    else if (
        millis()
        -
        lastWsAttempt
        >=
        WS_RECONNECT
    )
    {
        lastWsAttempt =
            millis();


        connectWebSocket();
    }


    // ========================================================
    // LOCAL UI ANIMATION
    // ========================================================

    animateProgress();

    animateVisualizer();

    animateMarquee();


    delay(
        1
    );
}
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <sched.h>
#include <sys/stat.h>

#include "OSC/OSCMessage.h"
#include "OSC/SimpleWriter.h"
#include "UdpSocket.h"
#include "OledScreen.h"
#include "MainMenu.h"
#include "Timer.h"
#include "AppData.h"
#include "Hardware.h"

static const unsigned int MAX_KNOBS = 6;
static int16_t knobs_[MAX_KNOBS];
static const int8_t EXPR_KNOB = 5;
int       previousScreen = -1;
int       encoderDownTime = -1;
const int SHUTDOWN_TIME = 4;


static int16_t pedalExprMin_ = 0;
static int16_t pedalExprMax_ = 1023;
enum PedalSwitchModes {
    PSM_PATCH = 0,
    PSM_FAVOURITES,
    PSM_MAX
};
static PedalSwitchModes pedalSwitchMode_ = PSM_PATCH;
int footswitchPos = 1; //normally closed

// dump of OSC message for sending 
SimpleWriter dump;

/*
sockets for communicating OSC with other programs we need 3:
    1) this program receives on 4001
    2) sends to Pd on 4000
    3) sends to Aux program (scripts in the System menu) on 4002
(the destinations are set below)
*/
UdpSocket udpSock(4001);
UdpSocket udpSockAux(4003);

// global app states, flags, screens, etc...
AppData app;

// main menu
MainMenu menu;

// hardware, keys, knobs, oled, etc...
Hardware interface;

// exit flag
int quit = 0;

void setEnv() {
    setenv("PATCH_DIR", app.getPatchDir().c_str(), 1);
    setenv("FW_DIR", app.getFirmwareDir().c_str(), 1);
    setenv("USER_DIR", app.getUserDir().c_str(), 1);
}


int execScript(const char* cmd) {
    char buf[128];
    sprintf(buf, "%s/scripts/%s", app.getFirmwareDir().c_str(), cmd);
    setEnv();
    return system(buf);
}


std::string getMainSystemFile(  const std::vector<std::string>& paths,
                            const std::string& filename) {
// look for file in set of paths, in preference order
    struct stat st;
    for (std::string path : paths) {
        std::string fp = path + "/" + filename;
        if (stat(fp.c_str(), &st) == 0) {
            return fp;
        }
    }

    // none found return empty string
    return "";
}

/** OSC messages received internally (from PD or other program) **/

// ui messages
void setScreen(OSCMessage &msg);
void vuMeter(OSCMessage &msg);
void setLED(OSCMessage &msg);
void flashLED(OSCMessage &msg);
void screenShot(OSCMessage &msg);
void enablePatchSubMenu(OSCMessage &msg);
void enableAuxSubMenu(OSCMessage &msg);
void goHome(OSCMessage &msg);

// new style graphics messages
void gShowInfoBar(OSCMessage &msg); // turns the vu meter on / off
void gClear(OSCMessage &msg);
void gSetPixel(OSCMessage &msg);
void gFillArea(OSCMessage &msg);
void gCircle(OSCMessage &msg);
void gFilledCircle(OSCMessage &msg);
void gLine(OSCMessage &msg);
void gBox(OSCMessage &msg);
void gInvert(OSCMessage &msg);
void gInvertArea(OSCMessage &msg);
void gCharacter(OSCMessage &msg);
void gPrintln(OSCMessage &msg);
void gWaveform(OSCMessage &msg);
void gFlip(OSCMessage &msg);


// older legacy messages for screen

// messages for patch screen
void setPatchScreenLine1(OSCMessage &msg);
void setPatchScreenLine2(OSCMessage &msg);
void setPatchScreenLine3(OSCMessage &msg);
void setPatchScreenLine4(OSCMessage &msg);
void setPatchScreenLine5(OSCMessage &msg);
void invertScreenLine(OSCMessage &msg);
// messages for aux screen
void setAuxScreenLine0(OSCMessage &msg);
void setAuxScreenLine1(OSCMessage &msg);
void setAuxScreenLine2(OSCMessage &msg);
void setAuxScreenLine3(OSCMessage &msg);
void setAuxScreenLine4(OSCMessage &msg);
void setAuxScreenLine5(OSCMessage &msg);
void invertAuxScreenLine(OSCMessage &msg);
void auxScreenClear(OSCMessage &msg);


// system message
void loadPatch(OSCMessage &msg);
void midiConfig(OSCMessage &msg);
void pedalConfig(OSCMessage &msg);
void patchLoaded(OSCMessage &msg);
void reload(OSCMessage &msg);
void sendShutdown(OSCMessage &msg);
void quitMother(OSCMessage &msg);
void programChange(OSCMessage &msg);

void pedalExprMin(OSCMessage &msg);
void pedalExprMax(OSCMessage &msg);
void pedalSwitchMode(OSCMessage &msg);

/* end internal OSC messages received */

/* OSC messages received from MCU (we only use ecncoder input, and smooth knobs,  key messages get passed righ to PD or other program */
void encoderInput(void);
void encoderButton(void);
void knobsInput(uint32_t * adc);
void footswitchInput(OSCMessage &msg);
/* end OSC messages received from MCU */

/* helpers */
void updateScreenPage(uint8_t page, OledScreen &screen);
void setScreenLine(OledScreen &screen, int lineNum, OSCMessage &msg);
void sendLed(unsigned c);
void patchLoaded(bool);
/* end helpers */

int main(int argc, char* argv[]) {
    printf("build date " __DATE__ "   " __TIME__ "/n");
    uint32_t seconds = 0;
    char udpPacketIn[256];
    //uint8_t osc_packet_in[256];
    uint8_t i = 0;
    int len = 0;
    int page = 0;

    Timer screenFpsTimer, screenLineTimer, knobPollTimer, pingTimer, upTime;

    screenFpsTimer.reset();
    knobPollTimer.reset();
    screenLineTimer.reset();
    pingTimer.reset();
    upTime.reset();

    // init hardware
    interface.hardwareInit();

    app.oled(AppData::MENU).showInfoBar = false;
    app.oled(AppData::AUX).showInfoBar = false;
    app.oled(AppData::PATCH).showInfoBar = true;

    // set locale so sorting happens in right order
    //std::setlocale(LC_ALL, "en_US.UTF-8");

    // for setting real time scheduling
    /*struct sched_param par;

    par.sched_priority = 10;
    printf("settin priority to: %d\n", 10);
    if (sched_setscheduler(0,SCHED_FIFO,&par) < 0){
        printf("failed to set rt scheduling\n");
    }*/

    udpSock.setDestination(4000, "localhost");
    udpSockAux.setDestination(4002, "localhost"); // for sending encoder to aux program

    OSCMessage msgIn;

    menu.buildMenu();

    OSCMessage dummy("/pedalConfig");
    pedalConfig(dummy);
    quit = 0;

    // full udp -> serial -> serial -> udp
    for (;;) {
        // receive udp, send to serial
        len = udpSock.readBuffer(udpPacketIn, 256, 0);
        if (len > 0) {
            msgIn.empty();
            for (i = 0; i < len; i++) {
                msgIn.fill(udpPacketIn[i]);
            }
            if (!msgIn.hasError()) {
                    //char buf[128];
                    //msgIn.getAddress(buf,0,128);
                    //printf("osc message received %s %i\n",buf,msgIn.size());
                // or'ing will do lazy eval, i.e. as soon as one succeeds it will stop
                bool processed =
                    msgIn.dispatch("/oled/vumeter", vuMeter, 0)
                    || msgIn.dispatch("/oled/gShowInfoBar", gShowInfoBar, 0)
                    || msgIn.dispatch("/oled/gClear", gClear, 0)
                    || msgIn.dispatch("/oled/gSetPixel", gSetPixel, 0)
                    || msgIn.dispatch("/oled/gFillArea", gFillArea, 0)
                    || msgIn.dispatch("/oled/gCircle", gCircle, 0)
                    || msgIn.dispatch("/oled/gFilledCircle", gFilledCircle, 0)
                    || msgIn.dispatch("/oled/gLine", gLine, 0)
                    || msgIn.dispatch("/oled/gBox", gBox, 0)
                    || msgIn.dispatch("/oled/gInvert", gInvert, 0)
                    || msgIn.dispatch("/oled/gCharacter", gCharacter, 0)
                    || msgIn.dispatch("/oled/gPrintln", gPrintln, 0)
                    || msgIn.dispatch("/oled/gWaveform", gWaveform, 0)
                    || msgIn.dispatch("/oled/gInvertArea", gInvertArea, 0)
                    || msgIn.dispatch("/oled/gFlip", gFlip, 0)

                    || msgIn.dispatch("/oled/line/1", setPatchScreenLine1, 0)
                    || msgIn.dispatch("/oled/line/2", setPatchScreenLine2, 0)
                    || msgIn.dispatch("/oled/line/3", setPatchScreenLine3, 0)
                    || msgIn.dispatch("/oled/line/4", setPatchScreenLine4, 0)
                    || msgIn.dispatch("/oled/line/5", setPatchScreenLine5, 0)
                    || msgIn.dispatch("/oled/invertline", invertScreenLine, 0)
                    || msgIn.dispatch("/oled/aux/line/1", setAuxScreenLine1, 0)
                    || msgIn.dispatch("/oled/aux/line/2", setAuxScreenLine2, 0)
                    || msgIn.dispatch("/oled/aux/line/3", setAuxScreenLine3, 0)
                    || msgIn.dispatch("/oled/aux/line/4", setAuxScreenLine4, 0)
                    || msgIn.dispatch("/oled/aux/line/5", setAuxScreenLine5, 0)
                    || msgIn.dispatch("/oled/aux/invertline", invertAuxScreenLine, 0)
                    || msgIn.dispatch("/oled/aux/clear", auxScreenClear, 0)

                    //|| msgIn.dispatch("/ready", sendReady, 0)
                    //|| msgIn.dispatch("/shutdown", sendShutdown, 0)
                    || msgIn.dispatch("/led", setLED, 0)
                    || msgIn.dispatch("/led/flash", flashLED, 0)
                    || msgIn.dispatch("/oled/setscreen", setScreen, 0)
                    || msgIn.dispatch("/reload", reload, 0)
                    || msgIn.dispatch("/quitmother", quitMother, 0)
                    || msgIn.dispatch("/screenshot", screenShot, 0)
                    || msgIn.dispatch("/pgmchg", programChange, 0)
                    || msgIn.dispatch("/gohome", goHome, 0)
                    || msgIn.dispatch("/enablepatchsub", enablePatchSubMenu, 0)
                    || msgIn.dispatch("/enableauxsub", enableAuxSubMenu, 0)
                    || msgIn.dispatch("/loadPatch", loadPatch, 0)
                    || msgIn.dispatch("/midiConfig", midiConfig, 0)
                    || msgIn.dispatch("/patchLoaded", patchLoaded, 0)
                    || msgIn.dispatch("/pedalConfig", pedalConfig, 0)

                    || msgIn.dispatch("/pedal/exprMin", pedalExprMin, 0)
                    || msgIn.dispatch("/pedal/exprMax", pedalExprMax, 0)
                    || msgIn.dispatch("/pedal/switchMode", pedalSwitchMode, 0)

                    ;
                if (!processed) {
                    char buf[128];
                    msgIn.getAddress(buf,0,128);
                    fprintf(stderr, "unrecognised osc message received %s %i\n",buf,msgIn.size());
                }
            }
            else {
                fprintf(stderr, "osc message has error \n ");
            }
            msgIn.empty();
        }

        // check hardware input, pass key presses along
        // knobs fix knobsInput
        // foot switch fix footswitchInput
        // keys send them along
        // encoder fix encoderInput and encoderButton
        interface.shiftRegRead();
        interface.checkEncoder();
        if (interface.encButFlag) encoderButton();
        if (interface.encTurnFlag) encoderInput();
        interface.getKeyStates();
        if (interface.keyStates != interface.keyStatesLast) {
            interface.keyStatesLast = interface.keyStates;
            for (int i = 0; i < 32; i++){
                printf("%d ", (interface.keyStates >> i) & 1);
            }
            printf("\n");
        }
       
        interface.clearFlags();
        
        // sleep for 2ms
        usleep(2000);

        if (app.currentScreen == AppData::AUX) {
            // we can do a whole screen,  but not faster than 20fps
            if (screenFpsTimer.getElapsed() > 50.f) {
                screenFpsTimer.reset();
                if (app.oled(AppData::AUX).newScreen) {
                    app.oled(AppData::AUX).newScreen = 0;
                    interface.oledWrite(app.oled(AppData::AUX).pix_buf); 
                }
            }
        }
        else if (app.currentScreen == AppData::MENU) {
            // we can do a whole screen,  but not faster than 20fps
            if (screenFpsTimer.getElapsed() > 50.f) {
                screenFpsTimer.reset();
                if (app.oled(AppData::MENU).newScreen) {
                    app.oled(AppData::MENU).newScreen = 0;
                    interface.oledWrite(app.oled(AppData::MENU).pix_buf);
                }

                // don't timeout to patch screen, whilst holding down encoder
                if (encoderDownTime == -1) {
                    // if there is a patch running while on menu screen, switch back to patch screen after the timeout
                    if (app.isPatchRunning() || app.isPatchLoading()) {
                        if (app.menuScreenTimeout > 0) app.menuScreenTimeout -= 50;
                        else {
                            app.currentScreen = AppData::PATCH;
                            app.oled(AppData::PATCH).newScreen = 1;
                        }
                    }
                }
            }
        }
        else if (app.currentScreen == AppData::PATCH) {
            if (screenFpsTimer.getElapsed() > 50.f) {
                screenFpsTimer.reset();
                if (app.oled(AppData::PATCH).newScreen) {
                    app.oled(AppData::PATCH).newScreen = 0;
                    interface.oledWrite(app.oled(AppData::PATCH).pix_buf);
                }
            }
        }

        // every 1 second do (slwo) periodic tasks
        if (pingTimer.getElapsed() > 1000.f) {

            // check for shutdown shortcut
            /*if (encoderDownTime != -1) {
                encoderDownTime--;
                if (encoderDownTime == 1) {
                    app.oled(AppData::AUX).clear();
                    app.oled(AppData::AUX).setLine(2, "HOLD to shutdown");
                    app.oled(AppData::AUX).setLine(4, "release to abort");
                    app.oled(AppData::AUX).newScreen = 1;
                    previousScreen = app.currentScreen;
                    app.currentScreen = AppData::AUX;
                }
                else if (encoderDownTime == 0) {
                    fprintf(stderr, "shutting down.....\n");
                    app.oled(AppData::AUX).clear();
                    app.oled(AppData::AUX).setLine(3, "Shutting down");
                    app.oled(AppData::AUX).newScreen = 1;
                    menu.runShutdown(0, 0);
                }
            }*/

            // check for patch loading timeout
            if (app.hasPatchLoadingTimedOut(1000)) {
                fprintf(stderr, "timeout: Patch did not return patchLoaded , will assume its loaded\n");
                patchLoaded(true);
            }

        }

        // poll for knobs
        if (knobPollTimer.getElapsed() > 40.f) {
            knobPollTimer.reset();
            // check knobs
            interface.adcReadAll();
            knobsInput(interface.adcs);
             
            // service led flasher
            if(app.ledFlashCounter) {
                app.ledFlashCounter--;
                if (!app.ledFlashCounter) sendLed(app.ledColor);
            }
        }

        // check exit flag
        if (quit) {
            printf("quitting\n");
            return 0;
        }
    } // for;;
}

/** OSC messages received internally (from PD or other program) **/
// settin patch screen
void setPatchScreenLine1(OSCMessage &msg) {
    setScreenLine(app.oled(AppData::PATCH), 1, msg);
    app.oled(AppData::PATCH).newScreen = 1;
}
void setPatchScreenLine2(OSCMessage &msg) {
    setScreenLine(app.oled(AppData::PATCH), 2, msg);
    app.oled(AppData::PATCH).newScreen = 1;
}
void setPatchScreenLine3(OSCMessage &msg) {
    setScreenLine(app.oled(AppData::PATCH), 3, msg);
    app.oled(AppData::PATCH).newScreen = 1;
}
void setPatchScreenLine4(OSCMessage &msg) {
    setScreenLine(app.oled(AppData::PATCH), 4, msg);
    app.oled(AppData::PATCH).newScreen = 1;
}
void setPatchScreenLine5(OSCMessage &msg) {
    setScreenLine(app.oled(AppData::PATCH), 5, msg);
    app.oled(AppData::PATCH).newScreen = 1;
}

inline AppData::Screen gScreen(unsigned s) {
    return (s - 1) < AppData::SCREEN_MAX ? (AppData::Screen) (s - 1) : AppData::PATCH;
}

// graphics messages
// these clear any pending newScreen flag
// and require sending gFlip to cause screen update
void gShowInfoBar(OSCMessage &msg) {
    app.oled(gScreen(msg.getInt(0))).newScreen = 0;
    if (msg.isInt(0) && msg.isInt(1)) {
        app.oled(gScreen(msg.getInt(0))).showInfoBar = (msg.getInt(1));
    }
}
void gClear(OSCMessage &msg) {
    app.oled(gScreen(msg.getInt(0))).newScreen = 0;
    if (msg.isInt(0) && msg.isInt(1)) {
        if (msg.getInt(1) == 1) app.oled(gScreen(msg.getInt(0))).clear();
    }
}

void gInvert(OSCMessage &msg) {
    app.oled(gScreen(msg.getInt(0))).newScreen = 0;
    if (msg.isInt(0) && msg.isInt(1)) {
        if (msg.getInt(1) == 1) app.oled(gScreen(msg.getInt(0))).invert_screen();
    }
}

void gSetPixel(OSCMessage &msg) {
    app.oled(gScreen(msg.getInt(0))).newScreen = 0;
    if (msg.isInt(0) && msg.isInt(1) && msg.isInt(2) && msg.isInt(3) ) {
        app.oled(gScreen(msg.getInt(0))).put_pixel(msg.getInt(3), msg.getInt(1), msg.getInt(2));
    }
}
void gFillArea(OSCMessage &msg) {
    app.oled(gScreen(msg.getInt(0))).newScreen = 0;
    if (msg.isInt(0) && msg.isInt(1) && msg.isInt(2) && msg.isInt(3) && msg.isInt(4) && msg.isInt(5)) {
        app.oled(gScreen(msg.getInt(0))).fill_area(msg.getInt(1), msg.getInt(2), msg.getInt(3), msg.getInt(4), msg.getInt(5));
    }
}

void gInvertArea(OSCMessage &msg) {
    app.oled(gScreen(msg.getInt(0))).newScreen = 0;
    if (msg.isInt(0) && msg.isInt(1) && msg.isInt(2) && msg.isInt(3) && msg.isInt(4)) {
        app.oled(gScreen(msg.getInt(0))).invert_area(msg.getInt(1), msg.getInt(2), msg.getInt(3), msg.getInt(4));
    }
}

void gFlip(OSCMessage &msg) {
    if (msg.isInt(0)) {
        app.oled(gScreen(msg.getInt(0))).newScreen = 1;
    }
}

void gWaveform(OSCMessage &msg) {
    uint8_t tmp[132];
    int len = 0;
    int i;
    app.oled(gScreen(msg.getInt(0))).newScreen = 0;
    if (msg.isInt(0) && msg.isBlob(1)) {
        len = msg.getBlob(1, tmp, 132);
        // only if we got 128 values (len and tmp includes the 4 size bytes of blob)
        if (len == 132) {
            // draw 127 connected lines
            for (i = 1; i < 128; i++) {
                app.oled(gScreen(msg.getInt(0))).draw_line(i - 1, tmp[i + 3], i, tmp[i + 4], 1);
            }
        }
    }
}

void gCircle(OSCMessage &msg) {
    app.oled(gScreen(msg.getInt(0))).newScreen = 0;
    if (msg.isInt(0) && msg.isInt(1) && msg.isInt(2) && msg.isInt(3) && msg.isInt(4)) {
        app.oled(gScreen(msg.getInt(0))).draw_circle(msg.getInt(1), msg.getInt(2), msg.getInt(3), msg.getInt(4));
    }
}

void gFilledCircle(OSCMessage &msg) {
    app.oled(gScreen(msg.getInt(0))).newScreen = 0;
    if (msg.isInt(0) && msg.isInt(1) && msg.isInt(2) && msg.isInt(3) && msg.isInt(4)) {
        app.oled(gScreen(msg.getInt(0))).draw_filled_circle(msg.getInt(1), msg.getInt(2), msg.getInt(3), msg.getInt(4));
    }
}

void gLine(OSCMessage &msg) {
    app.oled(gScreen(msg.getInt(0))).newScreen = 0;
    if (msg.isInt(0) && msg.isInt(1) && msg.isInt(2) && msg.isInt(3) && msg.isInt(4) && msg.isInt(5)) {
        app.oled(gScreen(msg.getInt(0))).draw_line(msg.getInt(1), msg.getInt(2), msg.getInt(3), msg.getInt(4), msg.getInt(5));
    }
}

void gPrintln(OSCMessage &msg) {

    char str[256];
    char line[256];
    int i;
    int x, y, height, color, screenNum;
    if (msg.isInt(0) && msg.isInt(1) && msg.isInt(2) && msg.isInt(3) && msg.isInt(4)) {
        i = 0;
        screenNum = msg.getInt(i++);
        x = msg.getInt(i++);
        y = msg.getInt(i++);
        height = msg.getInt(i++);
        color = msg.getInt(i++);
        // since there are no strings in pd, the line message will be made of different types
        // cat the line together, then throw it up on the patch screen
        line[0] = 0;
        while(i < msg.size()) {
            if (msg.isString(i)) {
                msg.getString(i, str, 256);
                strcat(line, str);
                strcat(line, " ");
            }
            else if (msg.isFloat(i)) {
                sprintf(str, "%g ", msg.getFloat(i));
                strcat(line, str);
            }
            else if (msg.isInt(i)) {
                sprintf(str, "%d ", msg.getInt(i));
                strcat(line, str);
            }
            i++;
        }
        app.oled(gScreen(screenNum)).newScreen = 0;
        app.oled(gScreen(screenNum)).println(line, x, y, height, color);
    }
}

void gBox(OSCMessage &msg) {
    app.oled(gScreen(msg.getInt(0))).newScreen = 0;
    if (msg.isInt(0) && msg.isInt(1) && msg.isInt(2) && msg.isInt(3) && msg.isInt(4) && msg.isInt(5)) {
        app.oled(gScreen(msg.getInt(0))).draw_box(msg.getInt(1), msg.getInt(2), msg.getInt(3), msg.getInt(4), msg.getInt(5));
    }
}
void gCharacter(OSCMessage &msg) {
    int size = 8;
    app.oled(gScreen(msg.getInt(0))).newScreen = 0;
    if (msg.isInt(0) && msg.isInt(1) && msg.isInt(2) && msg.isInt(3) && msg.isInt(4) && msg.isInt(5)) {
        size = msg.getInt(5);
        if (size == 8) app.oled(gScreen(msg.getInt(0))).put_char_small(msg.getInt(1), msg.getInt(2), msg.getInt(3), msg.getInt(4));
        else if (size == 16) app.oled(gScreen(msg.getInt(0))).put_char_arial16(msg.getInt(1), msg.getInt(2), msg.getInt(3), msg.getInt(4));
        else if (size == 24) app.oled(gScreen(msg.getInt(0))).put_char_arial24(msg.getInt(1), msg.getInt(2), msg.getInt(3), msg.getInt(4));
        else if (size == 32) app.oled(gScreen(msg.getInt(0))).put_char_arial32(msg.getInt(1), msg.getInt(2), msg.getInt(3), msg.getInt(4));
    }
}

// setting aux screen
void setAuxScreenLine1(OSCMessage &msg) {
    setScreenLine(app.oled(AppData::AUX), 1, msg);
    app.oled(AppData::AUX).newScreen = 1;
}
void setAuxScreenLine2(OSCMessage &msg) {
    setScreenLine(app.oled(AppData::AUX), 2, msg);
    app.oled(AppData::AUX).newScreen = 1;
}
void setAuxScreenLine3(OSCMessage &msg) {
    setScreenLine(app.oled(AppData::AUX), 3, msg);
    app.oled(AppData::AUX).newScreen = 1;
}
void setAuxScreenLine4(OSCMessage &msg) {
    setScreenLine(app.oled(AppData::AUX), 4, msg);
    app.oled(AppData::AUX).newScreen = 1;
}
void setAuxScreenLine5(OSCMessage &msg) {
    setScreenLine(app.oled(AppData::AUX), 5, msg);
    app.oled(AppData::AUX).newScreen = 1;
}
void auxScreenClear(OSCMessage &msg) {
    app.oled(AppData::AUX).clear();
    app.oled(AppData::AUX).newScreen = 1;
}

void screenShot(OSCMessage &msg) {
    if (app.currentScreen == AppData::AUX)
        app.oled(AppData::AUX).saveSVG("/usbdrive/AuxScreen.svg");

    if (app.currentScreen == AppData::MENU)
        app.oled(AppData::MENU).saveSVG("/usbdrive/MenuScreen.svg");

    if (app.currentScreen == AppData::PATCH)
        app.oled(AppData::PATCH).saveSVG("/usbdrive/PatchScreen.svg");
}

void programChange(OSCMessage &msg) {
    if (msg.isInt(0)) menu.programChange(msg.getInt(0));
}

void quitMother(OSCMessage &msg) {
    quit = 1;
}

void setLED(OSCMessage &msg) {
    if (msg.isInt(0)) {
        app.ledColor = msg.getInt(0);
        msg.send(dump);
        //slip.sendMessage(dump.buffer, dump.length, serial);
    }
}

void flashLED(OSCMessage &msg) {
    if (msg.isInt(0)){
        app.ledFlashCounter = msg.getInt(0);
        sendLed(7);
    }
}

void vuMeter(OSCMessage &msg) {
    static int count;

    char line[1024];
    int len, i, outR, outL, inR, inL;

    if (msg.isInt(0)) inR = msg.getInt(0);
    if (msg.isInt(1)) inL = msg.getInt(1);
    if (msg.isInt(2)) outR = msg.getInt(2);
    if (msg.isInt(3)) outL = msg.getInt(3);

    if (app.oled((AppData::Screen) app.currentScreen).showInfoBar) {
        app.oled((AppData::Screen) app.currentScreen).drawInfoBar(inR, inL, outR, outL);
        app.oled((AppData::Screen) app.currentScreen).newScreen = 1;
    }
}

void setScreen(OSCMessage &msg) {
    if (msg.isInt(0)) app.currentScreen = msg.getInt(0) - 1;
    app.oled( (AppData::Screen) app.currentScreen).newScreen = 1;
}

void reload(OSCMessage &msg) {
    printf("received reload msg\n");
    menu.reload();
}

void loadPatch(OSCMessage &msg) {
    char patchName[256];
    int i = 0;

    // loop over the patches and jump to the correct one
    if (msg.isString(0)) {
        msg.getString(0, patchName, 256);
        menu.loadPatch(patchName);
    }
}

void patchConfig(void) {
    std::vector<std::string> paths;
    paths.push_back("/tmp/patch");
    paths.push_back(app.getSystemDir());
    paths.push_back(app.getUserDir());
    paths.push_back(app.getFirmwareDir()+"/scripts");

    //patch is loaded, tell patch some config details
    app.setPatchLoading(false);
    app.setPatchRunning(true);
    printf("send patch config\n");

    std::string postPatch = getMainSystemFile(paths,"patch_loaded.sh");
    if(postPatch.length()>0) 
    {
        printf("using config %s\n", postPatch.c_str());
        system(postPatch.c_str());
    } 
}

void pedalConfig(OSCMessage &msg) {
    std::vector<std::string> paths;
    paths.push_back("/tmp/patch");
    paths.push_back(app.getSystemDir());
    paths.push_back(app.getUserDir());
    paths.push_back(app.getFirmwareDir()+"/scripts");

    std::string pedalConfig = getMainSystemFile(paths,"pedal_cfg.sh");
    if(pedalConfig.length()>0) 
    {
        printf("using pedal config %s\n", pedalConfig.c_str());
        system(pedalConfig.c_str());
    } 
}


void pedalExprMin(OSCMessage &msg) {
    if (msg.isInt(0)) {
        pedalExprMin_ = std::max(std::min(msg.getInt(0),1023),0);
    }
}

void pedalExprMax(OSCMessage &msg) {
    if (msg.isInt(0)) {
        pedalExprMax_ = std::max(std::min(msg.getInt(0),1023),0);
    }
}

void pedalSwitchMode(OSCMessage &msg) {
    if (msg.isInt(0)) {
        int value = msg.getInt(0);
        if(value >= 0 && value < PSM_MAX) {
            pedalSwitchMode_ = static_cast<PedalSwitchModes>(value);
        }
    }
}

void midiConfig(OSCMessage &msg) {
    patchConfig();
}

void patchLoaded(bool b) {
    printf("patch loaded\n");
    OSCMessage dummy("/pedalConfig");
    pedalConfig(dummy);
    patchConfig();

    // send current knob positions
    OSCMessage msgOut("/knobs");
    for(unsigned i = 0; i < MAX_KNOBS;i++) {
        msgOut.add(knobs_[i]);
    }
    msgOut.send(dump);
    udpSock.writeBuffer(dump.buffer, dump.length);        
}

void patchLoaded(OSCMessage &msg) {
    patchLoaded(true);
}

void invertScreenLine(OSCMessage &msg) {
    if (msg.isInt(0)) {
        // + 1 for backwards compatibility
        int line = msg.getInt(0);
        //printf("inverting %d\n", line);
        app.oled(AppData::PATCH).invertLine((line % 5)+1);
        app.oled(AppData::PATCH).newScreen = 1;
    }
}

void invertAuxScreenLine(OSCMessage &msg) {
    if (msg.isInt(0)) {
        // + 1 for backwards compatibility
        int line = msg.getInt(0);
        //printf("inverting %d\n", line);
        app.oled(AppData::AUX).invertLine((line % 5)+1);
        app.oled(AppData::AUX).newScreen = 1;
    }
}

void goHome(OSCMessage &msg ) {
    printf("returning to main menu\n");
    app.currentScreen = AppData::MENU;
    app.oled(AppData::MENU).newScreen = 1;
    app.menuScreenTimeout = MENU_TIMEOUT;

}

void enablePatchSubMenu(OSCMessage &msg ) {
    int v = 1;
    if (msg.isInt(0)) { v = msg.getInt(0);}
    printf("enabling patch sub menu %d\n", v);
    app.setPatchScreenEncoderOverride(v);
}

void enableAuxSubMenu(OSCMessage &msg ) {
    int v = 1;
    if (msg.isInt(0)) { v = msg.getInt(0);}
    printf("enabling aux sub menu %d\n", v);
    app.setAuxScreenEncoderOverride(v);
}

/* end internal OSC messages received */

/* OSC messages received from MCU (we only use ecncoder input, the key and knob messages get passed righ to PD or other program */

// this is when the encoder gets turned
// in menu screen, just navigate the menu
// in patch screen, bounce back to menu, unless override is on
// in aux screen, same
void encoderInput(void) {
    // if encoder is turned, abort shutdown timer
    encoderDownTime = -1;
    if (previousScreen >= 0) {
        app.currentScreen = previousScreen;
        previousScreen = -1;
        app.oled((AppData::Screen) app.currentScreen).newScreen = 1;
    }

    if (app.currentScreen == AppData::MENU) {
        app.menuScreenTimeout = MENU_TIMEOUT;
        if (interface.encTurn == 1) menu.encoderUp();
        if (interface.encTurn == 0) menu.encoderDown();
    }
    // if in patch mode, send encoder, but only if the patch said it wants encoder access
    if (app.currentScreen == AppData::PATCH) {
        if (app.isPatchScreenEncoderOverride()) {
            OSCMessage msgOut("/encoder/turn");
            msgOut.add(interface.encTurn);
            msgOut.send(dump);
            udpSock.writeBuffer(dump.buffer, dump.length);
        }
        else {
            app.currentScreen = AppData::MENU;
            app.menuScreenTimeout = MENU_TIMEOUT;
            app.oled(AppData::MENU).newScreen = 1;
        }
    }
    // same for aux screen
    if (app.currentScreen == AppData::AUX) {
        if (app.isAuxScreenEncoderOverride()) {
            OSCMessage msgOut("/encoder/turn");
            msgOut.add(interface.encTurn);
            msgOut.send(dump);
            udpSockAux.writeBuffer(dump.buffer, dump.length);
        }
        else {
            app.currentScreen = AppData::MENU;
            app.menuScreenTimeout = MENU_TIMEOUT;
            app.oled(AppData::MENU).newScreen = 1;
        }
    }
}

void knobsInput(uint32_t * adc) {
    bool changed = false;
    bool exprScaling = (pedalExprMin_!=0 || pedalExprMax_!=1023);
    // knob 1-4 + volume + expr , all 0-1023
    for(unsigned i = 0; i < MAX_KNOBS;i++) {
        int v = adc[i];

        if(i==EXPR_KNOB && exprScaling) {
            v = ( pedalExprMin_ <= pedalExprMax_ 
                ?  ((int32_t) ( v - pedalExprMin_ ) * 1023)  / (pedalExprMax_ - pedalExprMin_)
                :  ((int32_t) ( pedalExprMin_ - v ) * 1023)  / (pedalExprMin_ - pedalExprMax_)
                );
            v = std::max(std::min(v,1023),0);
        }

        if(v==0 || v==1023) {
            // allow extremes
            changed |= v != knobs_[i];
            knobs_[i] = v;
        } else {
            // 75% new value, 25% old value
            int16_t nv = (v >> 1) + (v >> 2) + (knobs_[i] >> 2);
            int diff = nv - knobs_[i];
            if(diff>2 || diff <-2) {
                changed = true;
                knobs_[i] = nv;
            }
        }
    }
    if(changed) {
        OSCMessage msgOut("/knobs");
        for(unsigned i = 0; i < MAX_KNOBS;i++) {
            msgOut.add(knobs_[i]);
        }
        msgOut.send(dump);
        udpSock.writeBuffer(dump.buffer, dump.length);        
    }
}

void footswitchInput(OSCMessage &msg) {
    switch(pedalSwitchMode_) {
        case PSM_FAVOURITES : {
            int pos = msg.getInt(0);

            // normally closed (1)
            // so if pos was open , and now closed we switch
            // i.e. do on lifting pedal
            if( footswitchPos == 0 && pos == 1) {
                menu.nextProgram();
            }

            footswitchPos = pos; 
            break;
        }
        case PSM_PATCH:
        default :  {
            msg.send(dump);
            udpSock.writeBuffer(dump.buffer, dump.length);      
            break;  
        }
    }  //switch
}


// this is when the encoder gets pressed
// in menu screen, execute the menu entry
// in patch screen, bounce back to menu, unless override is on
// in aux screen, same
void encoderButton(void) {
    if ( !  ( (app.currentScreen == AppData::PATCH && app.isPatchScreenEncoderOverride())
              || (app.currentScreen == AppData::AUX && app.isAuxScreenEncoderOverride()))) {

        if (interface.encBut) {
            if (encoderDownTime == -1) {
                encoderDownTime = SHUTDOWN_TIME;
            }
        }
        else {
            encoderDownTime = -1;
            if (previousScreen >= 0) {
                app.currentScreen = previousScreen;
                previousScreen = -1;
                app.oled((AppData::Screen) app.currentScreen ).newScreen = 1;
            }
        }
    }


    if (app.currentScreen == AppData::MENU) {
        if (interface.encBut == 1) {
            menu.encoderPress();
        }
        if (interface.encBut == 0) {
            menu.encoderRelease();
            // reset menu timeout when action is performed
            // (which is when we release encoder)
            app.menuScreenTimeout = MENU_TIMEOUT;
        }
    }

    // if in patch mode, send encoder, but only if the patch said it wants encoder access
    if (app.currentScreen == AppData::PATCH) {
        if (app.isPatchScreenEncoderOverride()) {
            OSCMessage msgOut("/encoder/button");
            msgOut.add(interface.encBut);
            msgOut.send(dump);
            udpSock.writeBuffer(dump.buffer, dump.length);
        }
    }
    // same for the aux screen
    if (app.currentScreen == AppData::AUX) {
        if (app.isAuxScreenEncoderOverride()) {
            OSCMessage msgOut("/encoder/button");
            msgOut.add(interface.encBut);
            msgOut.send(dump);
            udpSockAux.writeBuffer(dump.buffer, dump.length);
        }
    }
}


/* end OSC messages received from MCU */

/* helpers */
void sendLed(unsigned c) {
    OSCMessage msg("/led");
    msg.add(c);
    msg.send(dump);
    //slip.sendMessage(dump.buffer, dump.length, serial);
}

void setScreenLine(OledScreen &screen, int lineNum, OSCMessage &msg) {

    char str[256];
    char screenLine[256];
    int i = 0;

    screenLine[0] = 0;

    // since there are no strings in pd, the line message will be made of different types
    // cat the line together, then throw it up on the patch screen
    while (i < msg.size()) {
        if (msg.isString(i)) {
            msg.getString(i, str, 256);
            strcat(screenLine, str);
            strcat(screenLine, " ");
        }
        if (msg.isFloat(i)) {
            sprintf(str, "%g ", msg.getFloat(i));
            strcat(screenLine, str);
        }
        if (msg.isInt(i)) {
            sprintf(str, "%d ", msg.getInt(i));
            strcat(screenLine, str);
        }
        i++;
    }
    screen.setLine(lineNum, screenLine);
    //    printf("%s\n", screenLine);
}


void updateScreenPage(uint8_t page, OledScreen &screen) {

    uint8_t oledPage[128];
    uint8_t tmp;
    uint32_t i, j, esc;

    i = page;
    esc = 0;
    // copy 128 byte page from the screen buffer
    for (j = 0; j < 128; j++) {

        // hack to avoid too many SLIP END characters (192) in packet 
        // which causes packet size to increase and causes problems down the line
        tmp = screen.pix_buf[j + (i * 128)];
        if (tmp == 192){
            esc++;
            if (esc > 64) tmp = 128; // replace 192 with 128 'next best' 
        }
        oledPage[j] = tmp;
    }
    OSCMessage oledMsg("/oled");
    oledMsg.add(i);
    oledMsg.add(oledPage, 128);
    oledMsg.send(dump);
    //slip.sendMessage(dump.buffer, dump.length, serial);
    oledMsg.empty();
}





/* end helpers */



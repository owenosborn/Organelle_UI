
#include <wiringPi.h>
#include <wiringPiSPI.h>
#include <wiringShift.h>
#include <stdio.h>  
#include <stdlib.h>  
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "ssd1306.h"
#include "Hardware.h"

#define NUMBER_OF_SHIFT_CHIPS   4
#define DATA_WIDTH   NUMBER_OF_SHIFT_CHIPS * 8
#define PULSE_WIDTH_USEC   2
#define POLL_DELAY_MSEC   250

static const int ploadPin = 34;         // parallel load pin the 165
static const int clockEnablePin = 35;   // ce pin the 165
static const int dataPin = 33;             // Q7 pin the 165
static const int clockPin = 32;         // clk pin the 165
static const int oledDC = 5;            // DC pin of OLED
static const int oledRST = 6;         // RST pin of OLED
static const int LEDR = 22;          
static const int LEDG = 23;         
static const int LEDB = 24;         
static const int ampEn = 17;         

//static uint32_t pinValues;
//static uint32_t oldPinValues;

// OLED stuff
static unsigned char oled_initcode[] = {
	// Initialisation sequence
	SSD1306_DISPLAYOFF,                    // 0xAE
	SSD1306_SETLOWCOLUMN,            // low col = 0
	SSD1306_SETHIGHCOLUMN,           // hi col = 0
	SSD1306_SETSTARTLINE,            // line #0
	SSD1306_SETCONTRAST,                   // 0x81
	0xCF,
	0xa1,                                  // setment remap 95 to 0 (?)
	SSD1306_NORMALDISPLAY,                 // 0xA6
	SSD1306_DISPLAYALLON_RESUME,           // 0xA4
	SSD1306_SETMULTIPLEX,                  // 0xA8
	0x3F,                                  // 0x3F 1/64 duty
	SSD1306_SETDISPLAYOFFSET,              // 0xD3
	0x0,                                   // no offset
	SSD1306_SETDISPLAYCLOCKDIV,            // 0xD5
	0xF0,                                  // the suggested ratio 0x80
	SSD1306_SETPRECHARGE,                  // 0xd9
	0xF1,
	SSD1306_SETCOMPINS,                    // 0xDA
	0x12,                                  // disable COM left/right remap
	SSD1306_SETVCOMDETECT,                 // 0xDB
	0x40,                                  // 0x20 is default?
	SSD1306_MEMORYMODE,                    // 0x20
	0x00,                                  // 0x0 act like ks0108
	SSD1306_SEGREMAP | 0x1,
	SSD1306_COMSCANDEC,
	SSD1306_CHARGEPUMP,                    //0x8D
	0x14,
	// Enabled the OLED panel
	SSD1306_DISPLAYON
};

static unsigned char oled_poscode[] = {
   	SSD1306_SETLOWCOLUMN,            // low col = 0
	SSD1306_SETHIGHCOLUMN,           // hi col = 0
	SSD1306_SETSTARTLINE            // line #0
};

void Hardware::hardwareInit(void){
    // setup GPIO, this uses actual BCM pin numbers 
    wiringPiSetupGpio();

    // GPIO for shift registers
    pinMode(ploadPin, OUTPUT);
    pinMode(clockEnablePin, OUTPUT);
    pinMode(clockPin, OUTPUT);
    pinMode(dataPin, INPUT);
    digitalWrite(clockPin, LOW);
    digitalWrite(ploadPin, HIGH);
    digitalWrite(clockEnablePin, LOW);
    pinValues = 0;

    // enable amplifier
    pinMode(ampEn, OUTPUT);
    digitalWrite(ampEn, HIGH);

    // OLED pins
    pinMode (oledDC, OUTPUT) ;
    pinMode (oledRST, OUTPUT) ;
    wiringPiSPISetup(0, 4*1000*1000);
    wiringPiSPISetup(1, 4*1000*1000);  // for adc
    
    // reset OLED
    digitalWrite(oledRST,  LOW) ;
    delay(50);
    digitalWrite(oledRST,  HIGH) ;
    
    // initialize OLED
    digitalWrite(oledDC, LOW);
    wiringPiSPIDataRW(0, oled_initcode, 28);

    // GPIO for LEDs
    pinMode(LEDR, OUTPUT);
    pinMode(LEDG, OUTPUT);
    pinMode(LEDB, OUTPUT);
    digitalWrite(LEDR, LOW);
    digitalWrite(LEDG, LOW);
    digitalWrite(LEDB, LOW);
    delay(10); // flash em
//    digitalWrite(LEDR, HIGH);
//    digitalWrite(LEDG, HIGH);
 //   digitalWrite(LEDB, HIGH);

    // keys
    keyStatesLast = 0;

    clearFlags();
}

void Hardware::clearFlags(void){
    encButFlag = 0;
    encTurnFlag = 0;
}

void Hardware::flashLEDs(void) {
    digitalWrite(LEDR, LOW);
    digitalWrite(LEDG, LOW);
    digitalWrite(LEDB, LOW);

    delay(1);
    digitalWrite(LEDR, HIGH);
    digitalWrite(LEDG, HIGH);
    digitalWrite(LEDB, HIGH);

}

void Hardware::oledWrite(uint8_t * buf)
{
    // spi will overwrite the buffer with input, so we need a tmp
    uint8_t tmp[1024];
    memcpy(tmp, buf, 1024);
    
    digitalWrite(oledDC, LOW);
    wiringPiSPIDataRW(0, oled_poscode, 3);
    digitalWrite(oledDC, HIGH);
    wiringPiSPIDataRW(0, tmp, 1024);
}

uint32_t Hardware::shiftRegRead(void)
{
    uint32_t bitVal;
    uint32_t bytesVal = 0;

    // so far best way to do the bit banging reliably is to
    // repeat the calls to reduce output Fclk
    // delay functions no good for such small times

    // load
    digitalWrite(ploadPin, LOW);
    digitalWrite(ploadPin, LOW);
    digitalWrite(ploadPin, LOW);
    digitalWrite(ploadPin, LOW);
    digitalWrite(ploadPin, LOW);
    digitalWrite(ploadPin, LOW);
    digitalWrite(ploadPin, LOW);
    digitalWrite(ploadPin, LOW);
    
    digitalWrite(ploadPin, HIGH);
    digitalWrite(ploadPin, HIGH);
    digitalWrite(ploadPin, HIGH);
    digitalWrite(ploadPin, HIGH);
    digitalWrite(ploadPin, HIGH);
    digitalWrite(ploadPin, HIGH);
    digitalWrite(ploadPin, HIGH);
    digitalWrite(ploadPin, HIGH);
    
    // shiftin
   for(int i = 0; i < DATA_WIDTH; i++)
    {
        bitVal = digitalRead(dataPin);

        bytesVal |= (bitVal << ((DATA_WIDTH-1) - i));

        digitalWrite(clockPin, HIGH);
        digitalWrite(clockPin, HIGH);
        digitalWrite(clockPin, HIGH);
        digitalWrite(clockPin, HIGH);
        digitalWrite(clockPin, HIGH);
        digitalWrite(clockPin, HIGH);
        digitalWrite(clockPin, HIGH);
        digitalWrite(clockPin, HIGH);
        
        digitalWrite(clockPin, LOW);
        digitalWrite(clockPin, LOW);
        digitalWrite(clockPin, LOW);
        digitalWrite(clockPin, LOW);
        digitalWrite(clockPin, LOW);
        digitalWrite(clockPin, LOW);
        digitalWrite(clockPin, LOW);
        digitalWrite(clockPin, LOW);
    }

    //bytesVal = shiftIn(dataPin, clockPin, MSBFIRST);

    
    
    pinValues = bytesVal;
    return(bytesVal);
}

void Hardware::shiftRegDisplay(void)
{
    for(int i = 0; i < DATA_WIDTH; i++)
    {
        printf(" ");

        if((pinValues >> ((DATA_WIDTH-1)-i)) & 1)
            printf("1");
        else
            printf("0");

    }
    printf("\n");
}

void Hardware::getKeyStates(void){
    keyStates = 0;
    //for (int i = 0; i < 25; i++) {
    //    keyStates |= (pinValues >> (i + 7) & 1) << i;
    //}
    
    keyStates |= (pinValues >> (0 + 7) & 1) << 24;
    keyStates |= (pinValues >> (1 + 7) & 1) << 16;
    keyStates |= (pinValues >> (2 + 7) & 1) << 17;
    keyStates |= (pinValues >> (3 + 7) & 1) << 18;
    keyStates |= (pinValues >> (4 + 7) & 1) << 19;
    keyStates |= (pinValues >> (5 + 7) & 1) << 20;
    keyStates |= (pinValues >> (6 + 7) & 1) << 21;
    keyStates |= (pinValues >> (7 + 7) & 1) << 22;
   
    keyStates |= (pinValues >> (8 + 7) & 1) << 23;
    keyStates |= (pinValues >> (9 + 7) & 1) << 8;
    keyStates |= (pinValues >> (10 + 7) & 1) << 9;
    keyStates |= (pinValues >> (11 + 7) & 1) << 10;
    keyStates |= (pinValues >> (12 + 7) & 1) << 11;
    keyStates |= (pinValues >> (13 + 7) & 1) << 12;
    keyStates |= (pinValues >> (14 + 7) & 1) << 13;
    keyStates |= (pinValues >> (15 + 7) & 1) << 14;
    
    keyStates |= (pinValues >> (16 + 7) & 1) << 15;
    keyStates |= (pinValues >> (17 + 7) & 1) << 0;
    keyStates |= (pinValues >> (18 + 7) & 1) << 1;
    keyStates |= (pinValues >> (19 + 7) & 1) << 2;
    keyStates |= (pinValues >> (20 + 7) & 1) << 3;
    keyStates |= (pinValues >> (21 + 7) & 1) << 4;
    keyStates |= (pinValues >> (22 + 7) & 1) << 5;
    keyStates |= (pinValues >> (23 + 7) & 1) << 6;
    
    keyStates |= (pinValues >> (24 + 7) & 1) << 7;
    
    keyStates |= (0xFE000000);  // zero out the bits not key bits
    keyStates = ~keyStates;
}

// check for encoder events after reading the shift register pins
void Hardware::checkEncoder(void){

	static uint8_t encoder_last = 0;
	uint8_t encoder = 0;

	// because the encoder has a crappy switch, we need  a
	// different debouce time for press and release
	// assume checkEncoder gets called every 5ms,
	// we will wait 10 counts for press and 50 counts for release
	#define PRESS 0
	#define RELEASE 1
	uint8_t button;
	static uint8_t button_last = RELEASE;
	static uint8_t press_count = 0;
	static uint8_t release_count = 0;

	button = (pinValues >> 4) & 0x1;
	if (button == PRESS) {
		press_count++;
		release_count = 0;
	}
	if ((press_count > 10) && (button_last == RELEASE)){	// press
			button_last = PRESS;
			release_count = 0;
            encBut = 1;
            encButFlag = 1;
	}

	if (button == RELEASE) {
		release_count++;
		press_count = 0;
	}
	if ((release_count > 10) && (button_last == PRESS)){	// release
			button_last = RELEASE;
			press_count = 0;
			printf("RELEASED!!!!!\n");
            encBut = 0;
            encButFlag = 1;
	}

	// turning
	encoder = (pinValues >> 5) & 0x3;
	
    if (encoder != encoder_last) {
		if (encoder_last == 0) {
			if (encoder == 2){
				encTurn = 0;
                encTurnFlag = 1;
			}
            if (encoder == 1){
                encTurn = 1;
                encTurnFlag = 1;
		    }
        }
		if (encoder_last == 3) {
			if (encoder == 1){
                encTurn = 0;
                encTurnFlag = 1;
			}
            if (encoder == 2){
                encTurn = 1;
                encTurnFlag = 1;
		    }
        }
		encoder_last = encoder;
	}
}

void Hardware::adcReadAll(void) 
{
    adcs[0] = adcRead(0);
    adcs[1] = adcRead(1);
    adcs[2] = adcRead(2);
    adcs[3] = adcRead(3);
    adcs[4] = adcRead(4);
    adcs[5] = adcRead(5);
    adcs[6] = adcRead(7);
}

// read a channel
uint32_t Hardware::adcRead(uint8_t adcnum)
{ 
    unsigned int commandout = 0;

    commandout = adcnum & 0x7;  // only 0-7
    commandout |= 0x18;     // start bit + single-ended bit

    uint8_t spibuf[3];

    spibuf[0] = commandout;
    spibuf[1] = 0;
    spibuf[2] = 0;

    wiringPiSPIDataRW(1, spibuf, 3);    

    return ((spibuf[1] << 8) | (spibuf[2])) >> 4;
    
}
/*
int main(void)
{

    uint8_t dung[1024];
    uint32_t adcs[7];

    for (int i =0; i< 1024; i++){
        dung[i] = i & 0xff;
    }
    hardwareInit();
    pinValues = shiftRegRead();
    shiftRegDisplay();
    oldPinValues = pinValues;
   
    oledWrite(dung);
    for (;;) {
        pinValues = shiftRegRead();
        if(pinValues != oldPinValues)
        {
            shiftRegDisplay();
            oldPinValues = pinValues;
        }

        adcs[0] = adcRead(0);
        adcs[1] = adcRead(1);
        adcs[2] = adcRead(2);
        adcs[3] = adcRead(3);
        adcs[4] = adcRead(4);
        adcs[5] = adcRead(5);
        adcs[6] = adcRead(7);

        for (int i = 0; i < 7; i++){
            printf("%d ", adcs[i]);
        }
        printf("\n");
        delay(POLL_DELAY_MSEC);
    }

    return 0;
}
*/



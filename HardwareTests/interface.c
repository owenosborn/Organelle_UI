#include <wiringPi.h>
#include <wiringPiSPI.h>
#include <stdio.h>  
#include <stdlib.h>  
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "ssd1306.h"

#define NUMBER_OF_SHIFT_CHIPS   4
#define DATA_WIDTH   NUMBER_OF_SHIFT_CHIPS * 8
#define PULSE_WIDTH_USEC   1
#define POLL_DELAY_MSEC   500

static const int ploadPin = 34;         // parallel load pin the 165
static const int clockEnablePin = 35;   // ce pin the 165
static const int dataPin = 33;             // Q7 pin the 165
static const int clockPin = 32;         // clk pin the 165
static const int oledDC = 5;            // DC pin of OLED
static const int oledRST = 6;         // RST pin of OLED

static uint32_t pinValues;
static uint32_t oldPinValues;

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
	SSD1306_SEGREMAP,
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


void hardwareInit(void){
    // setup GPIO, this uses actual BCM pin numbers 
    wiringPiSetupGpio();

    // GPIO for shift registers
    pinMode(ploadPin, OUTPUT);
    pinMode(clockEnablePin, OUTPUT);
    pinMode(clockPin, OUTPUT);
    pinMode(dataPin, INPUT);
    digitalWrite(clockPin, LOW);
    digitalWrite(ploadPin, HIGH);

    // OLED
    pinMode (oledDC, OUTPUT) ;
    pinMode (oledRST, OUTPUT) ;
    wiringPiSPISetup(0, 4*1000*1000);
    wiringPiSPISetup(1, 4*1000*1000);  // for adc
    
    // reset
    digitalWrite(oledRST,  LOW) ;
    delay(50);
    digitalWrite(oledRST,  HIGH) ;
    
    // initialize it
    digitalWrite(oledDC, LOW);
    wiringPiSPIDataRW(0, oled_initcode, 28);

}

void oledWrite(uint8_t * buf)
{
    digitalWrite(oledDC, LOW);
    wiringPiSPIDataRW(0, oled_poscode, 3);
    digitalWrite(oledDC, HIGH);
    wiringPiSPIDataRW(0, buf, 1024);
}

uint32_t shiftRegRead()
{
    long bitVal;
    uint32_t bytesVal = 0;

    // load
    digitalWrite(clockEnablePin, HIGH);
    digitalWrite(ploadPin, LOW);
    delayMicroseconds(PULSE_WIDTH_USEC);
    digitalWrite(ploadPin, HIGH);
    digitalWrite(clockEnablePin, LOW);
    delayMicroseconds(PULSE_WIDTH_USEC);
    // shiftin
    for(int i = 0; i < DATA_WIDTH; i++)
    {
        bitVal = digitalRead(dataPin);

        bytesVal |= (bitVal << ((DATA_WIDTH-1) - i));

        digitalWrite(clockPin, HIGH);
        delayMicroseconds(PULSE_WIDTH_USEC);
        digitalWrite(clockPin, LOW);
    }

    return(bytesVal);
}

void shiftRegDisplay()
{
    for(int i = 0; i < DATA_WIDTH; i++)
    {
        printf(" ");

        if((pinValues >> i) & 1)
            printf("1");
        else
            printf("0");

    }
    printf("\n");
}


void checkEncoder(void){

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
            //encBut = 1;
            //encButFlag = 1;
	}

	if (button == RELEASE) {
		release_count++;
		press_count = 0;
	}
	if ((release_count > 10) && (button_last == PRESS)){	// release
			button_last = RELEASE;
			press_count = 0;
			printf("RELEASED!!!!!\n");
            //encBut = 0;
           // encButFlag = 1;
	}

	// turning
	encoder = (pinValues >> 5) & 0x3;
	
    if (encoder != encoder_last) {
		if (encoder_last == 0) {
			if (encoder == 2){
			//	encTurn = 0;
              //  encTurnFlag = 1;
			}
            if (encoder == 1){
                //encTurn = 1;
               // encTurnFlag = 1;
		    }
        }
		if (encoder_last == 3) {
			if (encoder == 1){
                //encTurn = 0;
                //encTurnFlag = 1;
			}
            if (encoder == 2){
                //encTurn = 1;
               // encTurnFlag = 1;
		    }
        }
		encoder_last = encoder;
	}
}


// read a channel
uint32_t adcRead(uint8_t adcnum)
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
        /*pinValues = shiftRegRead();
        if(pinValues != oldPinValues)
        {
            shiftRegDisplay();
            //uint32_t t = (pinValues >> 5) & 3;
            //printf("%d\n", t);
            oldPinValues = pinValues;
        }
        checkEncoder();
*/
        
            printf("\n");
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
        delay(POLL_DELAY_MSEC);
    }

    return 0;
}



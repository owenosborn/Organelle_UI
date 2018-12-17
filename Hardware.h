#ifndef HARDWARE_H
#define HARDWARE_H

#include <stdint.h> 

class Hardware
{
    public:
        uint32_t encBut;
        uint32_t encButFlag;
        uint32_t encTurn;
        uint32_t encTurnFlag;
        uint32_t pinValues;
        uint32_t pinValuesLast;
        uint32_t adcs[8];

        uint32_t keyStates;
        uint32_t keyStatesLast;

        void adcReadAll(void);
        void hardwareInit(void);
        void clearFlags(void);
        void checkEncoder(void);
        void getKeyStates(void);
        void oledWrite(uint8_t * buf);
        uint32_t shiftRegRead(void);
        void shiftRegDisplay(void);
        uint32_t adcRead(uint8_t adcnum);
        void flashLEDs(void);
};


#endif

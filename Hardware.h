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
        
        void hardwareInit(void);
        void clearFlags(void);
        void checkEncoder(void);
        void oledWrite(uint8_t * buf);
        uint32_t shiftRegRead(void);
        void shiftRegDisplay(void);
        uint32_t adcRead(uint8_t adcnum);

};


#endif

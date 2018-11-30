#ifndef HARDWARE_H
#define HARDWARE_H

#include <stdint.h> 

class Hardware
{
    public:
        uint32_t pinValues;
        void hardwareInit(void);
        void oledWrite(uint8_t * buf);
        uint32_t shiftRegRead(void);
        void shiftRegDisplay(void);
        uint32_t adcRead(uint8_t adcnum);

};


#endif

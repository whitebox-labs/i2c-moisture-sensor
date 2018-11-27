#include "firmware.h"
#include <avr/io.h>
#include <util/delay.h>
#include <avr/pgmspace.h>
#include "usi_i2c_master.h"

#define ISP_OUT   PORTA
#define ISP_IN    PINA
#define ISP_DDR   DDRA
#define ISP_RST   PA0
#define ISP_MOSI  PA6
#define ISP_MISO  PA5
#define ISP_SCK   PA4
#define LED_K PB0 
#define LED_A PB1

#define CLOCK_T_320us   60
#define TIMERVALUE      TCNT0
#define clockInit()  TCCR0B = (1 << CS01) | (1 << CS00);

#define PAGE_SIZE 8 //words

unsigned char sck_sw_delay = 1;

void inline ledOn() {
  DDRB |= _BV(LED_A) | _BV(LED_K); //forward bias the LED
  PORTB &= ~_BV(LED_K);            //flash it to discharge the PN junction capacitance
  PORTB |= _BV(LED_A);  
}

void inline ledOff() {
  DDRB &= ~(_BV(LED_A) | _BV(LED_K)); //make pins inputs
  PORTB &= ~(_BV(LED_A) | _BV(LED_K));//disable pullups
}


static inline void clockWait(uint8_t time) {

    uint8_t i;
    for (i = 0; i < time; i++) {
        uint8_t starttime = TIMERVALUE;
        while ((uint8_t) (TIMERVALUE - starttime) < CLOCK_T_320us) {
        }
    }
}

static inline void ispDelay() {
    uint8_t starttime = TIMERVALUE;
    while ((uint8_t) (TIMERVALUE - starttime) < sck_sw_delay) {
    }
}

unsigned char ispTransmit(unsigned char data) {
    unsigned char rec_byte = 0;
    unsigned char i;
    for (i = 0; i < 8; i++) {
        /* set MSB to MOSI-pin */
        if ((data & 0x80) != 0) {
            ISP_OUT |= (1 << ISP_MOSI); /* MOSI high */
        } else {
            ISP_OUT &= ~(1 << ISP_MOSI); /* MOSI low */
        }
        /* shift to next bit */
        data = data << 1;

        /* receive data */
        rec_byte = rec_byte << 1;
        if ((ISP_IN & (1 << ISP_MISO)) != 0) {
            rec_byte++;
        }

        /* pulse SCK */
        ISP_OUT |= (1 << ISP_SCK); /* SCK high */
        ispDelay();
        ISP_OUT &= ~(1 << ISP_SCK); /* SCK low */
        ispDelay();
    }

    return rec_byte;
}

static inline unsigned char ispEnterProgrammingMode() {
    unsigned char check;
    unsigned char retry = 32;

    while (retry--) {
        ispTransmit(0xAC);
        ispTransmit(0x53);
        check = ispTransmit(0);
        ispTransmit(0);

        if (check == 0x53) {
            return 0;
        }

        /* pulse RST */
        ispDelay();
        ISP_OUT |= (1 << ISP_RST); /* RST high */
        ispDelay();
        ISP_OUT &= ~(1 << ISP_RST); /* RST low */
        ispDelay();
    }
    return 1; /* error: device dosn't answer */
}

static inline void ispConnect() {
    ISP_DDR = 0;
    ISP_OUT = 0;
    /* all ISP pins are inputs before */
    /* now set output pins */
    ISP_DDR |= (1 << ISP_RST) | (1 << ISP_SCK) | (1 << ISP_MOSI);

    /* reset device */
    ISP_OUT &= ~(1 << ISP_RST); /* RST low */
    ISP_OUT &= ~(1 << ISP_SCK); /* SCK low */

    /* positive reset pulse > 2 SCK (target) */
    ispDelay();
    ISP_OUT |= (1 << ISP_RST); /* RST high */
    ispDelay();
    ISP_OUT &= ~(1 << ISP_RST); /* RST low */
}

void ispCommand(unsigned char instruction, unsigned char a1, unsigned char a2, unsigned char a3) {
    ispTransmit(instruction);
    ispTransmit(a1);
    ispTransmit(a2);
    ispTransmit(a3);    
}

static inline unsigned char isPageBoundary(unsigned int i) {
    return i > 0 && 0 == (i + 1) % (PAGE_SIZE);
}

static inline void setFuses(unsigned char low, unsigned char high, unsigned char extended) {
    ispCommand(0xAC, 0xA0, 0x00, low);
    clockWait(30);
    ispCommand(0xAC, 0xA8, 0x00, high);
    clockWait(30);
    ispCommand(0xAC, 0xA4, 0x00, extended);
    clockWait(30);
}

static inline void chipErase() {
    ispCommand(0xAC, 0x80, 00, 00);
    clockWait(30);
}

static inline void flashFirmware() {
    unsigned int i;
    for(i = 0; i < FIRMWARE_WORD_COUNT; i++) {
        if(i % 2) ledOn();
        else ledOff();
        unsigned int word = pgm_read_word(&(firmware[i]));
        ispCommand(0x40, i >> 8, i & 0xFF, word & 0xFF);
        ispCommand(0x48, i >> 8, i & 0xFF, word >> 8);
        if(isPageBoundary(i)) {
            ispCommand(0x4C, i >> 8, i & 0xFF, 0x00);
            clockWait(15);
        }
    }
    
    if(!isPageBoundary(i - 1)) {
        ispCommand(0x4C, (i -1) >> 8, (i - 1) & 0xFF, 0x00);
    }
}

static inline void blink(unsigned int count) {
        int i = 0;
        for(i = 0; i < count; i++) {
                ledOn();
                _delay_ms(50);
                ledOff();
                _delay_ms(50);
        }
}

volatile unsigned char incomming[] = {0x20 << 1 | 0x01, 0x00, 0x00};

static inline char testFirmwareVersionPasses() {
    char getFwVersionCommand[] = {0x20 << 1, 0x07};
    incomming[1] = 0;
    USI_I2C_Master_Start_Transmission(getFwVersionCommand, 2);
    USI_I2C_Master_Start_Transmission(incomming, 2);
    return 0x23 == incomming[1];
}

static inline char testCapacitanceWithinLimits() {
    char readCapacitanceCommand[] = {0x20 << 1, 0x00};
    USI_I2C_Master_Start_Transmission(readCapacitanceCommand, 2);
    incomming[1] = 0;
    incomming[2] = 0;
    _delay_ms(1);
    USI_I2C_Master_Start_Transmission(incomming, 3);
    unsigned int capacitance = ((((unsigned int) incomming[1]) << 8) | incomming[2]);
    return capacitance > 180 && capacitance < 300;
} 

static inline char testTempWithinLimits() {
    char readCapacitanceCommand[] = {0x20 << 1, 0x05};
    USI_I2C_Master_Start_Transmission(readCapacitanceCommand, 2);
    incomming[1] = 0;
    incomming[2] = 0;
    _delay_ms(1);
    USI_I2C_Master_Start_Transmission(incomming, 3);
    unsigned int temp = ((((unsigned int) incomming[1]) << 8) | incomming[2]);
    return temp > 100 && temp < 400;
} 

static inline void blinkError() {
    ledOn();
    _delay_ms(2000);
    ledOff();
}

static inline unsigned char testsPass() {
    return testFirmwareVersionPasses() &&
           testTempWithinLimits() &&
           testCapacitanceWithinLimits();
}

void main(void) {
        clockInit();
        
        while(1) {
                ispConnect();
                
                _delay_ms(100);
                while(ispEnterProgrammingMode()) {
                    ispConnect();
                    _delay_ms(500);
                    ledOn();
                    _delay_ms(500);
                    ledOff();
                }
                
                setFuses(0xAE, 0xDF, 0xF5);
                chipErase();
                flashFirmware();
                ledOff();

                ISP_OUT |= (1 << ISP_RST); //release reset
                _delay_ms(300);
                
                ISP_DDR = 0;
                ISP_OUT = 0;
                
                if(!testsPass()) {
                    blinkError();
                } else {
                    blink(30);
                }
                USICR = 0;
        }
}

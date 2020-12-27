// TinyCalibrator - OSC Calibrator and High Voltage Fuse Resetter
//
// Because the ATtinys only have a few pins available, they are usually
// operated without an external clock. The internal oscillator does a good
// job in most applications, but when it comes to precise timing, it is
// too inaccurate. The accuracy of the oscillator of an ATtiny is only 
// +/-10%. Fortunately, the oscillator can be calibrated, increasing its
// accuracy to +/-2% or better. There are a few ways to perform this manual
// calibration, but several steps are required. The TinyCalibrator does
// this fully automatically by a push of a button. In order to make the
// device more versatile, a high voltage fuse resetter was also integrated,
// with which "bricked" ATtinys can be reset to the factory state.
//
// To carry out the calibration, a program is first uploaded to the target
// ATtiny using the integrated high voltage serial programmer. This program
// applies an oscillating signal with half the clock frequency to pin PB0
// of the target ATtiny. Since the fuses were previously set so that the
// target ATtiny runs with a prescaler of 8, a signal with 1/16 of the
// oscillator frequency is applied to its PB0. This frequency is measured
// by the timers of the ATtiny84 and compared with the target value. The
// oscillator calibration value (OSCCAL) is then adjusted accordingly and
// written into the EEPROM of the target ATtiny. This value is in turn read
// by the target ATtiny and written to its OSCCAL register. This process is
// repeated until the OSCCAL value, which leads to the lowest frequency
// deviation, has been found.
//
// The high voltage serial programmer implementation is based on TinyHVSP
// https://github.com/wagiminator/ATtiny84-TinyHVSP
//
// The I²C OLED implementation is based on TinyOLEDdemo
// https://github.com/wagiminator/ATtiny13-TinyOLEDdemo
//
// Controller:  ATtiny44/84
// Clockspeed:  12 MHz external
//
// 2020 by Stefan Wagner 
// Project Files (EasyEDA): https://easyeda.com/wagiminator
// Project Files (Github):  https://github.com/wagiminator
// License: http://creativecommons.org/licenses/by-sa/3.0/


// Libraries
#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <util/delay.h>

// Pin assignments PORTA
#define I2C_SCL   PA4     // I2C Serial Clock (SCK)
#define I2C_SDA   PA6     // I2C Serial Data (SDA)
#define SCI_PIN   PA0     // Serial Clock Input (SCI)         Pin 2 of target device
#define SDO_PIN   PA1     // Serial Data Output (SDO)         Pin 7 of target device
#define SII_PIN   PA2     // Serial Instruction Input (SII)   Pin 6 of target device
#define SDI_PIN   PA3     // Serial Data Input (SDI)          Pin 5 of target device
#define RST_PIN   PA5     // Low  Voltage RESET               Pin 1 of target device
#define HVR_PIN   PA7     // High Voltage RESET               Pin 1 of target device
#define BUTTONS   PA0     // Buttons

// Pin assignments PORTB
#define VCC_PIN   PB2     // Target VCC                       Pin 8 of target device

// Global variables
uint8_t  inLFUSE, inHFUSE, inEFUSE;           // for reading fuses
uint8_t  outLFUSE, outHFUSE, outEFUSE;        // for writing fuses
uint8_t  inCALIB0, inCALIB1;                  // for reading calibration bytes
uint8_t  inLOCK;                              // for reading lock bits
uint8_t  target;                              // for target device
uint32_t signature;                           // for reading signature
uint16_t frequency;                           // for measures frequency in kHz

// -----------------------------------------------------------------------------
// I2C Implementation
// -----------------------------------------------------------------------------

// I2C macros
#define I2C_SDA_HIGH()  DDRA &= ~(1<<I2C_SDA) // release SDA   -> pulled HIGH by resistor
#define I2C_SDA_LOW()   DDRA |=  (1<<I2C_SDA) // SDA as output -> pulled LOW  by MCU
#define I2C_SCL_HIGH()  DDRA &= ~(1<<I2C_SCL) // release SCL   -> pulled HIGH by resistor
#define I2C_SCL_LOW()   DDRA |=  (1<<I2C_SCL) // SCL as output -> pulled LOW  by MCU
#define I2C_DELAY()     asm("lpm")            // delay 3 clock cycles

// I2C init function
void I2C_init(void) {
  DDRA  &= ~((1<<I2C_SDA)|(1<<I2C_SCL));  // pins as input (HIGH-Z) -> lines released
  PORTA &= ~((1<<I2C_SDA)|(1<<I2C_SCL));  // should be LOW when as ouput
}

// I2C transmit one data byte to the slave, ignore ACK bit, no clock stretching allowed
void I2C_write(uint8_t data) {
  I2C_DELAY();                            // delay 3 clock cycles
  for(uint8_t i = 8; i; i--, data<<=1) {  // transmit 8 bits, MSB first
    (data & 0x80) ? (I2C_SDA_HIGH()) : (I2C_SDA_LOW());  // SDA HIGH if bit is 1
    I2C_DELAY();                          // delay 3 clock cycles
    I2C_SCL_HIGH();                       // clock HIGH -> slave reads the bit
    I2C_DELAY();                          // delay 3 clock cycles
    I2C_SCL_LOW();                        // clock LOW again
  }
  I2C_DELAY();                            // delay 3 clock cycles
  I2C_DELAY();                            // delay 3 clock cycles
  I2C_SDA_HIGH();                         // release SDA for ACK bit of slave
  I2C_SCL_HIGH();                         // 9th clock pulse is for the ACK bit
  I2C_DELAY();                            // delay 3 clock cycles
  I2C_SCL_LOW();                          // but ACK bit is ignored
}

// I2C start transmission
void I2C_start(uint8_t addr) {
  I2C_SDA_LOW();                          // start condition: SDA goes LOW first
  I2C_SCL_LOW();                          // start condition: SCL goes LOW second
  I2C_write(addr);                        // send slave address
}

// I2C stop transmission
void I2C_stop(void) {
  I2C_SDA_LOW();                          // prepare SDA for LOW to HIGH transition
  I2C_SCL_HIGH();                         // stop condition: SCL goes HIGH first
  I2C_SDA_HIGH();                         // stop condition: SDA goes HIGH second
}

// -----------------------------------------------------------------------------
// OLED Implementation
// -----------------------------------------------------------------------------

// OLED definitions
#define OLED_ADDR       0x78              // OLED write address
#define OLED_CMD_MODE   0x00              // set command mode
#define OLED_DAT_MODE   0x40              // set data mode
#define OLED_INIT_LEN   9                 // length of init command array

// OLED 5x8 pixels character set
const uint8_t OLED_FONT[] PROGMEM = {
  0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2F, 0x00, 0x00, 0x00, 0x07, 0x00, 0x07, 0x00,
  0x14, 0x7F, 0x14, 0x7F, 0x14, 0x24, 0x2A, 0x7F, 0x2A, 0x12, 0x23, 0x13, 0x08, 0x64, 0x62,
  0x36, 0x49, 0x55, 0x22, 0x50, 0x00, 0x05, 0x03, 0x00, 0x00, 0x00, 0x1C, 0x22, 0x41, 0x00,
  0x00, 0x41, 0x22, 0x1C, 0x00, 0x14, 0x08, 0x3E, 0x08, 0x14, 0x08, 0x08, 0x3E, 0x08, 0x08,
  0x00, 0x00, 0xA0, 0x60, 0x00, 0x08, 0x08, 0x08, 0x08, 0x08, 0x00, 0x60, 0x60, 0x00, 0x00,
  0x20, 0x10, 0x08, 0x04, 0x02, 0x3E, 0x51, 0x49, 0x45, 0x3E, 0x00, 0x42, 0x7F, 0x40, 0x00,
  0x42, 0x61, 0x51, 0x49, 0x46, 0x21, 0x41, 0x45, 0x4B, 0x31, 0x18, 0x14, 0x12, 0x7F, 0x10,
  0x27, 0x45, 0x45, 0x45, 0x39, 0x3C, 0x4A, 0x49, 0x49, 0x30, 0x01, 0x71, 0x09, 0x05, 0x03,
  0x36, 0x49, 0x49, 0x49, 0x36, 0x06, 0x49, 0x49, 0x29, 0x1E, 0x00, 0x36, 0x36, 0x00, 0x00,
  0x00, 0x56, 0x36, 0x00, 0x00, 0x08, 0x14, 0x22, 0x41, 0x00, 0x14, 0x14, 0x14, 0x14, 0x14,
  0x00, 0x41, 0x22, 0x14, 0x08, 0x02, 0x01, 0x51, 0x09, 0x06, 0x32, 0x49, 0x59, 0x51, 0x3E,
  0x7C, 0x12, 0x11, 0x12, 0x7C, 0x7F, 0x49, 0x49, 0x49, 0x36, 0x3E, 0x41, 0x41, 0x41, 0x22,
  0x7F, 0x41, 0x41, 0x22, 0x1C, 0x7F, 0x49, 0x49, 0x49, 0x41, 0x7F, 0x09, 0x09, 0x09, 0x01,
  0x3E, 0x41, 0x49, 0x49, 0x7A, 0x7F, 0x08, 0x08, 0x08, 0x7F, 0x00, 0x41, 0x7F, 0x41, 0x00,
  0x20, 0x40, 0x41, 0x3F, 0x01, 0x7F, 0x08, 0x14, 0x22, 0x41, 0x7F, 0x40, 0x40, 0x40, 0x40,
  0x7F, 0x02, 0x0C, 0x02, 0x7F, 0x7F, 0x04, 0x08, 0x10, 0x7F, 0x3E, 0x41, 0x41, 0x41, 0x3E,
  0x7F, 0x09, 0x09, 0x09, 0x06, 0x3E, 0x41, 0x51, 0x21, 0x5E, 0x7F, 0x09, 0x19, 0x29, 0x46,
  0x46, 0x49, 0x49, 0x49, 0x31, 0x01, 0x01, 0x7F, 0x01, 0x01, 0x3F, 0x40, 0x40, 0x40, 0x3F,
  0x1F, 0x20, 0x40, 0x20, 0x1F, 0x3F, 0x40, 0x38, 0x40, 0x3F, 0x63, 0x14, 0x08, 0x14, 0x63,
  0x07, 0x08, 0x70, 0x08, 0x07, 0x61, 0x51, 0x49, 0x45, 0x43, 0x00, 0x7F, 0x41, 0x41, 0x00,
  0x02, 0x04, 0x08, 0x10, 0x20, 0x00, 0x41, 0x41, 0x7F, 0x00, 0x04, 0x02, 0x01, 0x02, 0x04,
  0x40, 0x40, 0x40, 0x40, 0x40, 0x00, 0x01, 0x02, 0x04, 0x00, 0x20, 0x54, 0x54, 0x54, 0x78,
  0x7F, 0x48, 0x44, 0x44, 0x38, 0x38, 0x44, 0x44, 0x44, 0x20, 0x38, 0x44, 0x44, 0x48, 0x7F,
  0x38, 0x54, 0x54, 0x54, 0x18, 0x08, 0x7E, 0x09, 0x01, 0x02, 0x18, 0xA4, 0xA4, 0xA4, 0x7C,
  0x7F, 0x08, 0x04, 0x04, 0x78, 0x00, 0x44, 0x7D, 0x40, 0x00, 0x40, 0x80, 0x84, 0x7D, 0x00,
  0x7F, 0x10, 0x28, 0x44, 0x00, 0x00, 0x41, 0x7F, 0x40, 0x00, 0x7C, 0x04, 0x18, 0x04, 0x78,
  0x7C, 0x08, 0x04, 0x04, 0x78, 0x38, 0x44, 0x44, 0x44, 0x38, 0xFC, 0x24, 0x24, 0x24, 0x18,
  0x18, 0x24, 0x24, 0x18, 0xFC, 0x7C, 0x08, 0x04, 0x04, 0x08, 0x48, 0x54, 0x54, 0x54, 0x20,
  0x04, 0x3F, 0x44, 0x40, 0x20, 0x3C, 0x40, 0x40, 0x20, 0x7C, 0x1C, 0x20, 0x40, 0x20, 0x1C,
  0x3C, 0x40, 0x30, 0x40, 0x3C, 0x44, 0x28, 0x10, 0x28, 0x44, 0x1C, 0xA0, 0xA0, 0xA0, 0x7C,
  0x44, 0x64, 0x54, 0x4C, 0x44, 0x08, 0x36, 0x41, 0x41, 0x00, 0x00, 0x00, 0x7F, 0x00, 0x00,
  0x00, 0x41, 0x41, 0x36, 0x08, 0x08, 0x04, 0x08, 0x10, 0x08, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

// OLED init settings
const uint8_t OLED_INIT_CMD[] PROGMEM = {
  0xC8, 0xA1,   // flip screen
  0xA8, 0x1F,   // set multiplex ratio
  0xDA, 0x02,   // set com pins hardware configuration
  0x8D, 0x14,   // set DC-DC enable
  0xAF          // display on
};

// OLED variables
uint8_t OLED_x, OLED_y;                   // current cursor position

// OLED init function
void OLED_init(void) {
  I2C_init();                             // initialize I2C first
  I2C_start(OLED_ADDR);                   // start transmission to OLED
  I2C_write(OLED_CMD_MODE);               // set command mode
  for (uint8_t i = 0; i < OLED_INIT_LEN; i++)
    I2C_write(pgm_read_byte(&OLED_INIT_CMD[i])); // send the command bytes
  I2C_stop();                             // stop transmission
}

// OLED set the cursor
void OLED_setCursor(uint8_t xpos, uint8_t ypos) {
  I2C_start(OLED_ADDR);                   // start transmission to OLED
  I2C_write(OLED_CMD_MODE);               // set command mode
  I2C_write(xpos & 0x0F);                 // set low nibble of start column
  I2C_write(0x10 | (xpos >> 4));          // set high nibble of start column
  I2C_write(0xB0 | (ypos & 0x07));        // set start page
  I2C_stop();                             // stop transmission
  OLED_x = xpos; OLED_y = ypos;           // set the cursor variables
}

// OLED clear line
void OLED_clearLine(uint8_t line) {
  OLED_setCursor(0, line);                // set cursor to line start
  I2C_start(OLED_ADDR);                   // start transmission to OLED
  I2C_write(OLED_DAT_MODE);               // set data mode
  for(uint8_t i=128; i; i--) I2C_write(0x00); // clear the line
  I2C_stop();                             // stop transmission
}

// OLED clear screen
void OLED_clearScreen(void) {
  for(uint8_t i=0; i<4; i++)              // 4 lines
    OLED_clearLine(i);                    // clear line
}

// OLED print a single character
void OLED_printChar(char c) {
  uint16_t ptr = c - 32;                  // character pointer
  ptr += ptr << 2;                        // -> ptr = (ch - 32) * 5;
  I2C_write(0x00);                        // write space between characters
  for (uint8_t i=5 ; i; i--) I2C_write(pgm_read_byte(&OLED_FONT[ptr++]));
  OLED_x += 6;                            // update cursor
  if (OLED_x > 122) {                     // line end ?
    I2C_stop();                           // stop data transmission
    OLED_setCursor(0,++OLED_y);           // set next line start
    I2C_start(OLED_ADDR);                 // start transmission to OLED
    I2C_write(OLED_DAT_MODE);             // set data mode
  }
}

// OLED print a string
void OLED_printStr(const char* p) {
  I2C_start(OLED_ADDR);                   // start transmission to OLED
  I2C_write(OLED_DAT_MODE);               // set data mode
  uint8_t i = 0;                          // position inside string
  char ch = p[i];                         // read first character in string
  while (ch != 0) {                       // repeat until string terminator
    OLED_printChar(ch);                   // print character on OLED
    ch = p[++i];                          // read next character
  }
  I2C_stop();                             // stop transmission
}

// OLED print a string from program memory
void OLED_printPrg(const char* p) {
  I2C_start(OLED_ADDR);                   // start transmission to OLED
  I2C_write(OLED_DAT_MODE);               // set data mode
  char ch = pgm_read_byte(p);             // read first character from program memory
  while (ch != 0) {                       // repeat until string terminator
    OLED_printChar(ch);                   // print character on OLED
    ch = pgm_read_byte(++p);              // read next character
  }
  I2C_stop();                             // stop transmission
}

// OLED convert byte nibble into hex character and prints it
void OLED_printNibble(uint8_t nibble) {
  char c;
  if (nibble <= 9)  c = '0' + nibble;
  else              c = 'A' + nibble - 10;
  OLED_printChar(c);
}

// OLED print byte as hex
void OLED_printHex(uint8_t value) {
  I2C_start(OLED_ADDR);                   // start transmission to OLED
  I2C_write(OLED_DAT_MODE);               // set data mode
  OLED_printNibble(value >> 4);           // print high nibble
  OLED_printNibble(value & 0x0F);         // print low nibble
  I2C_stop();                             // stop transmission
}

// OLED print 16-bit value as decimal (BCD conversion by substraction method)
void OLED_printDec(uint16_t value) {
  static uint16_t divider[5] = {10000, 1000, 100, 10, 1};  // for BCD conversion
  uint8_t leadflag = 0;
  I2C_start(OLED_ADDR);                         // start transmission to OLED
  I2C_write(OLED_DAT_MODE);                     // set data mode
  for(uint8_t digit = 0; digit < 5; digit++) {  // 5 digits
    uint8_t digitval = 0;                       // start with digit value 0
    while (value >= divider[digit]) {           // if current divider fits into the value
      leadflag = 1;                             // end of leading spaces
      digitval++;                               // increase digit value
      value -= divider[digit];                  // decrease value by divider
    }
    if (leadflag || (digit == 4)) OLED_printChar('0' + digitval);   // print the digit
    else OLED_printChar(' ');                   // print leading space
  }
  I2C_stop();                                   // stop transmission
}

// -----------------------------------------------------------------------------
// High Voltage Serial Programmer Implementation
// -----------------------------------------------------------------------------

// Target devices definitions
#define TGT_ERROR         0
#define TGT_T13           1
#define TGT_T25           2
#define TGT_T45           3
#define TGT_T85           4

// Desired fuse configuration (defaults) for ATtiny13
#define T13_LFUSE         0x6A
#define T13_HFUSE         0xFF

// Desired fuse configuration (defaults) for ATtiny25/45/85
#define Tx5_LFUSE         0x62
#define Tx5_HFUSE         0xDF
#define Tx5_EFUSE         0xFF

// Signatures
#define T13_SIG           0x1E9007
#define T25_SIG           0x1E9108
#define T45_SIG           0x1E9206
#define T85_SIG           0x1E930B

// HVSP macros
#define HVSP_RST_12V()    PORTA |=  (1<<HVR_PIN)
#define HVSP RST_HIGH()   PORTA =   (PORTA & (~(1<<HVR_PIN))) | (1<<RST_PIN)
#define HVSP_RST_LOW()    PORTA &= ~((1<<RST_PIN) | (1<<HVR_PIN))
#define HVSP_VCC_ON()     PORTB |=  (1<<VCC_PIN)
#define HVSP_VCC_OFF()    PORTB &= ~(1<<VCC_PIN)
#define HVSP_SCI_HIGH()   PORTA |=  (1<<SCI_PIN)
#define HVSP_SCI_LOW()    PORTA &= ~(1<<SCI_PIN)
#define HVSP_SII_HIGH()   PORTA |=  (1<<SII_PIN)
#define HVSP_SII_LOW()    PORTA &= ~(1<<SII_PIN)
#define HVSP_SDI_HIGH()   PORTA |=  (1<<SDI_PIN)
#define HVSP_SDI_LOW()    PORTA &= ~(1<<SDI_PIN)
#define HVSP_SDO_REL()    DDRA  &= ~(1<<SDO_PIN)
#define HVSP_SDO_BIT      (PINA &   (1<<SDO_PIN))
#define HVSP_CLOCK_OUT()  {HVSP_SCI_HIGH(); asm("nop"); HVSP_SCI_LOW();}

// Program for ATtiny13
const uint8_t PROG_T13_LENGTH = 72;
const uint8_t PROG_T13[] PROGMEM = {
  0x09, 0xC0, 0x0E, 0xC0, 0x0D, 0xC0, 0x0C, 0xC0, 0x0B, 0xC0, 0x0A, 0xC0, 0x09, 0xC0, 0x08, 0xC0,
  0x07, 0xC0, 0x06, 0xC0, 0x11, 0x24, 0x1F, 0xBE, 0xCF, 0xE9, 0xCD, 0xBF, 0x02, 0xD0, 0x12, 0xC0,
  0xEF, 0xCF, 0x80, 0xE0, 0x90, 0xE0, 0x07, 0xD0, 0x81, 0xBF, 0x81, 0xE0, 0x87, 0xBB, 0x92, 0xE4,
  0x9F, 0xBD, 0x83, 0xBF, 0xFF, 0xCF, 0xE1, 0x99, 0xFE, 0xCF, 0x8E, 0xBB, 0xE0, 0x9A, 0x99, 0x27,
  0x8D, 0xB3, 0x08, 0x95, 0xF8, 0x94, 0xFF, 0xCF
};

// Program for ATtiny25/45/85
const uint8_t PROG_Tx5_LENGTH = 84;
const uint8_t PROG_Tx5[] PROGMEM = {
  0x0E, 0xC0, 0x13, 0xC0, 0x12, 0xC0, 0x11, 0xC0, 0x10, 0xC0, 0x0F, 0xC0, 0x0E, 0xC0, 0x0D, 0xC0,
  0x0C, 0xC0, 0x0B, 0xC0, 0x0A, 0xC0, 0x09, 0xC0, 0x08, 0xC0, 0x07, 0xC0, 0x06, 0xC0, 0x11, 0x24,
  0x1F, 0xBE, 0xCF, 0xED, 0xCD, 0xBF, 0x02, 0xD0, 0x13, 0xC0, 0xEA, 0xCF, 0x80, 0xE0, 0x90, 0xE0,
  0x07, 0xD0, 0x81, 0xBF, 0x81, 0xE0, 0x87, 0xBB, 0x92, 0xE4, 0x9A, 0xBD, 0x83, 0xBF, 0xFF, 0xCF,
  0xE1, 0x99, 0xFE, 0xCF, 0x1F, 0xBA, 0x8E, 0xBB, 0xE0, 0x9A, 0x99, 0x27, 0x8D, 0xB3, 0x08, 0x95,
  0xF8, 0x94, 0xFF, 0xCF
};

// HVSP set up all control lines (output LOW)
void HVSP_init(void) {
  PORTA &= ~((1<<HVR_PIN) | (1<<RST_PIN) | (1<<SCI_PIN) | (1<<SDO_PIN) | (1<<SII_PIN) | (1<<SDI_PIN));
  DDRA  |=   (1<<HVR_PIN) | (1<<RST_PIN) | (1<<SCI_PIN) | (1<<SDO_PIN) | (1<<SII_PIN) | (1<<SDI_PIN);
  PORTB &=  ~(1<<VCC_PIN);
  DDRB  |=   (1<<VCC_PIN);
}

// HVSP release all control lines (input, no pullup)
void HVSP_release(void) {
  PORTA &= ~((1<<HVR_PIN) | (1<<RST_PIN) | (1<<SCI_PIN) | (1<<SDO_PIN) | (1<<SII_PIN) | (1<<SDI_PIN));
  DDRA  &= ~((1<<HVR_PIN) | (1<<RST_PIN) | (1<<SCI_PIN) | (1<<SDO_PIN) | (1<<SII_PIN) | (1<<SDI_PIN));
  PORTB &=  ~(1<<VCC_PIN);
  DDRB  &=  ~(1<<VCC_PIN);
}

// HVSP enter the programming mode
void HVSP_enterProgMode(void) {
  HVSP_init();                        // initialize control lines
  HVSP_VCC_ON();                      // apply VCC to target
  _delay_us(20);                      // wait 20us
  HVSP_RST_12V();                     // apply 12V to RESET pin of target
  _delay_us(10);                      // wait 10us
  HVSP_SDO_REL();                     // release SDO line
  _delay_us(300);                     // delay 300us
}

// HVSP exit the programming mode
void HVSP_exitProgMode(void) {
  HVSP_SCI_LOW();                     // set SCI line LOW
  HVSP_RST_LOW();                     // RESET to 0V
  HVSP_VCC_OFF();                     // power down the target
  HVSP_release();                     // release all control lines
}

// HVSP send instruction and receive reply
uint8_t HVSP_sendInstr(uint8_t SDI_BYTE, uint8_t SII_BYTE) {
  uint8_t SDO_BYTE = 0;               // receiver byte from target

  // wait until SDO_PIN goes high or time-out
  for(uint8_t i=10; i; i--) {
    if(HVSP_SDO_BIT) break;
    _delay_ms(1);
  }
       
  // send start bit
  HVSP_SDI_LOW();
  HVSP_SII_LOW();
  HVSP_CLOCK_OUT();

  // send instruction bytes; receive reply
  for(uint8_t i=8; i; i--) {
    (SDI_BYTE & 0x80) ? (HVSP_SDI_HIGH()) : (HVSP_SDI_LOW());
    (SII_BYTE & 0x80) ? (HVSP_SII_HIGH()) : (HVSP_SII_LOW());
    SDI_BYTE <<= 1;
    SII_BYTE <<= 1;
    SDO_BYTE <<= 1;
    if(HVSP_SDO_BIT) SDO_BYTE |= 1;
    HVSP_CLOCK_OUT();
  }
      
  // send end bits
  HVSP_SDI_LOW();
  HVSP_SII_LOW();
  HVSP_CLOCK_OUT();
  HVSP_CLOCK_OUT();
        
  return SDO_BYTE;
}

// HVSP read signature of target device
void HVSP_readSignature(void) {
  HVSP_sendInstr(0x08, 0x4C);
  HVSP_sendInstr(0x00, 0x0C);
  HVSP_sendInstr(0x00, 0x68);
  signature = HVSP_sendInstr(0x00, 0x6C);
  signature <<= 8;

  HVSP_sendInstr(0x01, 0x0C);
  HVSP_sendInstr(0x00, 0x68);
  signature |= HVSP_sendInstr(0x00, 0x6C);
  signature <<= 8;

  HVSP_sendInstr(0x02, 0x0C);
  HVSP_sendInstr(0x00, 0x68);
  signature |= HVSP_sendInstr(0x00, 0x6C);

  switch (signature) {
    case T13_SIG: target   = TGT_T13;   outLFUSE = T13_LFUSE;
                  outHFUSE = T13_HFUSE; break;
    case T25_SIG: target   = TGT_T25;   outLFUSE = Tx5_LFUSE;
                  outHFUSE = Tx5_HFUSE; outEFUSE = Tx5_EFUSE;  break;
    case T45_SIG: target   = TGT_T45;   outLFUSE = Tx5_LFUSE;
                  outHFUSE = Tx5_HFUSE; outEFUSE = Tx5_EFUSE;  break;
    case T85_SIG: target   = TGT_T85;   outLFUSE = Tx5_LFUSE;
                  outHFUSE = Tx5_HFUSE; outEFUSE = Tx5_EFUSE;  break;
    default:      target   = TGT_ERROR; break;
  }
}

// HVSP read calibration bytes
void HVSP_readCalib(void) {
  HVSP_sendInstr(0x08, 0x4C);
  HVSP_sendInstr(0x00, 0x0C);
  HVSP_sendInstr(0x00, 0x78);
  inCALIB0 = HVSP_sendInstr(0x00, 0x7C);

  if(target == TGT_T13) {
    HVSP_sendInstr(0x08, 0x4C);
    HVSP_sendInstr(0x01, 0x0C);
    HVSP_sendInstr(0x00, 0x78);
    inCALIB1 = HVSP_sendInstr(0x00, 0x7C);
  }
}

// HVSP read lock bits
void HVSP_readLock(void) {
  HVSP_sendInstr(0x04, 0x4C);
  HVSP_sendInstr(0x00, 0x78);
  inLOCK  = HVSP_sendInstr(0x00, 0x7C);
  inLOCK &= 0x03;
}

// HVSP read current fuse settings from target device
void HVSP_readFuses(void) {
  HVSP_sendInstr(0x04, 0x4C);
  HVSP_sendInstr(0x00, 0x68);
  inLFUSE = HVSP_sendInstr(0x00, 0x6C);

  HVSP_sendInstr(0x04, 0x4C);
  HVSP_sendInstr(0x00, 0x7A);
  inHFUSE = HVSP_sendInstr(0x00, 0x7E);

  if(target != TGT_T13) {
    HVSP_sendInstr(0x04, 0x4C);
    HVSP_sendInstr(0x00, 0x6A);
    inEFUSE = HVSP_sendInstr(0x00, 0x6E);
  }
}

// HVSP write fuse settings to target device
void HVSP_writeFuses(void) {
  HVSP_sendInstr(0x40,  0x4C);
  HVSP_sendInstr(outLFUSE, 0x2C);
  HVSP_sendInstr(0x00,  0x64);
  HVSP_sendInstr(0x00,  0x6C);

  HVSP_sendInstr(0x40,  0x4C);
  HVSP_sendInstr(outHFUSE, 0x2C);
  HVSP_sendInstr(0x00,  0x74);
  HVSP_sendInstr(0x00,  0x7C);

  if(target != TGT_T13) {
    HVSP_sendInstr(0x40,  0x4C);
    HVSP_sendInstr(outEFUSE & 0x01, 0x2C);
    HVSP_sendInstr(0x00,  0x66);
    HVSP_sendInstr(0x00,  0x6E);
  }
}

// HVSP perform chip erase
void HVSP_eraseChip(void) {
  HVSP_sendInstr(0x80, 0x4C);
  HVSP_sendInstr(0x00, 0x64);
  HVSP_sendInstr(0x00, 0x6C);
  while (!HVSP_SDO_BIT);        // wait for the chip erase cycle to finish
  HVSP_sendInstr(0x00, 0x4C);   // no operation to finish chip erase
}

// HVSP write to EEPROM
void HVSP_writeEEPROM(uint8_t addr, uint8_t data) {
  HVSP_sendInstr(0x11, 0x4C);    // enter EEPROM programming mode
  HVSP_sendInstr(addr, 0x0C);
  HVSP_sendInstr(data, 0x2C);
  HVSP_sendInstr(0x00, 0x6D);
  HVSP_sendInstr(0x00, 0x64);
  HVSP_sendInstr(0x00, 0x6C);
  while (!HVSP_SDO_BIT);        // wait for write cycle to finish
  HVSP_sendInstr(0x00, 0x4C);   // no operation to exit EEPROM programming
}

// HVSP write data from program memory to flash of target
void HVSP_writeFlash(const uint8_t* p, uint16_t length) {
  uint16_t addr = 0;
  uint8_t  pagesize = 16;
  if((target == TGT_T45) || (target == TGT_T85)) pagesize = 32;
  length = (length + 1) >> 1;

  HVSP_sendInstr(0x10, 0x4C);   // enter flash programming mode
  
  do {
    // write flash page
    HVSP_sendInstr(addr, 0x0C);
    HVSP_sendInstr(pgm_read_byte(p++), 0x2C);
    HVSP_sendInstr(pgm_read_byte(p++), 0x3C);
    HVSP_sendInstr(0x00, 0x7D);
    HVSP_sendInstr(0x00, 0x7C);

    // flush page if it's full
    if((++addr & (pagesize - 1)) == 0) {
      HVSP_sendInstr((addr-pagesize) >> 8, 0x1C);
      HVSP_sendInstr(0x00, 0x64);
      HVSP_sendInstr(0x00, 0x6C);
      while (!HVSP_SDO_BIT);
    }
  } while(--length);

  // flush last page if it's not full (not flushed yet)
  if(addr & (pagesize - 1)) {
    HVSP_sendInstr(addr >> 8, 0x1C);
    HVSP_sendInstr(0x00, 0x64);
    HVSP_sendInstr(0x00, 0x6C);
    while (!HVSP_SDO_BIT);
  }

  HVSP_sendInstr(0x00, 0x4C);   // no operation to exit flash programming
}

// -----------------------------------------------------------------------------
// Frequency Measurement Implementation
// -----------------------------------------------------------------------------

// Target power macros
#define TGT_VCC_ON()    {DDRB |= (1<<VCC_PIN); PORTB |= (1<<VCC_PIN); _delay_ms(100);}
#define TGT_VCC_OFF()   {PORTB &= ~(1<<VCC_PIN); DDRB &= ~(1<<VCC_PIN); _delay_ms(30);}

// Global variables for frequency measurement
volatile uint8_t FRQ_highByte;
volatile uint8_t FRQ_busy;

// Init frequency measurement
void FRQ_init(void) {
  OCR1A  = 48192;                   // timer1 compare match A after 32 ms
  TIMSK0 = (1<<TOIE0);              // enable timer0 overflow interrupt
  TIMSK1 = (1<<OCIE1A);             // enable timer1 compare match A interrupt
  sei();                            // enable global interrupts
}

// Measure frequency by counting signals from target (OSC / 16) for 32ms
void FRQ_measure(void) {
  TGT_VCC_ON();                     // power the target device
  FRQ_busy = 1;                     // sampling flag; "0" when completed
  FRQ_highByte = 0;                 // high byte for virtual 16-bit counter
  TCNT0  = 0;                       // clear timer0
  TCNT1  = 0;                       // clear timer1
  TCCR1B = (1<<CS11);               // start timer1, prescaler 8
  TCCR0B = (1<<CS02) | (1<<CS01);   // start counting signals from target
  while(FRQ_busy);                  // wait for sampling complete
  frequency = (uint16_t)(FRQ_highByte << 8) | TCNT0; 
  frequency >>= 1;                  // calculate frequency in kHz
  TGT_VCC_OFF();                    // shut off target device
}

// Timer0 overflow interrupt service routine (expand timer0 to 16-bit)
ISR(TIM0_OVF_vect) {
  FRQ_highByte++;
}

// Timer1 compare match A interrupt service routine (fires after 32ms);
ISR(TIM1_COMPA_vect) {
  TCCR0B   = 0;                     // stop counting signals
  TCCR1B   = 0;                     // stop timer1
  FRQ_busy = 0;                     // sampling complete
}

// -----------------------------------------------------------------------------
// Additional Functions
// -----------------------------------------------------------------------------

// Text strings stored in program memory
const char TitleScreen[]  PROGMEM = 
  "Tiny Calibrator v0.3 "
  "Insert  ATtiny  into "
  "the socket and press "
  "any key to continue. ";
const char SelectScreen[] PROGMEM =
  "(1) Calibrate OSC    "
  "(2) Reset Fuses      "
  "(3) Exit             ";
const char ErrorScreen[]  PROGMEM = 
  "Check correct place- "
  "ment of the chip and "
  "press a key to retry.";

const char ExitKeyStr[] PROGMEM =       "Press any key to exit";
const char CalibKeyStr[] PROGMEM =      "Press key to calibr. ";
const char ResetKeyStr[] PROGMEM =      "Press a key to reset ";
const char CurrentFuseStr[] PROGMEM =   "Current fuse settings";
const char CurrentCalibStr[] PROGMEM =  "Current OSC settings ";
const char CalibratingStr[] PROGMEM =   "Calibrating .........";
const char PleaseWaitStr[] PROGMEM =    "....... please wait !";
const char ResetTitleStr[] PROGMEM =    "HV Fuse Resetter";
const char ResetComplStr[] PROGMEM =    "Resetting finished";
const char DetectedStr[] PROGMEM =      "Detected: ";
const char AttinyStr[] PROGMEM =        "ATtiny";
const char ErrorStr[] PROGMEM =         "ERROR !";
const char DoneStr[] PROGMEM =          " done";
const char FrequencyStr[] PROGMEM =     "Freq (kHz):     ";
const char OsccalStr[] PROGMEM =        "OSCCAL value:      ";

// Read supply voltage in mV
uint16_t readVCC(void) {
  ADMUX   = (1<<MUX5) | (1<<MUX0);  // set 1.1V reference against Vcc
  _delay_ms(2);                     // wait for Vref to settle
  ADCSRA |= (1<<ADSC);              // start conversion
  while(ADCSRA & (1<<ADSC));        // wait for ADC conversion complete
  uint16_t vcc = ADC;               // get result
  vcc = 1125300L / vcc;             // calculate Vcc in mV; 1125300 = 1.1*1023*1000
  return vcc;                       // return VCC in mV
}

// Read button number; return 0 if no button is pressed
uint8_t readButton(void) {
  static uint16_t THRESHOLD[] = {896, 726, 597, 0};
  ADMUX   = BUTTONS;                // set buttons pin against Vcc
  ADCSRA |= (1<<ADSC);              // start conversion
  while(ADCSRA & (1<<ADSC));        // wait for ADC conversion complete
  uint16_t rawbutton = ADC;         // get result
  uint8_t button = 0;               // button number
  while (rawbutton < THRESHOLD[button]) button++; // figure out button number
  return (button);                  // return button number
}

// Wait for a button press; return pressed button number
uint8_t waitButton(void) {
  uint8_t button;                   // button number
  while(readButton());              // wait for button released
  do {
    _delay_ms(10);                  // debounce
    button = readButton();          // read button number
  } while(!button);                 // until button is pressed
  return (button);
}

// Calculate difference
uint16_t diff(uint16_t a, uint16_t b) {
  if (a > b) return (a - b);
  return (b - a);
}

// Calibrate oscillator of target device
void TGT_calibrate(void) {
  // print heading
  OLED_setCursor(0,0); OLED_printPrg(CurrentCalibStr);

  // read calibration byte, set fuses, upload code, write OSCCAL value to EEPROM
  HVSP_enterProgMode();
  HVSP_readSignature();
  HVSP_readCalib();
  HVSP_eraseChip();
  HVSP_writeFuses();
  if(target == TGT_T13) HVSP_writeFlash(PROG_T13, PROG_T13_LENGTH);
  else                  HVSP_writeFlash(PROG_Tx5, PROG_Tx5_LENGTH);
  HVSP_writeEEPROM(0, inCALIB0);
  HVSP_exitProgMode();

  // measure and print uncalibrated frequency
  FRQ_measure();
  OLED_printPrg(OsccalStr); OLED_printHex(inCALIB0);
  OLED_printPrg(FrequencyStr); OLED_printDec(frequency);
  OLED_printPrg(CalibKeyStr);
  waitButton();

  // print calibrating heading
  OLED_setCursor(0,0); OLED_printPrg(CalibratingStr);
  OLED_setCursor(0,3); OLED_printPrg(PleaseWaitStr);

  // calibration process variables
  uint8_t  calib      = inCALIB0;
  uint8_t  lastcalib  = calib;
  uint16_t difference = 65535;
  uint16_t targetfreq = 8000;
  if(target == TGT_T13) targetfreq = 9600;

  // calibration process
  while(1) {
    // leave if current OSCCAL value is worse than the last one
    if (diff(frequency, targetfreq) >= difference) break;

    // print current calibration results
    OLED_setCursor(0,1); OLED_printPrg(OsccalStr); OLED_printHex(calib);
                         OLED_printPrg(FrequencyStr); OLED_printDec(frequency);

    // calculate next OSCCAL value
    lastcalib = calib;
    difference = diff(frequency, targetfreq);
    if (frequency > targetfreq) calib--;
    else calib++;

    // program target for next calibration value
    HVSP_enterProgMode();
    HVSP_eraseChip();
    if(target == TGT_T13) HVSP_writeFlash(PROG_T13, PROG_T13_LENGTH);
    else                  HVSP_writeFlash(PROG_Tx5, PROG_Tx5_LENGTH);
    HVSP_writeEEPROM(0, calib);
    HVSP_exitProgMode();

    // measure frequency
    FRQ_measure();
  }

  // write final calibrated OSCCAL value to EEPROM
  HVSP_enterProgMode();
  HVSP_eraseChip();
  HVSP_writeEEPROM(0, lastcalib);
  HVSP_exitProgMode();

  // print calibration finished
  OLED_setCursor(96,0); OLED_printPrg(DoneStr);
  OLED_setCursor(0,3);  OLED_printPrg(ExitKeyStr);
  waitButton();
}

// Read and print current fuse settings on the OLED
void OLED_printFuses(void) {
  // read current fuse settings
  HVSP_enterProgMode();
  HVSP_readFuses();
  HVSP_exitProgMode();

  // print current fuse settings
  OLED_setCursor(0,1); OLED_printPrg(CurrentFuseStr);
  OLED_printStr("l: "); OLED_printHex(inLFUSE);
  OLED_printStr(" - h: "); OLED_printHex(inHFUSE);
  if (target != TGT_T13) {OLED_printStr(" - e: "); OLED_printHex(inEFUSE);}
}

// Reset fuses of target device
void TGT_resetFuses(void) {
  // print initial fuse settings
  OLED_clearScreen();
  OLED_setCursor(0,0); OLED_printPrg(ResetTitleStr);
  OLED_printFuses();
  OLED_setCursor(0,3); OLED_printPrg(ResetKeyStr);
  waitButton();

  // reset fuses
  HVSP_enterProgMode();
  HVSP_readSignature();
  HVSP_readLock();
  if(!inLOCK) HVSP_eraseChip();
  HVSP_writeFuses();
  HVSP_exitProgMode();
  _delay_ms(30);

  // print final fuse settings
  OLED_setCursor(0,0); OLED_printPrg(ResetComplStr);
  OLED_printFuses();
  OLED_setCursor(0,3); OLED_printPrg(ExitKeyStr);
  waitButton();
}

// -----------------------------------------------------------------------------
// Main Function
// -----------------------------------------------------------------------------

int main(void) {
  // setup
  ADCSRA = (1<<ADEN);               // setup ADC
  FRQ_init();                       // setup timers for frequency measurements
  OLED_init();                      // setup I2C OLED

  // loop
  while(1) {
    // print title screen
    OLED_clearScreen();
    OLED_setCursor(0,0);
    OLED_printPrg(TitleScreen);
    waitButton();

    // read target device information
    HVSP_enterProgMode();
    HVSP_readSignature();
    HVSP_exitProgMode();

    // print detected device or ERROR
    OLED_clearScreen();
    OLED_setCursor(0,0);
    OLED_printPrg(DetectedStr);
    if (target == TGT_ERROR) {
      OLED_printPrg(ErrorStr);
      OLED_setCursor(0,1);
      OLED_printPrg(ErrorScreen);
      waitButton();
      continue;
    }
    OLED_printPrg(AttinyStr);
    switch (target) {
      case TGT_T13:   OLED_printHex(0x13); break;
      case TGT_T25:   OLED_printHex(0x25); break;
      case TGT_T45:   OLED_printHex(0x45); break;
      case TGT_T85:   OLED_printHex(0x85); break;
      default:        break;
    }

    // print main menu
    OLED_setCursor(0,1); OLED_printPrg(SelectScreen);

    // read button and execute selected function
    uint8_t button = waitButton();
    switch (button) {
      case 1:         TGT_calibrate(); break;
      case 2:         TGT_resetFuses(); break;
      default:        break;
    }
  }
}

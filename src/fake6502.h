// fake6502.h — declarations for the vendored public-domain Fake6502 core (fake6502.c,
// (c)2011 Mike Chambers, released into the public domain). PSOROM runs the original Gottlieb
// sound-board 6502 on this core. The core calls read6502()/write6502() (provided by psorom).
#ifndef FAKE6502_H
#define FAKE6502_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

extern uint16_t pc;
extern uint8_t  sp, a, x, y, status;
extern uint32_t clockticks6502, instructions;

void reset6502(void);
void step6502(void);                 // execute one instruction
void exec6502(uint32_t tickcount);   // execute ~tickcount clock cycles
void irq6502(void);
void nmi6502(void);

// host-provided memory hooks (defined in psorom.cpp):
uint8_t read6502(uint16_t address);
void    write6502(uint16_t address, uint8_t value);

#ifdef __cplusplus
}
#endif
#endif

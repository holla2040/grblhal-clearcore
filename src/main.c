/*
 * grblhal-clearcore — Phase 0 minimal main.
 * Proves: bare-metal build, link at 0x4000, flash via Atmel-ICE, debugger
 * halt in main(). Replaced by the grblHAL driver entry in Phase 2.
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#include <sam.h>

volatile uint32_t counter;

int main(void)
{
    while (1) {
        counter++;
    }
}

/*
 * grblhal-clearcore — entry point. Reset_Handler (startup_same53.c) has
 * already run SystemInit (120 MHz clocks); grbl_enter() calls driver_init()
 * in driver.c and never returns.
 *
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#include "grbl/grbllib.h"

int main (void)
{
    grbl_enter();

    return 0;
}

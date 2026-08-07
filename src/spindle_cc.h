/*
 * spindle_cc.h — ClearCore spindle registration.
 * MIT License, Copyright (c) 2026 Craig Hollabaugh
 */

#ifndef __SPINDLE_CC_H__
#define __SPINDLE_CC_H__

void spindle_cc_register (void);
void spindle_cc_settings_changed (void);    /* re-run config on $30/$31/$33.. */

#endif

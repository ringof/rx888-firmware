#pragma once

CyU3PReturnStatus_t Si5351Init();

CyBool_t si5351_pll_locked(void);
CyBool_t si5351_clk0_enabled(void);

/* Diagnostic state from the most recent si5351_clk0_enabled() call —
 * surfaced by GETSTATS as bytes [20] (reg16) and [21] (result). */
extern volatile uint8_t glLastClk0Reg16;
extern volatile uint8_t glLastClk0Result;

CyU3PReturnStatus_t si5351aSetFrequencyA(UINT32 freq);
CyU3PReturnStatus_t si5351aSetFrequencyB(UINT32 freq2);

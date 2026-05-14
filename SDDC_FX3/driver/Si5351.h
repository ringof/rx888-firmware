#pragma once

/* Si5351 register addresses (subset — see Skyworks AN619 for the full set) */
#define SI_CLK0_CONTROL  16   /* CLK0_CONTROL — bit 7 is CLK0_PDN */

CyU3PReturnStatus_t Si5351Init();

CyBool_t si5351_pll_locked(void);
CyBool_t si5351_clk0_enabled(void);

CyU3PReturnStatus_t si5351aSetFrequencyA(UINT32 freq);
CyU3PReturnStatus_t si5351aSetFrequencyB(UINT32 freq2);

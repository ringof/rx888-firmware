/*
 * Si5351.c
 *
 *  Created on: 28/ott/2020
 *      Author: ik1xp
 */

#include "Application.h"

#define SI_CLK0_CONTROL		16			// Registers
#define SI_CLK1_CONTROL		17
#define SI_CLK2_CONTROL		18
#define SI_SYNTH_PLL_A		26
#define SI_SYNTH_PLL_B		34
#define SI_SYNTH_MS_0		42
#define SI_SYNTH_MS_1		50
#define SI_SYNTH_MS_2		58
#define SI_PLL_RESET		177

#define SI_R_DIV_1		(0)     //  0b00000000		/
#define SI_R_DIV_2		(0x10)  //  0b00010000
#define SI_R_DIV_4		(0x20)  //  0b00100000
#define SI_R_DIV_8		(0x30)  //  0b00110000
#define SI_R_DIV_16		(0x40)  //  0b01000000
#define SI_R_DIV_32		(0x50)  //  0b01010000
#define SI_R_DIV_64		(0x60)  //  0b01100000
#define SI_R_DIV_128	(0x70)  //  0b01110000

#define SI_CLK_SRC_PLL_A	0b00000000
#define SI_CLK_SRC_PLL_B	0b00100000

#define SI5351_FREQ	27000000			// Crystal frequency
#define SI5351_PLL_FIXED                80000000000ULL

#define SI5351_ADDR        			    (0xC0) // (0x60 << 1 )

#define SI5351_CRYSTAL_LOAD             183
#define SI5351_CRYSTAL_LOAD_MASK        (3<<6)
#define SI5351_CRYSTAL_LOAD_0PF         (0<<6)
#define SI5351_CRYSTAL_LOAD_6PF         (1<<6)
#define SI5351_CRYSTAL_LOAD_8PF         (2<<6)
#define SI5351_CRYSTAL_LOAD_10PF        (3<<6)

#define SI5351_PLL_INPUT_SOURCE         15
#define SI5351_CLKIN_DIV_MASK           (3<<6)
#define SI5351_CLKIN_DIV_1              (0<<6)
#define SI5351_CLKIN_DIV_2              (1<<6)
#define SI5351_CLKIN_DIV_4              (2<<6)
#define SI5351_CLKIN_DIV_8              (3<<6)
#define SI5351_PLLB_SOURCE              (1<<3)
#define SI5351_PLLA_SOURCE              (1<<2)

CyU3PReturnStatus_t Si5351Init()
{
	CyU3PReturnStatus_t status;
	status = I2cTransferW1 ( SI5351_CRYSTAL_LOAD , SI5351_ADDR, 0x52);
	if (status != CY_U3P_SUCCESS)
		return status;

	status = I2cTransferW1 (     SI_CLK0_CONTROL , SI5351_ADDR, 0x80); // clocks off
	if (status != CY_U3P_SUCCESS)
		return status;

	status = I2cTransferW1 (     SI_CLK1_CONTROL , SI5351_ADDR, 0x80); // clocks off
	if (status != CY_U3P_SUCCESS)
		return status;

	status = I2cTransferW1 (     SI_CLK2_CONTROL , SI5351_ADDR, 0x80); // clocks off
	if (status != CY_U3P_SUCCESS)
		return status;

	return status;
}


//
// Set up specified PLL with mult, num and denom
// mult is 15..90
// num is 0..1,048,575 (0xFFFFF)
// denom is 0..1,048,575 (0xFFFFF)
//
CyU3PReturnStatus_t SetupPLL(UINT8 pll, UINT8 mult, UINT32 num, UINT32 denom)
{
	UINT32 P1;					// PLL config register P1
	UINT32 P2;					// PLL config register P2
	UINT32 P3;					// PLL config register P3
	UINT8 data[8];

	// the actual multiplier is  mult + num / denom

	P1 = 128 * (UINT32)mult + (128 * num) / denom - 512;
	P2 = 128 * num - denom * ((128 * num) / denom);
	P3 = denom;

	data[0] = (P3 & 0x0000FF00) >> 8;
	data[1] = (P3 & 0x000000FF);
	data[2] = (P1 & 0x00030000) >> 16;
	data[3] = (P1 & 0x0000FF00) >> 8;
	data[4] = (P1 & 0x000000FF);
	data[5] = ((P3 & 0x000F0000) >> 12) | ((P2 & 0x000F0000) >> 16);
	data[6] = (P2 & 0x0000FF00) >> 8;
	data[7] = (P2 & 0x000000FF);

	return I2cTransfer ( pll , SI5351_ADDR, sizeof(data), data, false);
}

//
// Set up MultiSynth with integer divider and R divider
// R divider is the bit value which is OR'ed onto the appropriate register, it is a #define in this file
//
CyU3PReturnStatus_t SetupMultisynth(UINT8 synth, UINT32 divider, UINT8 rDiv)
{
	UINT32 P1;	// Synth config register P1
	UINT32 P2;	// Synth config register P2
	UINT32 P3;	// Synth config register P3
	UINT8 data[8];


	P1 = 128 * divider - 512;
	P2 = 0;							// P2 = 0, P3 = 1 forces an integer value for the divider
	P3 = 1;

	data[0] = (P3 & 0x0000FF00) >> 8;
	data[1] = (P3 & 0x000000FF);
	data[2] = ((P1 & 0x00030000) >> 16) | rDiv;
	data[3] = (P1 & 0x0000FF00) >> 8;
	data[4] = (P1 & 0x000000FF);
	data[5] = ((P3 & 0x000F0000) >> 12) | ((P2 & 0x000F0000) >> 16);
	data[6] = (P2 & 0x0000FF00) >> 8;
	data[7] = (P2 & 0x000000FF);

	return I2cTransfer ( synth , SI5351_ADDR, sizeof(data), data, false);
}

/*
 * si5351_pll_locked — check whether PLL A on the Si5351 is locked.
 *
 * Reads the Si5351 device status register (register 0).  Bit 5 (LOL_A)
 * is set when PLL A has lost lock — i.e. the ADC reference clock is not
 * being generated.  This function is used by the GPIF preflight check
 * (GpifPreflightCheck in StartStopApplication.c) to prevent the GPIF
 * state machine from being started without a valid external clock.
 *
 * Returns CyTrue if PLL A is locked and the clock is presumed valid.
 * Returns CyFalse if PLL A is unlocked, the Si5351 is absent, or the
 * I2C read fails (all of which mean: don't start the GPIF).
 */
CyBool_t si5351_pll_locked(void)
{
	uint8_t status = 0xFF;  /* default to "unlocked" if I2C fails */
	CyU3PReturnStatus_t rc;

	rc = I2cTransfer(0x00, SI5351_ADDR, 1, &status, CyTrue);
	if (rc != CY_U3P_SUCCESS)
		return CyFalse;        /* I2C failed — treat as unlocked */

	/* Bit 5 = LOL_A (Loss Of Lock, PLL A).  0 = locked, 1 = unlocked. */
	return (status & 0x20) == 0;
}

/*
 * si5351_clk0_enabled — return whether CLK0 is currently powered up.
 *
 * Reads CLK0_CONTROL (register 16) bit 7 (CLK0_PDN) directly over I2C.
 * Bit 7 = 0 → clock powered up and running.  Bit 7 = 1 → powered
 * down (chip default at power-on, or after si5351aSetFrequencyA(0)).
 *
 * Authoritative: the hardware register reflects actual chip state
 * regardless of which path programmed it (firmware-side STARTADC or
 * host-side I2CWFX3).  Per docs/vendor-protocol-plan.md "Commit 1",
 * this removes the hidden coupling that made GpifPreflightCheck()
 * refuse STARTFX3 whenever a host had configured Si5351 directly.
 *
 * On I2C bus failure the function returns CyFalse — GpifPreflightCheck
 * refuses STARTFX3 safely.  No new wedge surface.
 */
CyBool_t si5351_clk0_enabled(void)
{
	uint8_t reg16 = 0xFF;  /* default = "powered down" if I2C fails */
	CyU3PReturnStatus_t rc;

	rc = I2cTransfer(SI_CLK0_CONTROL, SI5351_ADDR, 1, &reg16, CyTrue);
	if (rc != CY_U3P_SUCCESS)
		return CyFalse;

	return (reg16 & 0x80) == 0;
}

CyU3PReturnStatus_t si5351aSetFrequencyA(UINT32 freq)
{
	CyU3PReturnStatus_t status;
	UINT32 frequency;
	UINT32 pllFreq;
	UINT32 xtalFreq = SI5351_FREQ;
	UINT32 l;
	UINT8 mult;
	UINT32 num;
	UINT32 denom;
	UINT32 divider;
	UINT32 rdiv;

	if (freq == 0)
	{
		return I2cTransferW1 ( SI_CLK0_CONTROL, SI5351_ADDR, 0x80); // clk0 off (CLK0_PDN=1)
	}

	rdiv = (UINT32)SI_R_DIV_1;

	frequency = freq;

	while (frequency < 1000000)
	{
		frequency = frequency * 2;
		rdiv += SI_R_DIV_2;
	}
#ifdef _PLLDEBUG_
	DbgPrintf((char *) "\nCLK0 frequency %d \n", frequency);
#endif
	divider = 900000000UL / frequency;// Calculate the division ratio. 900,000,000 is the maximum internal
									// PLL frequency: 900MHz
	if (divider % 2) divider--;		// Ensure an even integer division ratio

	pllFreq = divider * frequency;	// Calculate the pllFrequency: the divider * desired output frequency
#ifdef _PLLDEBUG_
	DbgPrintf((char *) "pllA Freq  %d \n", pllFreq);
#endif
	mult = pllFreq / xtalFreq;		// Determine the multiplier to get to the required pllFrequency
	l = pllFreq % xtalFreq;			// It has three parts:
									// mult is an integer that must be in the range 15..90
	num = (UINT32)((uint64_t)l * 1048575 / xtalFreq);	// num and denom are the fractional parts
	denom = 1048575;				// each is 20 bits (range 0..1048575)
									// Set up PLL A with the calculated multiplication ratio
	status = SetupPLL(SI_SYNTH_PLL_A, mult, num, denom);
	if (status != CY_U3P_SUCCESS) {
		DebugPrint(4, "Si5351 SetupPLL A failed: %d", status);
		return status;
	}
	// Set up MultiSynth divider 0, with the calculated divider.
	// The final R division stage can divide by a power of two, from 1..128.
	// represented by constants SI_R_DIV1 to SI_R_DIV128 (see top of this file)
	// If you want to output frequencies below 1MHz, you have to use the
	// final R division stage
	status = SetupMultisynth(SI_SYNTH_MS_0, divider, rdiv);
	if (status != CY_U3P_SUCCESS) {
		DebugPrint(4, "Si5351 SetupMultisynth 0 failed: %d", status);
		return status;
	}
	// Reset the PLL. This causes a glitch in the output. For small changes to
	// the parameters, you don't need to reset the PLL, and there is no glitch
	status = I2cTransferW1 (SI_PLL_RESET , SI5351_ADDR, 0x20);//pllA
	if (status != CY_U3P_SUCCESS) {
		DebugPrint(4, "Si5351 PLL A reset failed: %d", status);
		return status;
	}
	// Finally switch on the CLK0 output (0x4F)
	// and set the MultiSynth0 input to be PLL A
	status = I2cTransferW1 (SI_CLK0_CONTROL, SI5351_ADDR,  0x4F | SI_CLK_SRC_PLL_A);
	if (status != CY_U3P_SUCCESS)
		DebugPrint(4, "Si5351 CLK0 control failed: %d", status);
	/* CLK0 enabled state is now queried directly from the chip via
	 * si5351_clk0_enabled() — no software-flag bookkeeping needed. */
	return status;
}

CyU3PReturnStatus_t si5351aSetFrequencyB(UINT32 freq2)
{
	CyU3PReturnStatus_t status;
	UINT32 frequency;
	UINT32 pllFreq;
	UINT32 xtalFreq = SI5351_FREQ;
	UINT32 l;
	UINT8 mult;
	UINT32 num;
	UINT32 denom;
	UINT32 divider;
	UINT32 rdiv;

	if (freq2 == 0)
	{
		return I2cTransferW1 ( SI_CLK2_CONTROL, SI5351_ADDR, 0x80); // clk2 off
	}

	// calculate clk2
	frequency = freq2 ;
	rdiv = SI_R_DIV_1;
	xtalFreq = SI5351_FREQ;
	while (frequency <= 1000000)
	{
		frequency = frequency * 2;
		rdiv += SI_R_DIV_2;
	}
#ifdef _PLLDEBUG_
	DbgPrintf((char *) "\nCLK2 frequency %d \n", frequency);
#endif
	divider = 900000000UL / frequency;// Calculate the division ratio. 900,000,000 is the maximum internal
										// PLL frequency: 900MHz
	if (divider % 2) divider--;		// Ensure an even integer division ratio

	pllFreq = divider * frequency;	// Calculate the pllFrequency: the divider * desired output frequency
#ifdef _PLLDEBUG_
	DbgPrintf((char *) "pllB Freq  %d \n", pllFreq);
#endif
	mult = pllFreq / xtalFreq;		// Determine the multiplier to get to the required pllFrequency
	l = pllFreq % xtalFreq;			// It has three parts:
									// mult is an integer that must be in the range 15..90
	num = (UINT32)((uint64_t)l * 1048575 / xtalFreq);	// num and denom are the fractional parts
	denom = 1048575;				// each is 20 bits (range 0..1048575)

									// Set up PLL B with the calculated multiplication ratio
	status = SetupPLL(SI_SYNTH_PLL_B, mult, num, denom);
	if (status != CY_U3P_SUCCESS) {
		DebugPrint(4, "Si5351 SetupPLL B failed: %d", status);
		return status;
	}
	// Set up MultiSynth divider 0, with the calculated divider.
	// The final R division stage can divide by a power of two, from 1..128.
	// represented by constants SI_R_DIV1 to SI_R_DIV128 (see top of this file)
	// If you want to output frequencies below 1MHz, you have to use the
	// final R division stage

	status = SetupMultisynth(SI_SYNTH_MS_2, divider, rdiv);
	if (status != CY_U3P_SUCCESS) {
		DebugPrint(4, "Si5351 SetupMultisynth 2 failed: %d", status);
		return status;
	}
	// Reset the PLL. This causes a glitch in the output. For small changes to
	// the parameters, you don't need to reset the PLL, and there is no glitch
	status = I2cTransferW1 ( SI_PLL_RESET, SI5351_ADDR, 0x80) ; //pllB
	if (status != CY_U3P_SUCCESS) {
		DebugPrint(4, "Si5351 PLL B reset failed: %d", status);
		return status;
	}
	// Finally switch on the CLK2 output (0x4C)
	// and set the MultiSynth0 input to be PLL A
	status = I2cTransferW1 ( SI_CLK2_CONTROL, SI5351_ADDR,  0x4C | SI_CLK_SRC_PLL_B);  // select PLLB
	if (status != CY_U3P_SUCCESS)
		DebugPrint(4, "Si5351 CLK2 control failed: %d", status);
	return status;
}

/*
 * RunApplication.c
 *
 *  Created on: 21/set/2020
 *
 *  HF103_FX3 project
 *  Author: Oscar Steila
 *  modified from: SuperSpeed Device Design By Example - John Hyde
 *
 *  https://sdr-prototypes.blogspot.com/
 */
#include "Application.h"

#include "Si5351.h"

#include "health.h"

#define R828D_I2C_ADDR		0x74

#include "radio.h"

// Declare external functions
extern void CheckStatus(char* StringPtr, CyU3PReturnStatus_t Status);
extern void CheckStatusSilent(char* StringPtr, CyU3PReturnStatus_t Status);
extern CyU3PReturnStatus_t InitializeDebugConsole(void);
extern void IndicateError(uint16_t ErrorCode);
extern CyU3PReturnStatus_t InitializeUSB(uint8_t hwconfig);
extern void ParseCommand(void);

// Declare external data
extern const char* EventName[];
extern uint32_t glDMACount;
extern uint32_t glCounter[20];
extern CyU3PDmaMultiChannel glMultiChHandleSlFifoPtoU;

// Global data owned by this module
CyBool_t glIsApplnActive = CyFalse;     // Set true once device is enumerated
CyU3PQueue glEventAvailable;			  	// Used for thread communications
uint32_t glEventAvailableQueue[16] __attribute__ ((aligned (32)));// Queue for up to 16 uint32_t pointers
uint32_t glQevent __attribute__ ((aligned (32)));
CyU3PThread glThreadHandle[APP_THREADS];		// Handles to my Application Threads
void *glStackPtr[APP_THREADS];				// Stack allocated to each thread

uint8_t glWdgMaxRecovery = WDG_MAX_RECOVERY_DEFAULT;
uint8_t glWdgRecoveryCount = 0;

uint8_t glHWconfig = NORADIO;       // Hardware config type BBRF103
uint16_t glFWconfig = (FIRMWARE_VER_MAJOR << 8) | FIRMWARE_VER_MINOR;    // Firmware rc1 ver 1.02

CyU3PReturnStatus_t
ConfGPIOSimple(uint8_t gpioid, uint8_t mode)
{
	CyU3PReturnStatus_t status;
	CyU3PGpioSimpleConfig_t gpioConfig;

	status = CyU3PDeviceGpioOverride(gpioid, CyTrue);
	CheckStatusSilent("CyU3PDeviceGpioOverride", status);

	gpioConfig.outValue    = CyFalse;
	gpioConfig.inputEn     = (mode != GPIO_OUTPUT) ? CyTrue : CyFalse;
	gpioConfig.driveLowEn  = (mode == GPIO_OUTPUT) ? CyTrue : CyFalse;
	gpioConfig.driveHighEn = (mode == GPIO_OUTPUT) ? CyTrue : CyFalse;
	gpioConfig.intrMode    = CY_U3P_GPIO_NO_INTR;

	status = CyU3PGpioSetSimpleConfig(gpioid, &gpioConfig);
	CheckStatusSilent("CyU3PGpioSetSimpleConfig", status);

	if (mode == GPIO_INPUT)
		CyU3PGpioSetIoMode(gpioid, CY_U3P_GPIO_IO_MODE_NONE);
	else if (mode == GPIO_INPUT_PU)
		CyU3PGpioSetIoMode(gpioid, CY_U3P_GPIO_IO_MODE_WPU);

	return status;
}

/* Legacy wrappers -- kept for existing call sites */
CyU3PReturnStatus_t ConfGPIOsimpleout(uint8_t gpioid)   { return ConfGPIOSimple(gpioid, GPIO_OUTPUT); }
CyU3PReturnStatus_t ConfGPIOsimpleinput(uint8_t gpioid)  { return ConfGPIOSimple(gpioid, GPIO_INPUT); }
CyU3PReturnStatus_t ConfGPIOsimpleinputPU(uint8_t gpioid) { return ConfGPIOSimple(gpioid, GPIO_INPUT_PU); }

void
GpioInitClock()
{
	CyU3PReturnStatus_t Status;
    CyU3PGpioClock_t gpioClock;

    /* Init the GPIO module */
    gpioClock.fastClkDiv = 2;
    gpioClock.slowClkDiv = 0;
    gpioClock.simpleDiv = CY_U3P_GPIO_SIMPLE_DIV_BY_2;
    gpioClock.clkSrc = CY_U3P_SYS_CLK;
    gpioClock.halfDiv = 0;
    Status = CyU3PGpioInit(&gpioClock,   NULL);
    CheckStatus("CyU3PGpioInit", Status);
}



void MsgParsing(uint32_t qevent)
{
	uint8_t label;
	label = qevent >> 24; //  label4bit|data24bit
	switch (label)
	{
		case 0:
			DebugPrint(4, "\r\nEvent received = %s   \r\n", EventName[(uint8_t)qevent]);
			break;
		case 1:
			DebugPrint(4, "\r\nVendor request = %x  %x  %x\r\n", (uint8_t)( qevent >> 16), (uint8_t) (qevent >> 8) , (uint8_t) qevent );
			break;
		case 2:
			DebugPrint(4, "\r\nPIB error 0x%x\r\n", (uint16_t) qevent);
			break;
		case USER_COMMAND_AVAILABLE:
			ParseCommand();
			break;
	}
}

void ApplicationThread ( uint32_t input)
{
	// input is passed to this routine from CyU3PThreadCreate, useful if the same code is used for multiple threads
#ifndef _DEBUG_USB_
	int32_t Seconds = 0;  // second count in serial debug
#endif
	uint32_t nline;
	CyBool_t measure;
    CyU3PReturnStatus_t Status;
    glHWconfig = 0;

    GpioInitClock();

	DebugPrint(4, "Detect Hardware");
    Status = I2cInit ();
    if (Status != CY_U3P_SUCCESS)
    	DebugPrint(4, "I2cInit failed to initialize. Error code: %d.", Status);
	else
	{
		Status = Si5351Init();
		if (Status != CY_U3P_SUCCESS)
		{
			DebugPrint(4, "Si5351Init failed to initialize. Error code: %d.", Status);
		}
		else
		{
			ConfGPIOsimpleinputPU(GPIO36);

			Status = si5351aSetFrequencyB(16000000);
			if (Status != CY_U3P_SUCCESS)
				DebugPrint(4, "si5351aSetFrequencyB(16MHz) failed: %d.", Status);
			uint8_t identity;
			if (I2cTransfer(0, R828D_I2C_ADDR, 1, &identity, true) == CY_U3P_SUCCESS)
			{
				CyU3PGpioSimpleGetValue ( GPIO36, &measure);

				if (!measure)
				{
					glHWconfig = RX888r2;
					DebugPrint(4, "R828D detected. RX888r2 detected.");
				}
				else
				{
					glHWconfig = NORADIO;
					DebugPrint(4, "R828D detected but GPIO36 sense failed.");
				}
			}
			else
			{
				glHWconfig = NORADIO;
				DebugPrint(4, "No R828D tuner detected.");
			}
			Status = si5351aSetFrequencyB(0);
			if (Status != CY_U3P_SUCCESS)
				DebugPrint(4, "si5351aSetFrequencyB(0) failed: %d.", Status);
		}
	}
    DebugPrint(4, "HWconfig: %d.", glHWconfig);

	if (glHWconfig == RX888r2)
	{
		rx888r2_GpioInitialize();
	}

    // Spin up the USB Connection
	Status = InitializeUSB(glHWconfig);
	CheckStatus("Initialize USB", Status);
	if (Status == CY_U3P_SUCCESS)
	    {
	    	DebugPrint(4, "\r\nApplication started with %d", input);
			// Wait for the device to be enumerated
	    	while (!glIsApplnActive)
			{
				// Check for USB CallBack Events every 100msec
	    		CyU3PThreadSleep(100);
				health_pet();  /* Level 5 HWDT — main thread is alive */
				while( CyU3PQueueReceive(&glEventAvailable, &glQevent, CYU3P_NO_WAIT)== 0)
					{
						MsgParsing(glQevent);
					}
			}

	    	// Now run forever
			DebugPrint(4, "\r\nMAIN now running forever: ");
			while(1)
			{
				// Check for User Commands (and other CallBack Events) every 100msec
				CyU3PThreadSleep(100);
				health_pet();  /* Level 5 HWDT — main thread is alive */
				nline =0;
				while( CyU3PQueueReceive(&glEventAvailable, &glQevent, CYU3P_NO_WAIT)== 0)
				{
					if (nline++ == 0) DebugPrint(4, "\r\n"); //first line
					MsgParsing(glQevent);
				}
				/* GPIF watchdog: detect and recover from DMA stalls.
				 * If glDMACount hasn't advanced for 3 consecutive 100ms
				 * polls while the GPIF SM is in a BUSY/WAIT state, the
				 * streaming pipeline is wedged.  Tear it down and rebuild. */
				if (glIsApplnActive)
				{
					static uint32_t prevDMACount = 0;
					static uint8_t  stallCount = 0;

					uint32_t curDMA = glDMACount;
					if (curDMA == prevDMACount && curDMA > 0)
					{
						uint8_t gpifState = 0xFF;
						CyU3PGpifGetSMState(&gpifState);

						if (gpifState == 5 || gpifState == 7 ||
						    gpifState == 8 || gpifState == 9)
						{
							stallCount++;
							DebugPrint(4, "\r\nWDG: stall %d/3 SM=%d DMA=%u",
								stallCount, gpifState, curDMA);
							if (stallCount >= 3)  /* 300ms in BUSY/WAIT */
							{
								/* Check recovery cap before attempting */
								if (glWdgMaxRecovery > 0 &&
								    glWdgRecoveryCount >= glWdgMaxRecovery)
								{
									DebugPrint(4, "\r\nWDG: recovery limit (%d), waiting for STARTFX3",
										glWdgMaxRecovery);
									stallCount = 0;
								}
								else
								{
									CyU3PReturnStatus_t rc_reset, rc_flush;
									CyU3PReturnStatus_t rc_xfer = 0, rc_sm = 0;
									CyBool_t hw_ok;

									glWdgRecoveryCount++;
									DebugPrint(4, "\r\nWDG: === RECOVERY START === SM=%d DMA=%u recov=%d/%d",
										gpifState, curDMA, glWdgRecoveryCount, glWdgMaxRecovery);
									/* Soft-stop: deassert FW_TRG so the SM exits
									 * to IDLE via !FW_TRG transitions.
									 * REQUIRES updated SDDC_GPIF.h waveform with
									 * !FW_TRG exits on TH0_RD, TH0_WAIT, TH1_WAIT.
									 *
									 * Caveat: soft-stop needs the external clock
									 * to advance the SM.  If the Si5351 is dead
									 * or PLL unlocked, the SM can't transition
									 * and soft-stop fails — fall back to force. */
									CyU3PGpifControlSWInput(CyFalse);
									CyU3PThreadSleep(1);
									{
										uint8_t smState = 0xFF;
										CyU3PGpifGetSMState(&smState);
										if (smState == 1 /* IDLE */) {
											CyU3PGpifDisable(CyFalse);
										} else {
											DebugPrint(4, "\r\nWDG: soft-stop fail SM=%d, forcing", smState);
											CyU3PGpifDisable(CyTrue);
										}
									}

									rc_reset = CyU3PDmaMultiChannelReset(&glMultiChHandleSlFifoPtoU);
									rc_flush = CyU3PUsbFlushEp(CY_FX_EP_CONSUMER);

									hw_ok = si5351_clk0_enabled() && si5351_pll_locked();
									if (hw_ok)
									{
										rc_xfer = CyU3PDmaMultiChannelSetXfer(
											&glMultiChHandleSlFifoPtoU, FIFO_DMA_RX_SIZE, 0);
										rc_sm = CyU3PGpifSMStart(0, 0);
										CyU3PGpifControlSWInput(CyTrue);
									}
									glCounter[2]++;  /* streaming fault counter —
									                  * GETSTATS [15..18]; also incremented
									                  * by EP_UNDERRUN in USBHandler.c */
									DebugPrint(4, "\r\nWDG: === RECOVERY %s === rst=%d flush=%d xfer=%d sm=%d",
										hw_ok ? "DONE" : "WAIT",
										rc_reset, rc_flush, rc_xfer, rc_sm);
									stallCount = 0;
									prevDMACount = 0;
									glDMACount = 0;
								}
							}
						}
						else
						{
							if (stallCount > 0)
								DebugPrint(4, "\r\nWDG: stall cleared SM=%d (was %d/3)",
									gpifState, stallCount);
							stallCount = 0;
						}
					}
					else
					{
						if (stallCount > 0)
							DebugPrint(4, "\r\nWDG: DMA resumed (%u->%u), stall cleared",
								prevDMACount, curDMA);
						stallCount = 0;
						prevDMACount = curDMA;
					}
				}

				/* Health watchdog (Level 4 backstop, issues #104, #105).
				 * Evaluated outside the glIsApplnActive check so EP0
				 * hangs during enumeration are caught too.  On
				 * HEALTH_WEDGED_EP0, health_recover() calls
				 * CyU3PDeviceReset() — that does not return. */
				{
					health_status_t hs = health_evaluate();
					if (hs != HEALTH_OK)
					{
						DebugPrint(4, "\r\nHEALTH: status=%d, recovering", hs);
						health_recover(hs);
					}
				}

#ifndef _DEBUG_USB_  // second count in serial debug
				if (glDMACount > 7812)
				{
					glDMACount -= 7812;
					DebugPrint(4, "%d, \n", Seconds++);
				}
#endif
			}			
	    }
	 DebugPrint(4, "\r\nApplication failed to initialize. Error code: %d.\r\n", Status);
	 	 // Returning here will stop the Application Thread running - it failed anyway so this is OK
}

/* Application define function which creates the threads. */
void CyFxApplicationDefine (void) {

    uint32_t Status;

    // If I get here then RTOS has started correctly, turn off ErrorIndicator
	IndicateError(0);

    // Create any needed resources then the Application thread
    Status = InitializeDebugConsole();
    CheckStatus("Initialize Debug Console", Status);

    // Initialize the recovery state machine (no-op for events not yet wired).
    health_init();

    // Create Queue used to transfer callback messages
        Status = CyU3PQueueCreate(&glEventAvailable, 1, &glEventAvailableQueue, sizeof(glEventAvailableQueue));
        CheckStatus("Create EventAvailableQueue", Status);


    glStackPtr[0] = CyU3PMemAlloc (FIFO_THREAD_STACK);
    Status = CyU3PThreadCreate (&glThreadHandle[0],                  /* Slave FIFO app thread structure */
                          "11:HF103_ADC2USB30",                     /* Thread ID and thread name */
                          ApplicationThread,                   /* Slave FIFO app thread entry function */
                          0,                                       /* No input parameter to thread */
                          glStackPtr[0],                                     /* Pointer to the allocated thread stack */
                          FIFO_THREAD_STACK,               /* App Thread stack size */
                          FIFO_THREAD_PRIORITY,            /* App Thread priority */
                          FIFO_THREAD_PRIORITY,            /* App Thread pre-emption threshold */
                          CYU3P_NO_TIME_SLICE,                     /* No time slice for the application thread */
                          CYU3P_AUTO_START                         /* Start the thread immediately */
                          );

    /* Check the return code*/
    CheckStatus("CyFxApplicationDefine",Status);
    if (Status != 0)
    	while(1); // Application cannot continue. Loop indefinitely
}


/*
 * USB_handler.c
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

#include "radio.h"

#include "health.h"
#include "synth_pps.h"

// Declare external functions
extern void CheckStatus(char* StringPtr, CyU3PReturnStatus_t Status);
extern void StartApplication(void);
extern void StopApplication(void);
extern CyU3PReturnStatus_t StartGPIF(void);
extern CyBool_t GpifPreflightCheck(void);  /* StartStopApplication.c */
extern const CyU3PGpifConfig_t CyFxGpifConfig;
extern CyU3PReturnStatus_t SetUSBdescriptors(uint8_t hwconfig);

// Declare external data
extern CyU3PQueue glEventAvailable;			  	// Used for threads communications
extern CyBool_t glIsApplnActive;				// Set true once device is enumerated
extern uint8_t  glHWconfig;       			    // Hardware config
extern uint16_t  glFWconfig;       			    // Firmware config hb.lb

extern CyBool_t glFlagDebug;
extern volatile uint16_t glDebTxtLen;
extern uint8_t glBufDebug[MAXLEN_D_USB];
extern uint32_t glCounter[20];
extern uint16_t glLastPibArg;
extern uint32_t glDMACount;
extern uint8_t glWdgMaxRecovery;
extern uint8_t glWdgRecoveryCount;



#define CYFX_SDRAPP_MAX_EP0LEN  64      /* Max. data length supported for EP0 requests. */

extern CyU3PDmaMultiChannel glMultiChHandleSlFifoPtoU;
// Global data owned by this module
static uint8_t  *glEp0Buffer = 0;       /* Buffer used to handle vendor specific control requests. */
static uint8_t  glVendorRqtCnt = 0;

#ifdef TRACESERIAL

extern CyU3PQueue glEventAvailable;	  	// Used for threads communications
extern uint32_t glQevent __attribute__ ((aligned (32)));
extern void ConsoleAccumulateChar(char ch);

/* Trace function */
void
TraceSerial( uint8_t  bRequest, uint8_t * pdata, uint16_t wValue, uint16_t wIndex)
{
	if ( bRequest != READINFODEBUG)
	{
		if (bRequest >= FX3_CMD_BASE && (bRequest - FX3_CMD_BASE) < FX3_CMD_COUNT)
			DebugPrint(4, "%s\t", FX3CommandName[bRequest - FX3_CMD_BASE]);
		else
			DebugPrint(4, "0x%x\t", bRequest);
		switch(bRequest)
		{
		case SETARGFX3:
			if (wIndex < SETARGFX3_LIST_COUNT)
				DebugPrint(4, "%s\t%d", SETARGFX3List[wIndex],  wValue );
			else
				DebugPrint(4, "%d\t%d", wIndex, wValue);
			break;

		case GPIOFX3:
			{
				uint32_t val; memcpy(&val, pdata, 4);
				DebugPrint(4, "\t0x%x", val);
			}
			break;

		case STARTADC:
			{
				uint32_t val; memcpy(&val, pdata, 4);
				DebugPrint(4, "%d", val);
			}
			break;

		case STARTFX3:
		case STOPFX3:
		case RESETFX3:
			break;

		default:
			DebugPrint(4, "0x%x\t0x%x", pdata[0] , pdata[1]);
			break;
		}
		DebugPrint(4, "\r\n\n");
	}
}
#endif

/* USB driver thread context — blocking calls (e.g. CyU3PThreadSleep) are safe. */
CyBool_t
CyFxSlFifoApplnUSBSetupCB (
        uint32_t setupdat0,
        uint32_t setupdat1
    )
{
    /* Fast enumeration is used. Only requests addressed to the interface, class,
     * vendor and unknown control requests are received by this function.
     * This application does not support any class or vendor requests. */

    uint8_t  bRequest, bReqType;
    uint8_t  bType, bTarget;
    uint16_t wValue, wIndex;
    uint16_t wLength;
    CyBool_t isHandled = CyFalse;
    CyU3PReturnStatus_t apiRetStatus;

    /* Liveness: record callback entry for the health watchdog.  Matched
     * by HEALTH_EVENT_EP0_HANDLER_EXIT at every return path below. */
    health_record_event(HEALTH_EVENT_EP0_HANDLER_ENTER);

    /* Decode the fields from the setup request. */
    bReqType = (setupdat0 & CY_U3P_USB_REQUEST_TYPE_MASK);
    bType    = (bReqType & CY_U3P_USB_TYPE_MASK);
    bTarget  = (bReqType & CY_U3P_USB_TARGET_MASK);
    bRequest = ((setupdat0 & CY_U3P_USB_REQUEST_MASK) >> CY_U3P_USB_REQUEST_POS);
    wValue   = ((setupdat0 & CY_U3P_USB_VALUE_MASK)   >> CY_U3P_USB_VALUE_POS);
    wIndex   = ((setupdat1 & CY_U3P_USB_INDEX_MASK)   >> CY_U3P_USB_INDEX_POS);
    wLength   = ((setupdat1 & CY_U3P_USB_LENGTH_MASK)   >> CY_U3P_USB_LENGTH_POS);

    if (bType == CY_U3P_USB_STANDARD_RQT)
    {
        /* Handle SET_FEATURE(FUNCTION_SUSPEND) and CLEAR_FEATURE(FUNCTION_SUSPEND)
         * requests here. It should be allowed to pass if the device is in configured
         * state and failed otherwise. */
        if ((bTarget == CY_U3P_USB_TARGET_INTF) && ((bRequest == CY_U3P_USB_SC_SET_FEATURE)
                    || (bRequest == CY_U3P_USB_SC_CLEAR_FEATURE)) && (wValue == 0))
        {
            if (glIsApplnActive)
                CyU3PUsbAckSetup ();
            else
            {
                CyU3PUsbStall (0, CyTrue, CyFalse);
            }
            isHandled = CyTrue;
        }

        /* CLEAR_FEATURE(ENDPOINT_HALT) — clear stall/toggle, then flush.
         *
         * This path fires when the host sends libusb_clear_halt() at
         * device-open time to restart the XHCI endpoint ring.
         *
         * CyU3PUsbStall(ep, CyFalse, CyTrue) is required — without it
         * (ACK-only) the USB controller never re-arms the endpoint and
         * all bulk transfers fail with IO errors.  However, on a
         * non-stalled endpoint it has side-effects that corrupt
         * internal USB controller state across repeated open/close
         * cycles.  CyU3PUsbFlushEp immediately after clears any stale
         * FIFO state left by the stall-clear, making the endpoint
         * ready for the next STARTFX3.
         *
         * Do NOT add CyU3PUsbResetEp or CyU3PDmaMultiChannelReset
         * here — those desync the data toggle or corrupt the DMA
         * pipeline (see STARTFX3 comment block). */
        if ((bTarget == CY_U3P_USB_TARGET_ENDPT) && (bRequest == CY_U3P_USB_SC_CLEAR_FEATURE)
                && (wValue == CY_U3P_USBX_FS_EP_HALT))
        {
            if (glIsApplnActive)
            {
                CyU3PUsbStall (wIndex, CyFalse, CyTrue);
                CyU3PUsbFlushEp (CY_FX_EP_CONSUMER);
                isHandled = CyTrue;
            }
        }
    } else if (bType == CY_U3P_USB_VENDOR_RQT) {

    	isHandled = CyFalse;

    	/* Reject oversized EP0 data before any GetEP0Data call. */
    	if (wLength > CYFX_SDRAPP_MAX_EP0LEN) {
    		CyU3PUsbStall(0, CyTrue, CyFalse);
    		/* Match the ENTER recorded at the top of this function so the
    		 * health watchdog doesn't see an apparent stuck handler. */
    		health_record_event(HEALTH_EVENT_EP0_HANDLER_EXIT);
    		return CyTrue;
    	}

    	switch (bRequest)
    	 {
			case GPIOFX3:
					if(CyU3PUsbGetEP0Data(wLength, glEp0Buffer, NULL)== CY_U3P_SUCCESS)
					{
						uint32_t mdata; memcpy(&mdata, glEp0Buffer, 4);
						rx888r2_GpioSet(mdata);
						isHandled = CyTrue;
					}
					break;

			case STARTADC:
					if(CyU3PUsbGetEP0Data(wLength, glEp0Buffer, NULL)== CY_U3P_SUCCESS)
					{
						uint32_t freq;
						memcpy(&freq, glEp0Buffer, 4);
						/* If the GPIF SM is actively streaming, force-stop
						 * it before reprogramming the Si5351 ADC clock.
						 * Changing the clock while the SM is running will
						 * wedge the PIB (no clock edges for soft-stop).
						 * Host should send STOPFX3 first; this is a safety
						 * net against a clock-pull wedge if it doesn't. */
						{
							uint8_t smState = 0xFF;
							CyU3PGpifGetSMState(&smState);
							if (smState != 0 && smState != 0xFF) {
								DebugPrint(4, "\r\nSTARTADC: implicit GPIF stop (SM=%d)", smState);
								CyU3PGpifControlSWInput(CyFalse);
								CyU3PGpifDisable(CyTrue);
								CyU3PDmaMultiChannelReset(&glMultiChHandleSlFifoPtoU);
								CyU3PUsbFlushEp(CY_FX_EP_CONSUMER);
							}
						}
						apiRetStatus = si5351aSetFrequencyA(freq);
						if (apiRetStatus == CY_U3P_SUCCESS) {
							/* Poll PLL lock — typically <10 ms for Si5351.
							 * Max 100 iterations x 1 ms = 100 ms worst-case.
							 * This keeps the USB thread unblocked so STARTFX3
							 * arriving shortly after STARTADC is not delayed.
							 *
							 * We use si5351_pll_locked() rather than
							 * GpifPreflightCheck() because we just called
							 * si5351aSetFrequencyA(freq) with freq > 0, so
							 * the chip's CLK0_CONTROL bit 7 is already
							 * cleared — the extra clk0_enabled check would
							 * be redundant. */
							{
								int i;
								for (i = 0; i < 100; i++) {
									CyU3PThreadSleep(1);
									if (si5351_pll_locked()) break;
								}
							}
							isHandled = CyTrue;
						} else {
							DebugPrint(4, "STARTADC si5351 failed: %d", apiRetStatus);
							isHandled = CyFalse;
						}
					}
					break;

			case GETSTATS:
				{
					CyU3PMemSet(glEp0Buffer, 0, CYFX_SDRAPP_MAX_EP0LEN);
					uint8_t gpifState = 0xFF;
					CyU3PGpifGetSMState(&gpifState);
					uint16_t off = 0;
					memcpy(&glEp0Buffer[off], &glDMACount, 4);   off += 4;
					glEp0Buffer[off++] = gpifState;
					memcpy(&glEp0Buffer[off], &glCounter[0], 4); off += 4;
					memcpy(&glEp0Buffer[off], &glLastPibArg, 2); off += 2;
					memcpy(&glEp0Buffer[off], &glCounter[1], 4); off += 4;
					memcpy(&glEp0Buffer[off], &glCounter[2], 4); off += 4;
					{
						uint8_t si_status = 0;
						I2cTransfer(0x00, 0xC0, 1, &si_status, CyTrue); /* Si5351 reg 0 */
						glEp0Buffer[off++] = si_status;                  /* [19] */
					}
					{
						uint32_t boot_count = health_boot_count();       /* [20..23] */
						memcpy(&glEp0Buffer[off], &boot_count, 4); off += 4;
					}
					{
						/* CLK0 diagnostic — exposes both the raw register
						 * byte that si5351_clk0_enabled() reads and the
						 * boolean it returns.  Lets the host see Si5351's
						 * actual CLK0 power state and what
						 * GpifPreflightCheck() will observe. */
						uint8_t clk0_reg16 = 0xFF;
						I2cTransfer(SI_CLK0_CONTROL, 0xC0, 1, &clk0_reg16, CyTrue);
						glEp0Buffer[off++] = clk0_reg16;                 /* [24] */
						glEp0Buffer[off++] = si5351_clk0_enabled() ? 1 : 0; /* [25] */
					}
					/* Synthetic-PPS diagnostic counters (issue #125).
					 * Hosts that request wLength=26 keep working — the
					 * USB block returns only the requested prefix. */
					memcpy(&glEp0Buffer[off], &glPpsCount, 4);          /* [26..29] */
					off += 4;
					memcpy(&glEp0Buffer[off], &glPpsCommitFailCount, 4); /* [30..33] */
					off += 4;
					CyU3PUsbSendEP0Data(off, glEp0Buffer);
					isHandled = CyTrue;
				}
				break;

		case I2CWFX3:
					apiRetStatus  = CyU3PUsbGetEP0Data(wLength, glEp0Buffer, NULL);
					if (apiRetStatus == CY_U3P_SUCCESS)
						{
							apiRetStatus = I2cTransfer ( wIndex, wValue, wLength, glEp0Buffer, CyFalse);
							if (apiRetStatus == CY_U3P_SUCCESS)
								isHandled = CyTrue;
							else
							{
								CyU3PDebugPrint (4, "I2cwrite Error %d\n", apiRetStatus);
								isHandled = CyFalse;
							}
						}
					break;

			case I2CRFX3:
					CyU3PMemSet (glEp0Buffer, 0, CYFX_SDRAPP_MAX_EP0LEN);
					apiRetStatus = I2cTransfer (wIndex, wValue, wLength, glEp0Buffer, CyTrue);
					if (apiRetStatus == CY_U3P_SUCCESS)
					{
						apiRetStatus = CyU3PUsbSendEP0Data(wLength, glEp0Buffer);
						isHandled = CyTrue;
					}
					break;
			case SETARGFX3:
					CyU3PUsbGetEP0Data(wLength, glEp0Buffer, NULL);
					switch(wIndex) {
						case DAT31_ATT:
							rx888r2_SetAttenuator(wValue);
							glVendorRqtCnt++;
							isHandled = CyTrue;
							break;
						case AD8370_VGA:
							rx888r2_SetGain(wValue);
							glVendorRqtCnt++;
							isHandled = CyTrue;
							break;
						case WDG_MAX_RECOV:
							glWdgMaxRecovery = (uint8_t)(wValue & 0xFF);
							glVendorRqtCnt++;
							isHandled = CyTrue;
							break;
						default:
							/* Data phase already ACKed; stall status to
							   signal the unrecognized wIndex to the host. */
							CyU3PUsbStall(0, CyTrue, CyFalse);
							isHandled = CyTrue;
							break;
					}
				break;

    	 	case STARTFX3:
					CyU3PUsbLPMDisable();
    	 		    CyU3PUsbGetEP0Data(wLength, glEp0Buffer, NULL);
				/*
				 * Preflight: verify the ADC clock is running before
				 * starting the GPIF state machine.  The SM is clocked
				 * by the Si5351 output; without it the SM will wedge
				 * in a read state with no timeout-based recovery.
				 * See GpifPreflightCheck() in StartStopApplication.c
				 * and si5351_pll_locked() in Si5351.c for details.
				 */
				if (!GpifPreflightCheck()) {
					/* Data phase already ACKed by GetEP0Data;
					 * stall status phase so host sees rejection. */
					CyU3PUsbStall(0, CyTrue, CyFalse);
					isHandled = CyTrue;
					break;
				}
				/* Stop any running SM before restart.  Always use
				 * CyTrue (force-reload) here because StartGPIF()
				 * calls CyU3PGpifLoad() which reloads the config
				 * anyway — CyFalse offers no benefit and crashed
				 * the device when the SM wasn't in IDLE (no sleep
				 * in this path, unlike STOPFX3). */
				CyU3PGpifControlSWInput(CyFalse);
				CyU3PGpifDisable(CyTrue);
				CyU3PDmaMultiChannelReset (&glMultiChHandleSlFifoPtoU);
				CyU3PUsbFlushEp(CY_FX_EP_CONSUMER);  /* reclaim USB-side DMA descriptors
				    * left by the previous session; without this, zombie descriptors
				    * accumulate across rapid stop/start cycles until the USB controller's
				    * descriptor pool is exhausted and EP0 locks up. */
				/* DO NOT call CyU3PUsbResetEp() here.  It resets the FX3-side
				 * data toggle / sequence number without the host knowing, which
				 * desyncs the endpoint and silently kills all subsequent bulk
				 * transfers (dma_completions=0, PIB errors climb, GPIF stuck
				 * at 255).  The host-side libusb_clear_halt() at device open
				 * already handles the one-time toggle sync.  This call was
				 * added in 6b35bcc, reverted in 86a7e26, re-added in 2482476,
				 * and broke streaming every time.  See soak_test evidence:
				 * 1a97a30 (without) = 60 MiB/s; 2482476 (with) = 0 bytes.
				 * If you think you need this back, you must first identify
				 * what is actually wrong — the symptom it masked was an
				 * xHCI endpoint-ring issue fixed host-side by clear_halt. */
				glDMACount = 0;  /* reset so watchdog doesn't false-positive during GPIF bring-up */
				glWdgRecoveryCount = 0;  /* new session — reset recovery cap */
				apiRetStatus = CyU3PDmaMultiChannelSetXfer (&glMultiChHandleSlFifoPtoU, FIFO_DMA_RX_SIZE, 0);
				if (apiRetStatus == CY_U3P_SUCCESS) {
					apiRetStatus = StartGPIF();  /* reload waveform + SMStart */
					if (apiRetStatus == CY_U3P_SUCCESS) {
						CyU3PGpifControlSWInput(CyTrue);
					}
				}
				if (apiRetStatus != CY_U3P_SUCCESS) {
					/* DMA or GPIF setup failed — report to host and
					 * ensure EP0 is left in a clean state. */
					DebugPrint(4, "\r\nSTARTFX3 fail: %d", apiRetStatus);
					CyU3PUsbStall(0, CyTrue, CyFalse);
				}
				isHandled = CyTrue;  /* always — GetEP0Data already consumed data phase */
				{ uint8_t _s=0xFF; CyU3PGpifGetSMState(&_s);
				  DebugPrint(4,"\r\nGO s=%d r=%d",_s,apiRetStatus); }
				break;

			case STOPFX3:
					CyU3PUsbLPMEnable();
				    CyU3PUsbGetEP0Data(wLength, glEp0Buffer, NULL);
					/* Soft-stop: deassert FW_TRG so the GPIF SM exits to IDLE
					 * via the !FW_TRG transitions added in SDDC_GPIF.h.
					 * REQUIRES the updated waveform with !FW_TRG exits on
					 * TH0_RD, TH0_WAIT, and TH1_WAIT states.  Without
					 * the new waveform, the SM will hang in WAIT states
					 * and the soft-stop below will fail. */
					CyU3PGpifControlSWInput(CyFalse);
					CyU3PThreadSleep(1);  /* SM reaches IDLE within 3 clocks;
					                       * sleep 1 ms for DMA quiesce */
					{
						uint8_t smState = 0xFF;
						CyU3PGpifGetSMState(&smState);
						if (smState == 1 /* IDLE */) {
							CyU3PGpifDisable(CyFalse);
						} else {
							DebugPrint(4, "\r\nSTP soft-stop fail SM=%d, forcing", smState);
							CyU3PGpifDisable(CyTrue);
						}
					}
					CyU3PDmaMultiChannelReset (&glMultiChHandleSlFifoPtoU);
					CyU3PUsbFlushEp(CY_FX_EP_CONSUMER);
					glDMACount = 0;  /* prevent watchdog false-positive on stale count */
					glWdgRecoveryCount = 0;  /* reset recovery cap */
					{ uint8_t _s=0xFF; CyU3PGpifGetSMState(&_s);
					  DebugPrint(4,"\r\nSTP s=%d",_s); }
					isHandled = CyTrue;
					break;

			case RESETFX3:	// RESETTING CPU BY PC APPLICATION
				    CyU3PUsbGetEP0Data(wLength, glEp0Buffer, NULL);
					DebugPrint(4, "\r\n\r\nHOST RESETTING CPU \r\n");
					CyU3PThreadSleep(100);
					CyU3PDeviceReset(CyFalse);
					break;

            		case TESTFX3:
					glEp0Buffer[0] =  glHWconfig;
					glEp0Buffer[1] = (uint8_t) (glFWconfig >> 8);
					glEp0Buffer[2] = (uint8_t) glFWconfig;
					glEp0Buffer[3] = glVendorRqtCnt;
					CyU3PUsbSendEP0Data (4, glEp0Buffer);
					glFlagDebug = (wValue == 1); // debug mode
					glVendorRqtCnt++;
					isHandled = CyTrue;
					break;

				/* TEST-ONLY: deterministically wedge the EP0 handler by
				 * sleeping wValue milliseconds in this thread context.
				 * The health watchdog (see RunApplication.c + health.c)
				 * should detect the hang at EP0_HANDLER_TIMEOUT_MS and
				 * call CyU3PDeviceReset(CyFalse).  Used by the host-side
				 * test_health_recovery scenario to validate Level-4
				 * recovery end-to-end.  Issues #104, #105. */
				case HANGFX3:
					{
						uint32_t hang_ms = (uint32_t)wValue;
						DebugPrint(4, "\r\nHANGFX3: sleeping %u ms (test-only)", hang_ms);
						CyU3PThreadSleep(hang_ms);
						/* If we reach here the watchdog did NOT fire — wValue
						 * was shorter than the timeout.  Acknowledge so the
						 * host doesn't see a STALL. */
						CyU3PUsbAckSetup();
						isHandled = CyTrue;
					}
					break;

				/* TEST-ONLY: signal the main thread to enter an infinite
				 * spin on its next iteration.  ACK before the main thread
				 * notices (it's currently in CyU3PThreadSleep) so the
				 * host sees a clean reply.  Main loop checks the flag at
				 * the top of its iteration; spin freezes the heartbeat;
				 * the WD-clear timer callback stops petting; HWDT fires
				 * after HWDT_PERIOD_MS and resets the device.  Used by
				 * the host-side test_main_recovery scenario to validate
				 * Level-5 end-to-end. */
				case HANGMAIN:
					DebugPrint(4, "\r\nHANGMAIN: arming main-loop hang (test-only)");
					glHealthHangMain = 1;
					CyU3PUsbAckSetup();
					isHandled = CyTrue;
					break;

				/* DIAGNOSTIC: software-driven in-band PPS marker (issue #125).
				 * Dispatches to synth_pps_{stop,start,oneshot}().  The
				 * argument-validation matrix here (wValue 0..2, wIndex
				 * 10..60000 ms with 0 -> default 1000 ms) is the user-
				 * facing contract; SDK-level failures from synth_pps_*
				 * cause a STALL to keep the host informed. */
				case SYNTH_PPS:
					switch (wValue) {
						case 0: /* stop */
							DebugPrint(4, "\r\nSYNTH_PPS\tstop");
							if (synth_pps_stop() == CY_U3P_SUCCESS) {
								CyU3PUsbAckSetup();
							} else {
								CyU3PUsbStall(0, CyTrue, CyFalse);
							}
							isHandled = CyTrue;
							break;
						case 1: /* start */
							{
								uint16_t period_ms = (wIndex == 0) ? 1000 : wIndex;
								if (period_ms >= 10 && period_ms <= 60000) {
									DebugPrint(4, "\r\nSYNTH_PPS\tstart period=%ums", period_ms);
									if (synth_pps_start(period_ms) == CY_U3P_SUCCESS) {
										CyU3PUsbAckSetup();
									} else {
										CyU3PUsbStall(0, CyTrue, CyFalse);
									}
								} else {
									DebugPrint(4, "\r\nSYNTH_PPS\tSTALL period=%u out of range", period_ms);
									CyU3PUsbStall(0, CyTrue, CyFalse);
								}
								isHandled = CyTrue;
							}
							break;
						case 2: /* oneshot */
							DebugPrint(4, "\r\nSYNTH_PPS\toneshot");
							/* oneshot returns CY_U3P_ERROR_NOT_STARTED if streaming
							 * is idle.  We still ACK that case — the host issued a
							 * valid request and the firmware noted it; the absence
							 * of a commit shows up as glPpsCommitFailCount in
							 * GETSTATS rather than as an EP0 STALL. */
							(void)synth_pps_oneshot();
							CyU3PUsbAckSetup();
							isHandled = CyTrue;
							break;
						default:
							DebugPrint(4, "\r\nSYNTH_PPS\tSTALL action=%u", wValue);
							CyU3PUsbStall(0, CyTrue, CyFalse);
							isHandled = CyTrue;
							break;
					}
					break;


   case READINFODEBUG:
					{
					if (wValue >0)
					{
						char InputChar = (char) wValue;
					 	if (InputChar  == 0x0d)
						{
							glQevent = USER_COMMAND_AVAILABLE << 24;
							CyU3PQueueSend(&glEventAvailable, &glQevent, CYU3P_NO_WAIT);
						}
						else
						{
							ConsoleAccumulateChar(InputChar);
						}
					}
					if (glDebTxtLen > 0)
						{
							uint32_t intMask = CyU3PVicDisableAllInterrupts();
							uint16_t len = glDebTxtLen;
							if (len > CYFX_SDRAPP_MAX_EP0LEN - 1)
								len = CYFX_SDRAPP_MAX_EP0LEN - 1;
							memcpy(glEp0Buffer, glBufDebug, len);
							uint16_t remain = glDebTxtLen - len;
							if (remain > 0)
								memmove(glBufDebug, glBufDebug + len, remain);
							glDebTxtLen = remain;
							CyU3PVicEnableInterrupts(intMask);
							glEp0Buffer[len] = '\0';
							CyU3PUsbSendEP0Data (len + 1, glEp0Buffer);
							glVendorRqtCnt++;
							isHandled = CyTrue;
						}
					else
						{
							isHandled = CyTrue;
							CyU3PUsbStall (0, CyTrue, CyFalse);
						}
					}

					break;

            default: /* unknown request, stall the endpoint. */

					isHandled = CyFalse;
					CyU3PDebugPrint (4, "STALL EP0 V.REQ %x\n",bRequest);
					CyU3PUsbStall (0, CyTrue, CyFalse);
					break;
    	}
    	TraceSerial( bRequest, (uint8_t *) &glEp0Buffer[0], wValue, wIndex);
    }
    /* Liveness: record callback exit so the health watchdog stops the
     * timeout window started at HEALTH_EVENT_EP0_HANDLER_ENTER. */
    health_record_event(HEALTH_EVENT_EP0_HANDLER_EXIT);
    return isHandled;
}


/* USB driver thread context — blocking calls are safe. */
void USBEventCallback ( CyU3PUsbEventType_t evtype, uint16_t evdata)
{
	uint32_t event = evtype;
	CyU3PQueueSend(&glEventAvailable, &event, CYU3P_NO_WAIT);
    switch (evtype)
    {
        case CY_U3P_USB_EVENT_SETCONF:
            /* Stop the application before re-starting. */
            if (glIsApplnActive) StopApplication ();
            	StartApplication ();
            break;

        case CY_U3P_USB_EVENT_CONNECT:
        	break;
        case CY_U3P_USB_EVENT_RESET:
        case CY_U3P_USB_EVENT_DISCONNECT:
            if (glIsApplnActive)
            {
            	StopApplication ();
              	CyU3PUsbLPMEnable ();
            }
            break;

        case CY_U3P_USB_EVENT_EP_UNDERRUN:
        	glCounter[2]++;
        	DebugPrint (4, "\r\nEP Underrun on %d", evdata);
            break;

        case CY_U3P_USB_EVENT_EP0_STAT_CPLT:
               /* Make sure the bulk pipe is resumed once the control transfer is done. */
            break;

        default:
            break;
    }
}
/* USB driver thread context.
   Invoked on U0 -> U1/U2 state change.  Return CyTrue to stay in low-power
   state, CyFalse to immediately exit back to U0.  This application always
   allows U1/U2 transitions. */
CyBool_t  LPMRequestCallback ( CyU3PUsbLinkPowerMode link_mode)
{
	return CyTrue;
}



// Spin up USB, let the USB driver handle enumeration
CyU3PReturnStatus_t InitializeUSB(uint8_t hwconfig)
{
	CyU3PReturnStatus_t Status;
	CyBool_t NeedToRenumerate = CyTrue;
    /* Allocate a buffer for handling control requests. */
    glEp0Buffer = (uint8_t *)CyU3PDmaBufferAlloc (CYFX_SDRAPP_MAX_EP0LEN);

	Status = CyU3PUsbStart();

    if (Status == CY_U3P_ERROR_NO_REENUM_REQUIRED)
    {
    	NeedToRenumerate = CyFalse;
    	Status = CY_U3P_SUCCESS;
    	DebugPrint(4,"\r\nNeedToRenumerate = CyFalse");
    }
    CheckStatus("Start USB Driver", Status);

  // Setup callbacks to handle the setup requests, USB Events and LPM Requests (for USB 3.0)
    CyU3PUsbRegisterSetupCallback(CyFxSlFifoApplnUSBSetupCB, CyTrue);
    CyU3PUsbRegisterEventCallback(USBEventCallback);
    CyU3PUsbRegisterLPMRequestCallback( LPMRequestCallback );

    // Driver needs all of the descriptors so it can supply them to the host when requested
    Status = SetUSBdescriptors(hwconfig);
    CheckStatus("Set USB Descriptors", Status);
    // Connect the USB Pins with SuperSpeed operation enabled
    if (NeedToRenumerate)
    {
		  Status = CyU3PConnectState(CyTrue, CyTrue);
		  CheckStatus("ConnectUSB", Status);

    }
    else	// USB connection already exists, restart the Application
    {
    	if (glIsApplnActive) StopApplication();
    	StartApplication();
    }

	return Status;
}

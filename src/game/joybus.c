#include <ultra64.h>
#include <PR/os_internal.h>
#include "config.h"
#include "string.h"
#include "joybus.h"
#include "engine/math_util.h"
#include "game_init.h"
#include "game_input.h"
#include "rumble_init.h"

OSPortInfo gPortInfo[MAXCONTROLLERS] = { 0 };

void __osSiGetAccess(void);
void __osSiRelAccess(void);

////////////////////
// contreaddata.c //
////////////////////

static void __osPackReadData(void);

/**
 * @brief Sets up PIF commands to poll controller inputs.
 * Unmodified from vanilla libultra, but __osPackReadData is modified.
 * Called by poll_controller_inputs.
 *
 * @param[in] mq The SI event message queue.
 * @returns Error status: -1 = busy, 0 = success.
 */
s32 osContStartReadDataEx(OSMesgQueue* mq) {
    s32 ret = 0;

    __osSiGetAccess();

    // If this was called twice in a row, there is no need to write the command again.
    if (__osContLastCmd != CONT_CMD_READ_BUTTON) {
        // Write the command to __osContPifRam.
        __osPackReadData();

        // Write __osContPifRam to the PIF RAM.
        ret = __osSiRawStartDma(OS_WRITE, &__osContPifRam);

        // Wait for the command to execute.
        osRecvMesg(mq, NULL, OS_MESG_BLOCK);
    }

    // Read the resulting __osContPifRam from the PIF RAM.
    ret = __osSiRawStartDma(OS_READ, &__osContPifRam);

    __osContLastCmd = CONT_CMD_READ_BUTTON;

    __osSiRelAccess();

    return ret;
}

/**
 * @brief Writes controller data to OSContPadEx and stores the controller center on first run.
 * Called by osContGetReadDataEx.
 *
 * @param[out] pad     The controller pad to write data to.
 * @param[in ] gcn     The raw GCN button data.
 * @param[in ] stick   The raw GCN analog stick data.
 * @param[in ] c_stick The raw GCN C-stick data.
 * @param[in ] trig    The raw GCN analog triggers data.
 */
static void __osContReadGCNInputData(OSContPadEx* pad, GCNButtons gcn, Analog_u8 stick, Analog_u8 c_stick, Analog_u8 trig) {
    OSContOrigins* origins = &pad->origins;
    N64Buttons n64 = { .raw = 0x0 };

    // The first time the controller is connected, store the origins for the controller's analog sticks.
    if (!origins->initialized) {
        //! TODO: Repoll for origins (0x41) if gcn.standard.GET_ORIGIN is set.
        origins->initialized = TRUE;
        origins->stick   = stick;
        origins->c_stick = c_stick;
        origins->trig    = trig;
    }

    // Write the analog data.
    //! TODO: gcn.standard.USE_ORIGIN behavior.
    pad->stick   = ANALOG_S8_CENTER(stick,   origins->stick);
    pad->c_stick = ANALOG_S8_CENTER(c_stick, origins->c_stick);
    pad->trig    = ANALOG_U8_CENTER(trig,    origins->trig);

    // Map GCN button bits to N64 button bits.
    n64.standard.A       = gcn.standard.A;
    n64.standard.B       = gcn.standard.B;
    n64.standard.Z       = (gcn.standard.L || (trig.l > GCN_TRIGGER_THRESHOLD)); // Swap L and Z.
    n64.standard.START   = gcn.standard.START;
    n64.standard.D_UP    = gcn.standard.D_UP;
    n64.standard.D_DOWN  = gcn.standard.D_DOWN;
    n64.standard.D_LEFT  = gcn.standard.D_LEFT;
    n64.standard.D_RIGHT = gcn.standard.D_RIGHT;
    n64.standard.RESET   = gcn.standard.X; // This bit normally gets set when L+R+START is pressed on a standard N64 controller to recalibrate the analog stick (which also unsets the START bit).
    n64.standard.unused  = gcn.standard.Y; // The N64 controller's unused bit.
    n64.standard.L       = gcn.standard.Z; // Swap L and Z.
    n64.standard.R       = gcn.standard.R;
    n64.standard.C_UP    = (pad->c_stick.y >  GCN_C_STICK_THRESHOLD);
    n64.standard.C_DOWN  = (pad->c_stick.y < -GCN_C_STICK_THRESHOLD);
    n64.standard.C_LEFT  = (pad->c_stick.x < -GCN_C_STICK_THRESHOLD);
    n64.standard.C_RIGHT = (pad->c_stick.x >  GCN_C_STICK_THRESHOLD);

    // Write the button data.
    pad->button = n64.raw;
}

/**
 * @brief Reads PIF command result written by __osPackReadData and converts it into OSContPadEx data.
 * Modified from vanilla libultra to handle GameCube controllers, skip empty/unassigned ports,
 *   and trigger status polling if an active controller is unplugged.
 * Called by poll_controller_inputs.
 *
 * @param[in,out] pad A pointer to the data for the 4 controller pads.
 */
void osContGetReadDataEx(OSContPadEx* pad) {
    u8* ptr = (u8*)__osContPifRam.ramarray;
    __OSContGenericFormat *readformatptr = NULL;
    N64InputData n64Input;
    GCNInputData gcnInput;

    while (*ptr != PIF_CMD_END) {
        if (*ptr == PIF_CMD_SKIP_CHNL || *ptr == PIF_CMD_RESET_CHNL) {
            // Skip empty channels/ports.
            pad++;
            ptr++;
            continue;
        }
        if (*ptr == PIF_CMD_NOP) {
            // Skip bytes that are PIF_CMD_NOP (0xFF).
            ptr++;
            continue;
        }

        // Not a special byte, so read a poll command:
        readformatptr = (__OSContGenericFormat*)ptr;
        pad->errno = CHNL_ERR(readformatptr->size);

        // If the controller being read was unplugged, start status polling on all 4 ports.
        if (pad->errno == (CHNL_ERR_NORESP >> 4)) {
            start_controller_status_polling(FALSE);
            return;
        }

        // Handle different types of poll commands:
        switch (readformatptr->send.cmdID) {
            case CONT_CMD_READ_BUTTON:
                if (pad->errno == (CHNL_ERR_SUCCESS >> 4)) {
                    n64Input = (*(__OSContReadFormat*)ptr).recv.input;

                    pad->button  = n64Input.buttons.raw;
                    pad->stick   = n64Input.stick;
                    pad->c_stick = (Analog_s8){ 0x00, 0x00 };
                    pad->trig    = (Analog_u8){ 0x00, 0x00 };
                }

                ptr += sizeof(__OSContReadFormat);
                break;

            case CONT_CMD_GCN_SHORT_POLL:
                if (pad->errno == (CHNL_ERR_SUCCESS >> 4)) {
                    gcnInput = (*(__OSContGCNShortPollFormat*)ptr).recv.input;
                    Analog_u8 c_stick, trig;

                    // The GameCube controller has various modes for returning the lower analog bits (4 bits per axis vs. 8 bits per axis).
                    switch ((*(__OSContGCNShortPollFormat*)ptr).send.analog_mode) {
                        default: // GCN_MODE_0_211, GCN_MODE_5_211, GCN_MODE_6_211, GCN_MODE_7_211
                            c_stick = gcnInput.m0.c_stick;
                            trig    = ANALOG_U4_TO_U8(gcnInput.m0.trig);
                            break;
                        case GCN_MODE_1_121:
                            c_stick = ANALOG_U4_TO_U8(gcnInput.m1.c_stick);
                            trig    = gcnInput.m1.trig;
                            break;
                        case GCN_MODE_2_112:
                            c_stick = ANALOG_U4_TO_U8(gcnInput.m2.c_stick);
                            trig    = ANALOG_U4_TO_U8(gcnInput.m2.trig);
                            break;
                        case GCN_MODE_3_220:
                            c_stick = gcnInput.m3.c_stick;
                            trig    = gcnInput.m3.trig;
                            break;
                        case GCN_MODE_4_202:
                            c_stick = gcnInput.m3.c_stick;
                            trig    = (Analog_u8){ 0x00, 0x00 };
                            break;
                    }

                    __osContReadGCNInputData(pad, gcnInput.buttons, gcnInput.stick, c_stick, trig);
                } else {
                    pad->origins.initialized = FALSE;
                }

                ptr += sizeof(__OSContGCNShortPollFormat);
                break;

            case CONT_CMD_GCN_LONG_POLL:
                if (pad->errno == (CHNL_ERR_SUCCESS >> 4)) {
                    gcnInput = (*(__OSContGCNLongPollFormat*)ptr).recv.input;

                    // Long poll returns 8 bits for all analog axes (equivalent to mode 3 but with 2 more bytes for the usually unused analog buttons (1 byte each)).
                    __osContReadGCNInputData(pad, gcnInput.buttons, gcnInput.stick, gcnInput.c_stick, gcnInput.trig);
                } else {
                    pad->origins.initialized = FALSE;
                }

                ptr += sizeof(__OSContGCNLongPollFormat);
                break;

            default:
                osSyncPrintf("osContGetReadDataEx error: Unknown input poll command: %.02X\n", readformatptr->send.cmdID);
                return;
        }

        pad++;
    }
}

// Default N64 Controller Input Poll command:
static const __OSContReadFormat sN64WriteFormat = {
    .size.tx            = sizeof(sN64WriteFormat.send),
    .size.rx            = sizeof(sN64WriteFormat.recv),
    .send.cmdID         = CONT_CMD_READ_BUTTON,
    .recv.input.raw.u8  = { [0 ... (sizeof(sN64WriteFormat.recv.input.raw.u8) - 1)] = PIF_CMD_NOP }, // 4 bytes of PIF_CMD_NOP (0xFF).
};

// Default GCN Controller Input Short Poll command:
static const __OSContGCNShortPollFormat sGCNWriteFormatShort = {
    .size.tx            = sizeof(sGCNWriteFormatShort.send),
    .size.rx            = sizeof(sGCNWriteFormatShort.recv),
    .send.cmdID         = CONT_CMD_GCN_SHORT_POLL,
    .send.analog_mode   = GCN_MODE_3_220,
    .send.rumble        = MOTOR_STOP,
    .recv.input.raw.u8  = { [0 ... (sizeof(sGCNWriteFormatShort.recv.input.raw.u8) - 1)] = PIF_CMD_NOP }, // 8 bytes of PIF_CMD_NOP (0xFF).
};

/**
 * @brief Writes PIF commands to poll controller inputs.
 * Modified from vanilla libultra to handle GameCube controllers and skip empty/unassigned ports.
 * Called by osContStartReadData and osContStartReadDataEx.
 */
static void __osPackReadData(void) {
    u8* ptr = (u8*)__osContPifRam.ramarray;
    OSPortInfo* portInfo = NULL;
    int port;

    bzero(__osContPifRam.ramarray, sizeof(__osContPifRam.ramarray));
    __osContPifRam.pifstatus = PIF_STATUS_EXE;

    for (port = 0; port < __osMaxControllers; port++) {
        portInfo = &gPortInfo[port];

        // Make sure this port has a controller plugged in, and if not status repolling, only poll assigned ports.
        if (portInfo->plugged && (gContStatusPolling || portInfo->playerNum)) {
            if (portInfo->type & CONT_CONSOLE_GCN) {
                (*(__OSContGCNShortPollFormat*)ptr) = sGCNWriteFormatShort;
                (*(__OSContGCNShortPollFormat*)ptr).send.rumble = portInfo->gcRumble;

                ptr += sizeof(__OSContGCNShortPollFormat);
            } else {
                (*(__OSContReadFormat*)ptr) = sN64WriteFormat;

                ptr += sizeof(__OSContReadFormat);
            }
        } else {
            // Empty channel/port, so leave a PIF_CMD_SKIP_CHNL (0x00) byte to tell the PIF to skip it.
            ptr++;
        }
    }

    *ptr = PIF_CMD_END;
}

/////////////////
// contquery.c //
/////////////////

void __osContGetInitDataEx(u8* pattern, OSContStatus* data);

/**
 * @brief Read status query data written by osContStartQuery.
 * odified from vanilla libultra to return bitpattern, similar to osContInit.
 * Called by poll_controller_statuses.
 *
 * @param[out] bitpattern The first 4 bits correspond to which 4 ports have controllers plugged in (low-high).
 * @param[out] data       A pointer to the 4 controller statuses.
 */
void osContGetQueryEx(u8* bitpattern, OSContStatus* data) {
    __osContGetInitDataEx(bitpattern, data);
}

//////////////////
// controller.c //
//////////////////

/**
 * @brief Reads PIF command result written by __osPackRequestData and converts it into OSContStatus data.
 * Linker script will resolve references to the original function with this one instead.
 * Modified from vanilla libultra to set gPortInfo type and plugged status.
 * Called by osContInit, osContGetQuery, osContGetQueryEx, and osContReset.
 *
 * @param[out] bitpattern The first 4 bits correspond to which 4 ports have controllers plugged in (low-high).
 * @param[out] data       A pointer to the 4 controller statuses.
 */
void __osContGetInitDataEx(u8* pattern, OSContStatus* data) {
    u8* ptr = (u8*)__osContPifRam.ramarray;
    __OSContRequestFormatAligned requestHeader;
    OSPortInfo* portInfo = NULL;
    u8 bits = 0x0;
    int port;

    for (port = 0; port < __osMaxControllers; port++) {
        requestHeader = *(__OSContRequestFormatAligned*)ptr;
        data->error = CHNL_ERR(requestHeader.fmt.size);

        if (data->error == (CHNL_ERR_SUCCESS >> 4)) {
            portInfo = &gPortInfo[port];

            // Byteswap the SI identifier. This is done in vanilla libultra.
            data->type = ((requestHeader.fmt.recv.type.l << 8) | requestHeader.fmt.recv.type.h);

            // Check the type of controller device connected to the port.
            // Some mupen cores seem to send back a controller type of CONT_TYPE_NULL (0xFFFF) if the core doesn't initialize the input plugin quickly enough,
            //   so check for that and set the input type to N64 controller if so.
            portInfo->type = ((s16)data->type == (s16)CONT_TYPE_NULL) ? CONT_TYPE_NORMAL : data->type;

            // Set this port's status.
            data->status = requestHeader.fmt.recv.status.raw;
            portInfo->plugged = TRUE;
            bits |= (1 << port);
        }

        ptr += sizeof(requestHeader);
        data++;
    }

    *pattern = bits;
}

/////////////
// motor.c //
/////////////

// A buffer to hold separate rumble commands for each port.
ALIGNED64 static OSPifRamEx __MotorDataBuf[MAXCONTROLLERS];

/**
 * @brief Turns controller rumble on or off.
 * Modified from vanilla libultra to handle GameCube controller rumble.
 * Called by osMotorStart, osMotorStop, and osMotorStopHard via macro.
 *
 * @param[in] pfs        A pointer to a buffer for the controller pak (AKA rumble pak) file system.
 * @param[in] motorState MOTOR_STOP = stop motor, MOTOR_START = start motor, MOTOR_STOP_HARD (GCN only) = motor brake.
 * @returns PIF error status.
 */
s32 __osMotorAccessEx(OSPfs* pfs, s32 motorState) {
    s32 err = PFS_ERR_SUCCESS;
    int channel = pfs->channel;
    u8* ptr = (u8*)&__MotorDataBuf[channel];

    if (!(pfs->status & PFS_MOTOR_INITIALIZED)) {
        return PFS_ERR_INVALID;
    }

    if (gPortInfo[channel].type & CONT_CONSOLE_GCN) { // GCN Controllers.
        gPortInfo[channel].gcRumble = motorState;
        __osContLastCmd = PIF_CMD_END;
    } else { // N64 Controllers.
        // N64 rumble pak can only use MOTOR_STOP or MOTOR_START.
        motorState &= MOTOR_MASK_N64;

        __osSiGetAccess();

        // Set the PIF to be ready to run a command.
        __MotorDataBuf[channel].pifstatus = PIF_STATUS_EXE;

        // Leave a PIF_CMD_SKIP_CHNL (0x00) byte in __MotorDataBuf for each skipped channel.
        ptr += channel;

        __OSContRamWriteFormatAligned* readformat = (__OSContRamWriteFormatAligned*)ptr;

        // Set the entire block to either MOTOR_STOP or MOTOR_START.
        memset(readformat->fmt.send.data, motorState, sizeof(readformat->fmt.send.data));

        __osContLastCmd = PIF_CMD_END;

        // Write __MotorDataBuf to the PIF RAM and then wait for the command to execute.
        __osSiRawStartDma(OS_WRITE, &__MotorDataBuf[channel]);
        osRecvMesg(pfs->queue, NULL, OS_MESG_BLOCK);
        // Read the resulting __MotorDataBuf from the PIF RAM and then wait for the command to execute.
        __osSiRawStartDma(OS_READ, &__MotorDataBuf[channel]);
        osRecvMesg(pfs->queue, NULL, OS_MESG_BLOCK);

        // Check for errors.
        err = CHNL_ERR(readformat->fmt.size);
        if (err == (CHNL_ERR_SUCCESS >> 4)) {
            if (motorState == MOTOR_STOP) {
                if (readformat->fmt.recv.datacrc != 0x00) { // 0xFF = Disconnected.
                    err = PFS_ERR_CONTRFAIL; // "Controller pack communication error"
                }
            } else { // MOTOR_START
                if (readformat->fmt.recv.datacrc != 0xEB) { // 0x14 = Uninitialized.
                    err = PFS_ERR_CONTRFAIL; // "Controller pack communication error"
                }
            }
        }

        __osSiRelAccess();
    }

    return err;
}

u8 __osContAddressCrc(u16 addr);
s32 __osPfsSelectBank(OSPfs* pfs, u8 bank);
s32 __osContRamRead(OSMesgQueue* mq, int channel, u16 address, u8* buffer);

/**
 * @brief Writes PIF commands to control the rumble pak.
 * Unmodified from vanilla libultra.
 * Called by osMotorInit and osMotorInitEx.
 *
 * @param[in] channel The port ID to operate on.
 * @param[in] mdata   A pointer to a buffer for the PIF RAM command data.
 */
static void _MakeMotorData(int channel, OSPifRamEx* mdata) {
    u8* ptr = (u8*)mdata->ramarray;
    __OSContRamWriteFormatAligned ramwriteformat;
    int i;

    ramwriteformat.align0          = PIF_CMD_NOP;
    ramwriteformat.fmt.size.tx     = sizeof(ramwriteformat.fmt.send);
    ramwriteformat.fmt.size.rx     = sizeof(ramwriteformat.fmt.recv);
    ramwriteformat.fmt.send.cmdID  = CONT_CMD_WRITE_MEMPAK;
    ramwriteformat.fmt.send.addr.h = (CONT_BLOCK_RUMBLE >> 3);
    ramwriteformat.fmt.send.addr.l = (u8)(__osContAddressCrc(CONT_BLOCK_RUMBLE) | (CONT_BLOCK_RUMBLE << 5));

    // Leave a PIF_CMD_SKIP_CHNL (0x00) byte in mdata->ramarray for each skipped channel.
    if (channel != 0) {
        for (i = 0; i < channel; i++) {
            *ptr++ = PIF_CMD_SKIP_CHNL;
        }
    }

    *(__OSContRamWriteFormatAligned*)ptr = ramwriteformat;
    ptr += sizeof(__OSContRamWriteFormatAligned);
    *ptr = PIF_CMD_END;
}

/**
 * @brief Initializes the Rumble Pak.
 * Modified from vanilla libultra to ignore GameCube controllers.
 * Called by thread6_rumble_loop and cancel_rumble.
 *
 * @param[in]  mq      The SI event message queue.
 * @param[out] pfs     A pointer to a buffer for the controller pak (AKA rumble pak) file system.
 * @param[in]  channel The port ID to operate on.
 * @returns    PFS error status.
 */
s32 osMotorInitEx(OSMesgQueue* mq, OSPfs* pfs, int channel) {
    s32 err = PFS_ERR_SUCCESS;
    u8 data[BLOCKSIZE];

    pfs->status     = PFS_STATUS_NONE;
    pfs->queue      = mq;
    pfs->channel    = channel;
    pfs->activebank = ACCESSORY_ID_NULL;

    if (!(gPortInfo[channel].type & CONT_CONSOLE_GCN)) {
        // Write probe value (ensure Transfer Pak is turned off).
        err = __osPfsSelectBank(pfs, ACCESSORY_ID_TRANSFER_OFF);
        if (err == PFS_ERR_NEW_PACK) {
            // Write probe value (Rumble bank).
            err = __osPfsSelectBank(pfs, ACCESSORY_ID_RUMBLE);
        }
        if (err != PFS_ERR_SUCCESS) {
            return err;
        }

        // Read probe value (1).
        err = __osContRamRead(mq, channel, CONT_BLOCK_DETECT, data);
        if (err == PFS_ERR_NEW_PACK) {
            err = PFS_ERR_CONTRFAIL; // "Controller pack communication error"
        }
        if (err != PFS_ERR_SUCCESS) {
            return err;
        }

        // Ensure the accessory is not a turned off Transfer Pak.
        if (data[BLOCKSIZE - 1] == ACCESSORY_ID_TRANSFER_OFF) {
            return PFS_ERR_DEVICE; // Wrong device
        }

        // Write probe value (Rumble bank).
        err = __osPfsSelectBank(pfs, ACCESSORY_ID_RUMBLE);
        if (err == PFS_ERR_NEW_PACK) {
            err = PFS_ERR_CONTRFAIL; // "Controller pack communication error"
        }
        if (err != PFS_ERR_SUCCESS) {
            return err;
        }

        // Read probe value (2).
        err = __osContRamRead(mq, channel, CONT_BLOCK_DETECT, data);
        if (err == PFS_ERR_NEW_PACK) {
            err = PFS_ERR_CONTRFAIL; // "Controller pack communication error"
        }
        if (err != PFS_ERR_SUCCESS) {
            return err;
        }

        // Ensure the accessory is a Rumble Pak.
        if (data[BLOCKSIZE - 1] != ACCESSORY_ID_RUMBLE) {
            return PFS_ERR_DEVICE; // Wrong device
        }

        // Write the PIF command.
        if (!(pfs->status & PFS_MOTOR_INITIALIZED)) {
            _MakeMotorData(channel, &__MotorDataBuf[channel]);
        }
    }

    pfs->status = PFS_MOTOR_INITIALIZED;

    return PFS_ERR_SUCCESS;
}

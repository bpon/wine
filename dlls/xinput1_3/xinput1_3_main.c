/*
 * The Wine project - Xinput Joystick Library
 * Copyright 2008 Andrew Fenn
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include "config.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#ifdef HAVE_SYS_IOCTL_H
# include <sys/ioctl.h>
#endif
#ifdef HAVE_LINUX_INPUT_H
# include <linux/input.h>
# undef SW_MAX
# if defined(EVIOCGBIT) && defined(EV_ABS) && defined(BTN_PINKIE)
#  define HAVE_CORRECT_LINUXINPUT_H
# endif
#endif

#include "wine/debug.h"
#include "windef.h"
#include "winbase.h"
#include "winerror.h"

#include "xinput.h"

WINE_DEFAULT_DEBUG_CHANNEL(xinput);

#ifdef HAVE_CORRECT_LINUXINPUT_H

#define test_bit(arr,bit)   (((BYTE*)(arr))[(bit)>>3]&(1<<((bit)&7)))
#define set_bit(arr,bit)    (((BYTE*)(arr))[(bit)>>3]|=(1<<((bit)&7)))
#define EVDEVPREFIX         "/dev/input/event"
#define MAX_JOYDEV          64
#define WINE_XINPUT_AXES     8
#define WINE_XINPUT_BUTTONS 10

static BOOL enabled = 1;
static const unsigned int axes[WINE_XINPUT_AXES]        = {ABS_X, ABS_Y, ABS_Z, ABS_RX, ABS_RY, ABS_RZ, ABS_HAT0X, ABS_HAT0Y};
static const unsigned int jbuttons[WINE_XINPUT_BUTTONS] = {BTN_START, BTN_BACK, BTN_THUMBL, BTN_THUMBR, BTN_TL, BTN_TR, BTN_A, BTN_B, BTN_X, BTN_Y};
static const unsigned int xbuttons[WINE_XINPUT_BUTTONS] = {XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_BACK, XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB, XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y};

static struct xpad {
    char             device[strlen(EVDEVPREFIX)+4];
    int              fd;
    BOOL             ff;
    XINPUT_STATE     state;
#ifdef HAVE_STRUCT_FF_EFFECT_DIRECTION
    struct ff_effect eff;
#endif
} xpads[XUSER_MAX_COUNT];

static inline BOOL different_states(const PXINPUT_STATE const a, const PXINPUT_STATE const b)
{
    return a->Gamepad.sThumbLX      != b->Gamepad.sThumbLX
        || a->Gamepad.sThumbLY      != b->Gamepad.sThumbLY
        || a->Gamepad.sThumbRX      != b->Gamepad.sThumbRX
        || a->Gamepad.sThumbRY      != b->Gamepad.sThumbRY
        || a->Gamepad.bLeftTrigger  != b->Gamepad.bLeftTrigger
        || a->Gamepad.bRightTrigger != b->Gamepad.bRightTrigger
        || a->Gamepad.wButtons      != b->Gamepad.wButtons;
}

static inline BOOL different_vibrations(const struct ff_rumble_effect* const a, const PXINPUT_VIBRATION const b)
{
    return a->strong_magnitude != b->wLeftMotorSpeed
        || a->weak_magnitude   != b->wRightMotorSpeed;
}

static void xinput_find_joydevs(void)
{
    int  fd, i, j;
    char buf[MAX_PATH];

    static BOOL init;
    static BYTE absreq[(ABS_MAX+7)/8];
    static BYTE keyreq[(KEY_MAX+7)/8];

    /* Initialize required axes/button bits. These are used to detect which devices are Xbox360 Controllers */
    if(!init) {
        for(i=0; i<XUSER_MAX_COUNT; i++)
            xpads[i].fd = -1;
        for(i=0; i<WINE_XINPUT_AXES; i++)
            set_bit(absreq,axes[i]);
        for(i=0; i<WINE_XINPUT_BUTTONS; i++)
            set_bit(keyreq,jbuttons[i]);
        set_bit(keyreq,BTN_MODE); /* The globe button is reported by the xpad driver but not used by XInput */
        init = TRUE;
    }

    /* Check whether any opened devices have become unavailable */
    for(i=0,j=0; i<XUSER_MAX_COUNT; i++)
        if(xpads[i].fd != -1) {
           if(ioctl(xpads[i].fd, EVIOCGNAME(sizeof(buf)), buf) == -1) {
               TRACE("Xbox360 Controller %d disconnected\n", i);
               close(xpads[i].fd);
               ZeroMemory(&xpads[i], sizeof(struct xpad));
               xpads[i].fd = -1;
           } else
               j++;
        }
    /* If there are enough connected devices we need not check for new ones */
    if(j>=XUSER_MAX_COUNT)
        return;

    /* Loop through all devices and test whether they are Xbox360 Controllers */
    for(j=0; j<MAX_JOYDEV; j++) {
        int  num;
        BOOL no_ff_check = 0;
        BYTE ffbits[(FF_MAX+7)/8];
        BYTE absbits[(ABS_MAX+7)/8];
        BYTE keybits[(KEY_MAX+7)/8];
        ZeroMemory(ffbits, sizeof(ffbits));
        ZeroMemory(absbits, sizeof(absbits));
        ZeroMemory(keybits, sizeof(keybits));

        snprintf(buf, MAX_PATH, EVDEVPREFIX"%d", j);

        /* Test if this device is already opened */
        for(i=0; i<XUSER_MAX_COUNT; i++)
            if(!strcmp(buf, xpads[i].device) && xpads[i].fd != -1)
                break;
        if(i<XUSER_MAX_COUNT)
            continue;

        if((fd = open(buf, O_RDWR)) == -1) {
            fd = open(buf, O_RDONLY);
            no_ff_check = 1;
        }
        if(fd == -1)
            continue;

        /* Get device name */
        buf[MAX_PATH - 1] = 0;
        if (ioctl(fd, EVIOCGNAME(MAX_PATH-1), buf) == -1) {
            WARN("ioct(EVIOCGNAME) failed: %d %s\n", errno, strerror(errno));
            goto fail;
        }

        /* Get device axes and check whether they match those of a Xbox360 Controller */
        if(ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(absbits)), absbits) == -1) {
            WARN("ioct(EVIOCGBIT, EV_ABS) failed: %d %s\n", errno, strerror(errno));
            goto fail;
        }
        if(memcmp(absbits, absreq, sizeof(absbits)))
            goto fail;

        /* Get device buttons and check whether they match those of a Xbox360 Controller */
        if(ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) == -1) {
            WARN("ioct(EVIOCGBIT, EV_KEY) failed: %d %s\n", errno, strerror(errno));
            goto fail;
        }
        if(memcmp(keybits, keyreq, sizeof(keybits)))
            goto fail;

        /* Find the next free XInput slot */
        for(i=0; i<XUSER_MAX_COUNT; i++)
            if(xpads[i].fd == -1)
                break;
        if(i>=XUSER_MAX_COUNT)
            goto fail;

        /* Add the device */
        snprintf(xpads[i].device, sizeof(xpads[i].device), EVDEVPREFIX"%d", j);
        xpads[i].fd = fd;
        TRACE("Added device %s as Xbox360 Controller %d: %s\n", xpads[i].device, i, buf);

#ifdef HAVE_STRUCT_FF_EFFECT_DIRECTION
        xpads[i].eff.type = FF_RUMBLE;
        xpads[i].eff.id = -1;
        xpads[i].eff.replay.length = 0x7fff;

        if(!no_ff_check
        && ioctl(fd, EVIOCGBIT(EV_FF, sizeof(ffbits)), ffbits) != -1
        && test_bit(ffbits,FF_RUMBLE)
        && ioctl(fd, EVIOCGEFFECTS, &num) != -1
        && num > 0
        && ioctl(fd, EVIOCSFF, &xpads[i].eff) != -1) {
            TRACE(" ... with rumble\n");
            xpads[i].ff = 1;
        }
#endif  /* HAVE_STRUCT_FF_EFFECT_DIRECTION */
        continue;

fail:   close(fd);
    }
}
#endif  /* HAVE_CORRECT_LINUXINPUT_H */

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    switch(reason)
    {
    case DLL_WINE_PREATTACH:
        return FALSE; /* prefer native version */
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(inst);
        break;
    case DLL_PROCESS_DETACH:
        XInputEnable(0);
        break;
    }
    return TRUE;
}

void WINAPI XInputEnable(BOOL enable)
{
    /* Setting to false will stop messages from XInputSetState being sent
    to the controllers. Setting to true will send the last vibration
    value (sent to XInputSetState) to the controller and allow messages to
    be sent */
#ifdef HAVE_CORRECT_LINUXINPUT_H
    int i;

    TRACE("(%d) Stub!\n", enable);

#ifdef HAVE_STRUCT_FF_EFFECT_DIRECTION
    if(enabled != enable)
        for(i=0; i<XUSER_MAX_COUNT; i++)
            if(xpads[i].fd != -1 && xpads[i].ff)
                write(xpads[i].fd, &(struct input_event){.type=EV_FF, xpads[i].eff.id, enable}, sizeof(struct input_event));
#endif  /* HAVE_STRUCT_FF_EFFECT_DIRECTION */
    enabled = enable;

#else  /* HAVE_CORRECT_LINUXINPUT_H */
    FIXME("(%d) Stub!\n", enable);
#endif  /* HAVE_CORRECT_LINUXINPUT_H */
}

DWORD WINAPI XInputSetState(DWORD dwUserIndex, XINPUT_VIBRATION* pVibration)
{
#ifdef HAVE_CORRECT_LINUXINPUT_H
    TRACE("(%d %p) Stub!\n", dwUserIndex, pVibration);

    if (!pVibration || dwUserIndex >= XUSER_MAX_COUNT)
        return ERROR_BAD_ARGUMENTS;

    xinput_find_joydevs();
    if(xpads[dwUserIndex].fd == -1)
        return ERROR_DEVICE_NOT_CONNECTED;

#ifdef HAVE_STRUCT_FF_EFFECT_DIRECTION
    if(xpads[dwUserIndex].ff)
        if(different_vibrations(&xpads[dwUserIndex].eff.u.rumble, pVibration)) {
            xpads[dwUserIndex].eff.u.rumble = *(struct ff_rumble_effect*)pVibration;
            ioctl(xpads[dwUserIndex].fd, EVIOCSFF, &xpads[dwUserIndex].eff);
            write(xpads[dwUserIndex].fd, &(struct input_event){.type=EV_FF, xpads[dwUserIndex].eff.id, enabled}, sizeof(struct input_event));
        }
#endif  /* HAVE_STRUCT_FF_EFFECT_DIRECTION */

    return ERROR_SUCCESS;

#else  /* HAVE_CORRECT_LINUXINPUT_H */
    FIXME("(%d %p) Stub!\n", dwUserIndex, pVibration);

    if (dwUserIndex < XUSER_MAX_COUNT)
    {
        return ERROR_DEVICE_NOT_CONNECTED;
        /* If controller exists then return ERROR_SUCCESS */
    }
    return ERROR_BAD_ARGUMENTS;
#endif  /* HAVE_CORRECT_LINUXINPUT_H */
}

DWORD WINAPI XInputGetState(DWORD dwUserIndex, XINPUT_STATE* pState)
{
#ifdef HAVE_CORRECT_LINUXINPUT_H
    int i;
    BYTE key[(KEY_MAX+7)/8];
    struct input_absinfo abs[WINE_XINPUT_AXES];

    TRACE("(%u %p)\n", dwUserIndex, pState);

    if (!pState || dwUserIndex >= XUSER_MAX_COUNT)
        return ERROR_BAD_ARGUMENTS;

    xinput_find_joydevs();
    if(xpads[dwUserIndex].fd == -1)
        return ERROR_DEVICE_NOT_CONNECTED;

    /* If XInput is not enabled we need to return neutral data */
    if(!enabled) {
        ZeroMemory(pState, sizeof(XINPUT_STATE));
        goto packet;
    }

    /* Get joystick axes and map them to XInput axes */
    for(i=0; i<WINE_XINPUT_AXES; i++)
        if(ioctl(xpads[dwUserIndex].fd, EVIOCGABS(axes[i]), &abs[i]) == -1) {
            WARN("ioct(EVIOCGABS, %u) failed: %d %s\n", axes[i], errno, strerror(errno));
            return ERROR_DEVICE_NOT_CONNECTED;
        }
    pState->Gamepad.sThumbLX      =  abs[0].value;
    pState->Gamepad.sThumbLY      = ~abs[1].value;  /* On Linux a positive value means down */
    pState->Gamepad.sThumbRX      =  abs[3].value;  /* On Windows a positive value means up */
    pState->Gamepad.sThumbRY      = ~abs[4].value;  /* Thus we need to invert both Y-axes   */
    pState->Gamepad.bLeftTrigger  =  abs[2].value;
    pState->Gamepad.bRightTrigger =  abs[5].value;
    pState->Gamepad.wButtons      = (abs[7].value<0) *XINPUT_GAMEPAD_DPAD_UP
                                  | (abs[7].value>0) *XINPUT_GAMEPAD_DPAD_DOWN
                                  | (abs[6].value<0) *XINPUT_GAMEPAD_DPAD_LEFT
                                  | (abs[6].value>0) *XINPUT_GAMEPAD_DPAD_RIGHT;

    /* Get joystick keystate and map it to XInput buttons */
    ZeroMemory(key, sizeof(key));
    if(ioctl(xpads[dwUserIndex].fd, EVIOCGKEY(sizeof(key)), key) == -1) {
        WARN("ioct(EVIOCGKEY) failed: %d %s\n", errno, strerror(errno));
        return ERROR_DEVICE_NOT_CONNECTED;
    }
    for(i=0; i<WINE_XINPUT_BUTTONS; i++)
        if(test_bit(key,jbuttons[i])) pState->Gamepad.wButtons |= xbuttons[i];

packet:
    /* Increment PacketNumber only if state has changed */
    pState->dwPacketNumber = xpads[dwUserIndex].state.dwPacketNumber;
    if(different_states(pState, &xpads[dwUserIndex].state)) {
        pState->dwPacketNumber++;
        xpads[dwUserIndex].state = *pState;
    }

    return ERROR_SUCCESS;

#else  /* HAVE_CORRECT_LINUXINPUT_H */
    static int warn_once;

    if (!warn_once++)
        FIXME("(%u %p)\n", dwUserIndex, pState);

    if (dwUserIndex < XUSER_MAX_COUNT)
    {
        return ERROR_DEVICE_NOT_CONNECTED;
        /* If controller exists then return ERROR_SUCCESS */
    }
    return ERROR_BAD_ARGUMENTS;
#endif  /* HAVE_CORRECT_LINUXINPUT_H */
}

DWORD WINAPI XInputGetKeystroke(DWORD dwUserIndex, DWORD dwReserve, PXINPUT_KEYSTROKE pKeystroke)
{
    FIXME("(%d %d %p) Stub!\n", dwUserIndex, dwReserve, pKeystroke);

    if (dwUserIndex < XUSER_MAX_COUNT)
    {
        return ERROR_DEVICE_NOT_CONNECTED;
        /* If controller exists then return ERROR_SUCCESS */
    }
    return ERROR_BAD_ARGUMENTS;
}

DWORD WINAPI XInputGetCapabilities(DWORD dwUserIndex, DWORD dwFlags, XINPUT_CAPABILITIES* pCapabilities)
{
#ifdef HAVE_CORRECT_LINUXINPUT_H
    XINPUT_STATE tempstate;

    TRACE("(%d %d %p)\n", dwUserIndex, dwFlags, pCapabilities);

    if (!pCapabilities || dwUserIndex >= XUSER_MAX_COUNT || (dwFlags != 0 && dwFlags != XINPUT_FLAG_GAMEPAD))
        return ERROR_BAD_ARGUMENTS;

    if(XInputGetState(dwUserIndex, &tempstate) == ERROR_SUCCESS) {
        pCapabilities->Type = XINPUT_DEVTYPE_GAMEPAD;
        pCapabilities->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
        pCapabilities->Flags = 0;
        pCapabilities->Gamepad = xpads[dwUserIndex].state.Gamepad;
#ifdef HAVE_STRUCT_FF_EFFECT_DIRECTION
        pCapabilities->Vibration = *(PXINPUT_VIBRATION)&xpads[dwUserIndex].eff.u.rumble;
#else  /* HAVE_STRUCT_FF_EFFECT_DIRECTION */
        pCapabilities->Vibration = (XINPUT_VIBRATION){0,0};
#endif  /* HAVE_STRUCT_FF_EFFECT_DIRECTION */
        return ERROR_SUCCESS;
    } else
        return ERROR_DEVICE_NOT_CONNECTED;

#else  /* HAVE_CORRECT_LINUXINPUT_H */
    static int warn_once;

    if (!warn_once++)
        FIXME("(%d %d %p)\n", dwUserIndex, dwFlags, pCapabilities);

    if (dwUserIndex < XUSER_MAX_COUNT)
    {
        return ERROR_DEVICE_NOT_CONNECTED;
        /* If controller exists then return ERROR_SUCCESS */
    }
    return ERROR_BAD_ARGUMENTS;
#endif  /* HAVE_CORRECT_LINUXINPUT_H */
}

DWORD WINAPI XInputGetDSoundAudioDeviceGuids(DWORD dwUserIndex, GUID* pDSoundRenderGuid, GUID* pDSoundCaptureGuid)
{
    FIXME("(%d %p %p) Stub!\n", dwUserIndex, pDSoundRenderGuid, pDSoundCaptureGuid);

    if (dwUserIndex < XUSER_MAX_COUNT)
    {
        return ERROR_DEVICE_NOT_CONNECTED;
        /* If controller exists then return ERROR_SUCCESS */
    }
    return ERROR_BAD_ARGUMENTS;
}

DWORD WINAPI XInputGetBatteryInformation(DWORD dwUserIndex, BYTE deviceType, XINPUT_BATTERY_INFORMATION* pBatteryInfo)
{
    FIXME("(%d %u %p) Stub!\n", dwUserIndex, deviceType, pBatteryInfo);

    if (dwUserIndex < XUSER_MAX_COUNT)
    {
        return ERROR_DEVICE_NOT_CONNECTED;
        /* If controller exists then return ERROR_SUCCESS */
    }
    return ERROR_BAD_ARGUMENTS;
}

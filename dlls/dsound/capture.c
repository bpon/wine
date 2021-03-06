/*              DirectSoundCapture
 *
 * Copyright 1998 Marcus Meissner
 * Copyright 1998 Rob Riggs
 * Copyright 2000-2001 TransGaming Technologies, Inc.
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
/*
 * TODO:
 *	Implement FX support.
 *	Implement both IDirectSoundCaptureBuffer and IDirectSoundCaptureBuffer8
 *	Make DirectSoundCaptureCreate and DirectSoundCaptureCreate8 behave differently
 */

#include <stdarg.h>

#define COBJMACROS
#define NONAMELESSSTRUCT
#define NONAMELESSUNION
#include "windef.h"
#include "winbase.h"
#include "winuser.h"
#include "mmsystem.h"
#include "mmddk.h"
#include "winternl.h"
#include "winnls.h"
#include "wine/debug.h"
#include "dsound.h"
#include "dsound_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(dsound);


/*****************************************************************************
 * IDirectSoundCaptureNotify implementation structure
 */
struct IDirectSoundCaptureNotifyImpl
{
    /* IUnknown fields */
    const IDirectSoundNotifyVtbl       *lpVtbl;
    LONG                                ref;
    IDirectSoundCaptureBufferImpl*      dscb;
};

/*******************************************************************************
 *		IDirectSoundCaptureNotify
 */
static HRESULT WINAPI IDirectSoundCaptureNotifyImpl_QueryInterface(
    LPDIRECTSOUNDNOTIFY iface,
    REFIID riid,
    LPVOID *ppobj)
{
    IDirectSoundCaptureNotifyImpl *This = (IDirectSoundCaptureNotifyImpl *)iface;
    TRACE("(%p,%s,%p)\n",This,debugstr_guid(riid),ppobj);

    if (This->dscb == NULL) {
	WARN("invalid parameter\n");
	return E_INVALIDARG;
    }

    return IDirectSoundCaptureBuffer_QueryInterface((LPDIRECTSOUNDCAPTUREBUFFER)This->dscb, riid, ppobj);
}

static ULONG WINAPI IDirectSoundCaptureNotifyImpl_AddRef(LPDIRECTSOUNDNOTIFY iface)
{
    IDirectSoundCaptureNotifyImpl *This = (IDirectSoundCaptureNotifyImpl *)iface;
    ULONG ref = InterlockedIncrement(&(This->ref));
    TRACE("(%p) ref was %d\n", This, ref - 1);
    return ref;
}

static ULONG WINAPI IDirectSoundCaptureNotifyImpl_Release(LPDIRECTSOUNDNOTIFY iface)
{
    IDirectSoundCaptureNotifyImpl *This = (IDirectSoundCaptureNotifyImpl *)iface;
    ULONG ref = InterlockedDecrement(&(This->ref));
    TRACE("(%p) ref was %d\n", This, ref + 1);

    if (!ref) {
	This->dscb->notify=NULL;
	IDirectSoundCaptureBuffer_Release((LPDIRECTSOUNDCAPTUREBUFFER)This->dscb);
	HeapFree(GetProcessHeap(),0,This);
	TRACE("(%p) released\n", This);
    }
    return ref;
}

static HRESULT WINAPI IDirectSoundCaptureNotifyImpl_SetNotificationPositions(
    LPDIRECTSOUNDNOTIFY iface,
    DWORD howmuch,
    LPCDSBPOSITIONNOTIFY notify)
{
    IDirectSoundCaptureNotifyImpl *This = (IDirectSoundCaptureNotifyImpl *)iface;
    TRACE("(%p,0x%08x,%p)\n",This,howmuch,notify);

    if (howmuch > 0 && notify == NULL) {
	WARN("invalid parameter: notify == NULL\n");
	return DSERR_INVALIDPARAM;
    }

    if (TRACE_ON(dsound)) {
	unsigned int i;
	for (i=0;i<howmuch;i++)
            TRACE("notify at %d to %p\n",
	    notify[i].dwOffset,notify[i].hEventNotify);
    }

    if (howmuch > 0) {
	/* Make an internal copy of the caller-supplied array.
	 * Replace the existing copy if one is already present. */
	if (This->dscb->notifies)
	    This->dscb->notifies = HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
		This->dscb->notifies, howmuch * sizeof(DSBPOSITIONNOTIFY));
	else
	    This->dscb->notifies = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
		howmuch * sizeof(DSBPOSITIONNOTIFY));

	if (This->dscb->notifies == NULL) {
	    WARN("out of memory\n");
	    return DSERR_OUTOFMEMORY;
	}
	CopyMemory(This->dscb->notifies, notify, howmuch * sizeof(DSBPOSITIONNOTIFY));
	This->dscb->nrofnotifies = howmuch;
    } else {
        HeapFree(GetProcessHeap(), 0, This->dscb->notifies);
        This->dscb->notifies = NULL;
        This->dscb->nrofnotifies = 0;
    }

    return S_OK;
}

static const IDirectSoundNotifyVtbl dscnvt =
{
    IDirectSoundCaptureNotifyImpl_QueryInterface,
    IDirectSoundCaptureNotifyImpl_AddRef,
    IDirectSoundCaptureNotifyImpl_Release,
    IDirectSoundCaptureNotifyImpl_SetNotificationPositions,
};

static HRESULT IDirectSoundCaptureNotifyImpl_Create(
    IDirectSoundCaptureBufferImpl *dscb,
    IDirectSoundCaptureNotifyImpl **pdscn)
{
    IDirectSoundCaptureNotifyImpl * dscn;
    TRACE("(%p,%p)\n",dscb,pdscn);

    dscn = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*dscn));

    if (dscn == NULL) {
	WARN("out of memory\n");
	return DSERR_OUTOFMEMORY;
    }

    dscn->ref = 0;
    dscn->lpVtbl = &dscnvt;
    dscn->dscb = dscb;
    dscb->notify = dscn;
    IDirectSoundCaptureBuffer_AddRef((LPDIRECTSOUNDCAPTUREBUFFER)dscb);

    *pdscn = dscn;
    return DS_OK;
}


static const char * const captureStateString[] = {
    "STATE_STOPPED",
    "STATE_STARTING",
    "STATE_CAPTURING",
    "STATE_STOPPING"
};


/*******************************************************************************
 *		IDirectSoundCaptureBuffer
 */
static HRESULT WINAPI
IDirectSoundCaptureBufferImpl_QueryInterface(
    LPDIRECTSOUNDCAPTUREBUFFER8 iface,
    REFIID riid,
    LPVOID* ppobj )
{
    IDirectSoundCaptureBufferImpl *This = (IDirectSoundCaptureBufferImpl *)iface;
    HRESULT hres;

    TRACE( "(%p,%s,%p)\n", This, debugstr_guid(riid), ppobj );

    if (ppobj == NULL) {
	WARN("invalid parameter\n");
	return E_INVALIDARG;
    }

    *ppobj = NULL;

    if ( IsEqualGUID( &IID_IDirectSoundNotify, riid ) ) {
	if (!This->notify){
	    hres = IDirectSoundCaptureNotifyImpl_Create(This, &This->notify);
	    if(FAILED(hres))
		return hres;
	}

        IDirectSoundNotify_AddRef((LPDIRECTSOUNDNOTIFY)This->notify);
        *ppobj = This->notify;
        return DS_OK;
    }

    if ( IsEqualGUID( &IID_IDirectSoundCaptureBuffer, riid ) ||
         IsEqualGUID( &IID_IDirectSoundCaptureBuffer8, riid ) ) {
	IDirectSoundCaptureBuffer8_AddRef(iface);
	*ppobj = This;
	return NO_ERROR;
    }

    FIXME("(%p,%s,%p) unsupported GUID\n", This, debugstr_guid(riid), ppobj);
    return E_NOINTERFACE;
}

static ULONG WINAPI
IDirectSoundCaptureBufferImpl_AddRef( LPDIRECTSOUNDCAPTUREBUFFER8 iface )
{
    IDirectSoundCaptureBufferImpl *This = (IDirectSoundCaptureBufferImpl *)iface;
    ULONG ref = InterlockedIncrement(&(This->ref));
    TRACE("(%p) ref was %d\n", This, ref - 1);
    return ref;
}

static ULONG WINAPI
IDirectSoundCaptureBufferImpl_Release( LPDIRECTSOUNDCAPTUREBUFFER8 iface )
{
    IDirectSoundCaptureBufferImpl *This = (IDirectSoundCaptureBufferImpl *)iface;
    ULONG ref = InterlockedDecrement(&(This->ref));
    TRACE("(%p) ref was %d\n", This, ref + 1);

    if (!ref) {
        TRACE("deleting object\n");
        if (This->device->state == STATE_CAPTURING)
            This->device->state = STATE_STOPPING;

        HeapFree(GetProcessHeap(),0, This->pdscbd);

        if (This->device->client) {
            IAudioClient_Release(This->device->client);
            This->device->client = NULL;
        }

        if (This->device->capture) {
            IAudioCaptureClient_Release(This->device->capture);
            This->device->capture = NULL;
        }

        /* remove from DirectSoundCaptureDevice */
        This->device->capture_buffer = NULL;

        if (This->notify)
            IDirectSoundNotify_Release((LPDIRECTSOUNDNOTIFY)This->notify);

        HeapFree(GetProcessHeap(), 0, This->notifies);
        HeapFree( GetProcessHeap(), 0, This );
        TRACE("(%p) released\n", This);
    }
    return ref;
}

static HRESULT WINAPI
IDirectSoundCaptureBufferImpl_GetCaps(
    LPDIRECTSOUNDCAPTUREBUFFER8 iface,
    LPDSCBCAPS lpDSCBCaps )
{
    IDirectSoundCaptureBufferImpl *This = (IDirectSoundCaptureBufferImpl *)iface;
    TRACE( "(%p,%p)\n", This, lpDSCBCaps );

    if (lpDSCBCaps == NULL) {
        WARN("invalid parameter: lpDSCBCaps == NULL\n");
        return DSERR_INVALIDPARAM;
    }

    if (lpDSCBCaps->dwSize < sizeof(DSCBCAPS)) {
        WARN("invalid parameter: lpDSCBCaps->dwSize = %d\n", lpDSCBCaps->dwSize);
        return DSERR_INVALIDPARAM;
    }

    if (This->device == NULL) {
        WARN("invalid parameter: This->device == NULL\n");
        return DSERR_INVALIDPARAM;
    }

    lpDSCBCaps->dwSize = sizeof(DSCBCAPS);
    lpDSCBCaps->dwFlags = This->flags;
    lpDSCBCaps->dwBufferBytes = This->pdscbd->dwBufferBytes;
    lpDSCBCaps->dwReserved = 0;

    TRACE("returning DS_OK\n");
    return DS_OK;
}

static HRESULT WINAPI
IDirectSoundCaptureBufferImpl_GetCurrentPosition(
    LPDIRECTSOUNDCAPTUREBUFFER8 iface,
    LPDWORD lpdwCapturePosition,
    LPDWORD lpdwReadPosition )
{
    IDirectSoundCaptureBufferImpl *This = (IDirectSoundCaptureBufferImpl *)iface;
    TRACE( "(%p,%p,%p)\n", This, lpdwCapturePosition, lpdwReadPosition );

    if (This->device == NULL) {
        WARN("invalid parameter: This->device == NULL\n");
        return DSERR_INVALIDPARAM;
    }

    EnterCriticalSection(&This->device->lock);

    if (!This->device->client) {
        LeaveCriticalSection(&This->device->lock);
        WARN("no driver\n");
        return DSERR_NODRIVER;
    }

    if(lpdwCapturePosition)
        *lpdwCapturePosition = This->device->write_pos_bytes;

    if(lpdwReadPosition)
        *lpdwReadPosition = This->device->write_pos_bytes;

    LeaveCriticalSection(&This->device->lock);

    TRACE("cappos=%d readpos=%d\n", (lpdwCapturePosition?*lpdwCapturePosition:-1), (lpdwReadPosition?*lpdwReadPosition:-1));
    TRACE("returning DS_OK\n");

    return DS_OK;
}

static HRESULT WINAPI
IDirectSoundCaptureBufferImpl_GetFormat(
    LPDIRECTSOUNDCAPTUREBUFFER8 iface,
    LPWAVEFORMATEX lpwfxFormat,
    DWORD dwSizeAllocated,
    LPDWORD lpdwSizeWritten )
{
    IDirectSoundCaptureBufferImpl *This = (IDirectSoundCaptureBufferImpl *)iface;
    HRESULT hres = DS_OK;
    TRACE( "(%p,%p,0x%08x,%p)\n", This, lpwfxFormat, dwSizeAllocated,
        lpdwSizeWritten );

    if (This->device == NULL) {
        WARN("invalid parameter: This->device == NULL\n");
        return DSERR_INVALIDPARAM;
    }

    if (dwSizeAllocated > (sizeof(WAVEFORMATEX) + This->device->pwfx->cbSize))
        dwSizeAllocated = sizeof(WAVEFORMATEX) + This->device->pwfx->cbSize;

    if (lpwfxFormat) { /* NULL is valid (just want size) */
        CopyMemory(lpwfxFormat, This->device->pwfx, dwSizeAllocated);
        if (lpdwSizeWritten)
            *lpdwSizeWritten = dwSizeAllocated;
    } else {
        if (lpdwSizeWritten)
            *lpdwSizeWritten = sizeof(WAVEFORMATEX) + This->device->pwfx->cbSize;
        else {
            TRACE("invalid parameter: lpdwSizeWritten = NULL\n");
            hres = DSERR_INVALIDPARAM;
        }
    }

    TRACE("returning %08x\n", hres);
    return hres;
}

static HRESULT WINAPI
IDirectSoundCaptureBufferImpl_GetStatus(
    LPDIRECTSOUNDCAPTUREBUFFER8 iface,
    LPDWORD lpdwStatus )
{
    IDirectSoundCaptureBufferImpl *This = (IDirectSoundCaptureBufferImpl *)iface;
    TRACE( "(%p, %p), thread is %04x\n", This, lpdwStatus, GetCurrentThreadId() );

    if (This->device == NULL) {
        WARN("invalid parameter: This->device == NULL\n");
        return DSERR_INVALIDPARAM;
    }

    if (lpdwStatus == NULL) {
        WARN("invalid parameter: lpdwStatus == NULL\n");
        return DSERR_INVALIDPARAM;
    }

    *lpdwStatus = 0;
    EnterCriticalSection(&(This->device->lock));

    TRACE("old This->device->state=%s, old lpdwStatus=%08x\n",
	captureStateString[This->device->state],*lpdwStatus);
    if ((This->device->state == STATE_STARTING) ||
        (This->device->state == STATE_CAPTURING)) {
        *lpdwStatus |= DSCBSTATUS_CAPTURING;
        if (This->flags & DSCBSTART_LOOPING)
            *lpdwStatus |= DSCBSTATUS_LOOPING;
    }
    TRACE("new This->device->state=%s, new lpdwStatus=%08x\n",
	captureStateString[This->device->state],*lpdwStatus);
    LeaveCriticalSection(&(This->device->lock));

    TRACE("status=%x\n", *lpdwStatus);
    TRACE("returning DS_OK\n");
    return DS_OK;
}

static HRESULT WINAPI
IDirectSoundCaptureBufferImpl_Initialize(
    LPDIRECTSOUNDCAPTUREBUFFER8 iface,
    LPDIRECTSOUNDCAPTURE lpDSC,
    LPCDSCBUFFERDESC lpcDSCBDesc )
{
    IDirectSoundCaptureBufferImpl *This = (IDirectSoundCaptureBufferImpl *)iface;

    FIXME( "(%p,%p,%p): stub\n", This, lpDSC, lpcDSCBDesc );

    return DS_OK;
}

static HRESULT WINAPI
IDirectSoundCaptureBufferImpl_Lock(
    LPDIRECTSOUNDCAPTUREBUFFER8 iface,
    DWORD dwReadCusor,
    DWORD dwReadBytes,
    LPVOID* lplpvAudioPtr1,
    LPDWORD lpdwAudioBytes1,
    LPVOID* lplpvAudioPtr2,
    LPDWORD lpdwAudioBytes2,
    DWORD dwFlags )
{
    HRESULT hres = DS_OK;
    IDirectSoundCaptureBufferImpl *This = (IDirectSoundCaptureBufferImpl *)iface;
    TRACE( "(%p,%08u,%08u,%p,%p,%p,%p,0x%08x) at %d\n", This, dwReadCusor,
        dwReadBytes, lplpvAudioPtr1, lpdwAudioBytes1, lplpvAudioPtr2,
        lpdwAudioBytes2, dwFlags, GetTickCount() );

    if (This->device == NULL) {
        WARN("invalid parameter: This->device == NULL\n");
        return DSERR_INVALIDPARAM;
    }

    if (lplpvAudioPtr1 == NULL) {
        WARN("invalid parameter: lplpvAudioPtr1 == NULL\n");
        return DSERR_INVALIDPARAM;
    }

    if (lpdwAudioBytes1 == NULL) {
        WARN("invalid parameter: lpdwAudioBytes1 == NULL\n");
        return DSERR_INVALIDPARAM;
    }

    EnterCriticalSection(&(This->device->lock));

    if (This->device->client) {
        *lplpvAudioPtr1 = This->device->buffer + dwReadCusor;
        if ( (dwReadCusor + dwReadBytes) > This->device->buflen) {
            *lpdwAudioBytes1 = This->device->buflen - dwReadCusor;
	    if (lplpvAudioPtr2)
            	*lplpvAudioPtr2 = This->device->buffer;
	    if (lpdwAudioBytes2)
		*lpdwAudioBytes2 = dwReadBytes - *lpdwAudioBytes1;
        } else {
            *lpdwAudioBytes1 = dwReadBytes;
	    if (lplpvAudioPtr2)
            	*lplpvAudioPtr2 = 0;
	    if (lpdwAudioBytes2)
            	*lpdwAudioBytes2 = 0;
        }
    } else {
        TRACE("invalid call\n");
        hres = DSERR_INVALIDCALL;   /* DSERR_NODRIVER ? */
    }

    LeaveCriticalSection(&(This->device->lock));

    TRACE("returning %08x\n", hres);
    return hres;
}

static HRESULT WINAPI
IDirectSoundCaptureBufferImpl_Start(
    LPDIRECTSOUNDCAPTUREBUFFER8 iface,
    DWORD dwFlags )
{
    HRESULT hres;
    IDirectSoundCaptureBufferImpl *This = (IDirectSoundCaptureBufferImpl *)iface;

    TRACE( "(%p,0x%08x)\n", This, dwFlags );

    if (This->device == NULL) {
        WARN("invalid parameter: This->device == NULL\n");
        return DSERR_INVALIDPARAM;
    }

    if ( !This->device->client ) {
        WARN("no driver\n");
        return DSERR_NODRIVER;
    }

    EnterCriticalSection(&(This->device->lock));

    This->flags = dwFlags;
    TRACE("old This->device->state=%s\n",captureStateString[This->device->state]);
    if (This->device->state == STATE_STOPPED)
        This->device->state = STATE_STARTING;
    else if (This->device->state == STATE_STOPPING)
        This->device->state = STATE_CAPTURING;
    TRACE("new This->device->state=%s\n",captureStateString[This->device->state]);

    if (This->device->buffer)
        FillMemory(This->device->buffer, This->device->buflen, (This->device->pwfx->wBitsPerSample == 8) ? 128 : 0);

    hres = IAudioClient_Start(This->device->client);
    if(FAILED(hres)){
        WARN("Start failed: %08x\n", hres);
        LeaveCriticalSection(&This->device->lock);
        return hres;
    }

    LeaveCriticalSection(&This->device->lock);

    TRACE("returning DS_OK\n");
    return DS_OK;
}

static HRESULT WINAPI
IDirectSoundCaptureBufferImpl_Stop( LPDIRECTSOUNDCAPTUREBUFFER8 iface )
{
    HRESULT hres;
    IDirectSoundCaptureBufferImpl *This = (IDirectSoundCaptureBufferImpl *)iface;
    TRACE( "(%p)\n", This );

    if (This->device == NULL) {
        WARN("invalid parameter: This->device == NULL\n");
        return DSERR_INVALIDPARAM;
    }

    EnterCriticalSection(&(This->device->lock));

    TRACE("old This->device->state=%s\n",captureStateString[This->device->state]);
    if (This->device->state == STATE_CAPTURING)
	This->device->state = STATE_STOPPING;
    else if (This->device->state == STATE_STARTING)
	This->device->state = STATE_STOPPED;
    TRACE("new This->device->state=%s\n",captureStateString[This->device->state]);

    if(This->device->client){
        hres = IAudioClient_Stop(This->device->client);
        if(FAILED(hres)){
            LeaveCriticalSection(&This->device->lock);
            return hres;
        }
    }

    LeaveCriticalSection(&(This->device->lock));

    TRACE("returning DS_OK\n");
    return DS_OK;
}

static HRESULT WINAPI
IDirectSoundCaptureBufferImpl_Unlock(
    LPDIRECTSOUNDCAPTUREBUFFER8 iface,
    LPVOID lpvAudioPtr1,
    DWORD dwAudioBytes1,
    LPVOID lpvAudioPtr2,
    DWORD dwAudioBytes2 )
{
    HRESULT hres = DS_OK;
    IDirectSoundCaptureBufferImpl *This = (IDirectSoundCaptureBufferImpl *)iface;
    TRACE( "(%p,%p,%08u,%p,%08u)\n", This, lpvAudioPtr1, dwAudioBytes1,
        lpvAudioPtr2, dwAudioBytes2 );

    if (lpvAudioPtr1 == NULL) {
        WARN("invalid parameter: lpvAudioPtr1 == NULL\n");
        return DSERR_INVALIDPARAM;
    }

    if (!This->device->client) {
        WARN("invalid call\n");
        hres = DSERR_INVALIDCALL;
    }

    TRACE("returning %08x\n", hres);
    return hres;
}

static HRESULT WINAPI
IDirectSoundCaptureBufferImpl_GetObjectInPath(
    LPDIRECTSOUNDCAPTUREBUFFER8 iface,
    REFGUID rguidObject,
    DWORD dwIndex,
    REFGUID rguidInterface,
    LPVOID* ppObject )
{
    IDirectSoundCaptureBufferImpl *This = (IDirectSoundCaptureBufferImpl *)iface;

    FIXME( "(%p,%s,%u,%s,%p): stub\n", This, debugstr_guid(rguidObject),
        dwIndex, debugstr_guid(rguidInterface), ppObject );

    return DS_OK;
}

static HRESULT WINAPI
IDirectSoundCaptureBufferImpl_GetFXStatus(
    LPDIRECTSOUNDCAPTUREBUFFER8 iface,
    DWORD dwFXCount,
    LPDWORD pdwFXStatus )
{
    IDirectSoundCaptureBufferImpl *This = (IDirectSoundCaptureBufferImpl *)iface;

    FIXME( "(%p,%u,%p): stub\n", This, dwFXCount, pdwFXStatus );

    return DS_OK;
}

static const IDirectSoundCaptureBuffer8Vtbl dscbvt =
{
    /* IUnknown methods */
    IDirectSoundCaptureBufferImpl_QueryInterface,
    IDirectSoundCaptureBufferImpl_AddRef,
    IDirectSoundCaptureBufferImpl_Release,

    /* IDirectSoundCaptureBuffer methods */
    IDirectSoundCaptureBufferImpl_GetCaps,
    IDirectSoundCaptureBufferImpl_GetCurrentPosition,
    IDirectSoundCaptureBufferImpl_GetFormat,
    IDirectSoundCaptureBufferImpl_GetStatus,
    IDirectSoundCaptureBufferImpl_Initialize,
    IDirectSoundCaptureBufferImpl_Lock,
    IDirectSoundCaptureBufferImpl_Start,
    IDirectSoundCaptureBufferImpl_Stop,
    IDirectSoundCaptureBufferImpl_Unlock,

    /* IDirectSoundCaptureBuffer methods */
    IDirectSoundCaptureBufferImpl_GetObjectInPath,
    IDirectSoundCaptureBufferImpl_GetFXStatus
};

static void capture_CheckNotify(IDirectSoundCaptureBufferImpl *This, DWORD from, DWORD len)
{
    int i;
    for (i = 0; i < This->nrofnotifies; ++i) {
        LPDSBPOSITIONNOTIFY event = This->notifies + i;
        DWORD offset = event->dwOffset;
        TRACE("checking %d, position %d, event = %p\n", i, offset, event->hEventNotify);

        if (offset == DSBPN_OFFSETSTOP) {
            if (!from && !len) {
                SetEvent(event->hEventNotify);
                TRACE("signalled event %p (%d)\n", event->hEventNotify, i);
                return;
            }
            else return;
        }

        if (offset >= from && offset < (from + len))
        {
            TRACE("signalled event %p (%d)\n", event->hEventNotify, i);
            SetEvent(event->hEventNotify);
        }
    }
}

static HRESULT IDirectSoundCaptureBufferImpl_Create(
    DirectSoundCaptureDevice *device,
    IDirectSoundCaptureBufferImpl ** ppobj,
    LPCDSCBUFFERDESC lpcDSCBufferDesc)
{
    LPWAVEFORMATEX  wfex;
    IDirectSoundCaptureBufferImpl *This;
    TRACE( "(%p,%p,%p)\n", device, ppobj, lpcDSCBufferDesc);

    if (ppobj == NULL) {
	WARN("invalid parameter: ppobj == NULL\n");
	return DSERR_INVALIDPARAM;
    }

    *ppobj = NULL;

    if (!device) {
	WARN("not initialized\n");
	return DSERR_UNINITIALIZED;
    }

    if (lpcDSCBufferDesc == NULL) {
	WARN("invalid parameter: lpcDSCBufferDesc == NULL\n");
	return DSERR_INVALIDPARAM;
    }

    if ( ((lpcDSCBufferDesc->dwSize != sizeof(DSCBUFFERDESC)) &&
          (lpcDSCBufferDesc->dwSize != sizeof(DSCBUFFERDESC1))) ||
        (lpcDSCBufferDesc->dwBufferBytes == 0) ||
        (lpcDSCBufferDesc->lpwfxFormat == NULL) ) { /* FIXME: DSERR_BADFORMAT ? */
	WARN("invalid lpcDSCBufferDesc\n");
	return DSERR_INVALIDPARAM;
    }

    wfex = lpcDSCBufferDesc->lpwfxFormat;

    TRACE("(formattag=0x%04x,chans=%d,samplerate=%d,"
        "bytespersec=%d,blockalign=%d,bitspersamp=%d,cbSize=%d)\n",
        wfex->wFormatTag, wfex->nChannels, wfex->nSamplesPerSec,
        wfex->nAvgBytesPerSec, wfex->nBlockAlign,
        wfex->wBitsPerSample, wfex->cbSize);

    device->pwfx = DSOUND_CopyFormat(wfex);
    if ( device->pwfx == NULL )
	return DSERR_OUTOFMEMORY;

    This = HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,
        sizeof(IDirectSoundCaptureBufferImpl));

    if ( This == NULL ) {
	WARN("out of memory\n");
	return DSERR_OUTOFMEMORY;
    } else {
        HRESULT err = DS_OK;
        LPBYTE newbuf;
        DWORD buflen;

        This->ref = 1;
        This->device = device;
        This->device->capture_buffer = This;
        This->notify = NULL;
        This->nrofnotifies = 0;

        This->pdscbd = HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,
            lpcDSCBufferDesc->dwSize);
        if (This->pdscbd)
            CopyMemory(This->pdscbd, lpcDSCBufferDesc, lpcDSCBufferDesc->dwSize);
        else {
            WARN("no memory\n");
            This->device->capture_buffer = 0;
            HeapFree( GetProcessHeap(), 0, This );
            return DSERR_OUTOFMEMORY;
        }

        This->lpVtbl = &dscbvt;

        err = IMMDevice_Activate(device->mmdevice, &IID_IAudioClient,
                CLSCTX_INPROC_SERVER, NULL, (void**)&device->client);
        if(FAILED(err)){
            WARN("Activate failed: %08x\n", err);
            HeapFree(GetProcessHeap(), 0, This->pdscbd);
            This->device->capture_buffer = 0;
            HeapFree( GetProcessHeap(), 0, This );
            return err;
        }

        err = IAudioClient_Initialize(device->client,
                AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_NOPERSIST,
                200 * 100000, 50000, device->pwfx, NULL);
        if(FAILED(err)){
            WARN("Initialize failed: %08x\n", err);
            IAudioClient_Release(device->client);
            device->client = NULL;
            HeapFree(GetProcessHeap(), 0, This->pdscbd);
            This->device->capture_buffer = 0;
            HeapFree( GetProcessHeap(), 0, This );
            return err;
        }

        err = IAudioClient_GetService(device->client, &IID_IAudioCaptureClient,
                (void**)&device->capture);
        if(FAILED(err)){
            WARN("GetService failed: %08x\n", err);
            IAudioClient_Release(device->client);
            device->client = NULL;
            HeapFree(GetProcessHeap(), 0, This->pdscbd);
            This->device->capture_buffer = 0;
            HeapFree( GetProcessHeap(), 0, This );
            return err;
        }

        buflen = lpcDSCBufferDesc->dwBufferBytes;
        TRACE("desired buflen=%d, old buffer=%p\n", buflen, device->buffer);
        if (device->buffer)
            newbuf = HeapReAlloc(GetProcessHeap(),0,device->buffer,buflen);
        else
            newbuf = HeapAlloc(GetProcessHeap(),0,buflen);
        if (newbuf == NULL) {
            IAudioClient_Release(device->client);
            device->client = NULL;
            IAudioCaptureClient_Release(device->capture);
            device->capture = NULL;
            HeapFree(GetProcessHeap(), 0, This->pdscbd);
            This->device->capture_buffer = 0;
            HeapFree( GetProcessHeap(), 0, This );
            return DSERR_OUTOFMEMORY;
        }
        device->buffer = newbuf;
        device->buflen = buflen;
    }

    *ppobj = This;

    TRACE("returning DS_OK\n");
    return DS_OK;
}


/*******************************************************************************
 * DirectSoundCaptureDevice
 */
static HRESULT DirectSoundCaptureDevice_Create(
    DirectSoundCaptureDevice ** ppDevice)
{
    DirectSoundCaptureDevice * device;
    TRACE("(%p)\n", ppDevice);

    /* Allocate memory */
    device = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(DirectSoundCaptureDevice));

    if (device == NULL) {
	WARN("out of memory\n");
        return DSERR_OUTOFMEMORY;
    }

    device->ref = 1;
    device->state = STATE_STOPPED;

    InitializeCriticalSection( &(device->lock) );
    device->lock.DebugInfo->Spare[0] = (DWORD_PTR)(__FILE__ ": DirectSoundCaptureDevice.lock");

    *ppDevice = device;

    return DS_OK;
}

static ULONG DirectSoundCaptureDevice_Release(
    DirectSoundCaptureDevice * device)
{
    ULONG ref = InterlockedDecrement(&(device->ref));
    TRACE("(%p) ref was %d\n", device, ref + 1);

    if (!ref) {
        TRACE("deleting object\n");

        timeKillEvent(device->timerID);
        timeEndPeriod(DS_TIME_RES);

        EnterCriticalSection(&DSOUND_capturers_lock);
        list_remove(&device->entry);
        LeaveCriticalSection(&DSOUND_capturers_lock);

        if (device->capture_buffer)
            IDirectSoundCaptureBufferImpl_Release(
		(LPDIRECTSOUNDCAPTUREBUFFER8) device->capture_buffer);

        if(device->mmdevice)
            IMMDevice_Release(device->mmdevice);
        HeapFree(GetProcessHeap(), 0, device->pwfx);
        device->lock.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection( &(device->lock) );
        HeapFree(GetProcessHeap(), 0, device);
	TRACE("(%p) released\n", device);
    }
    return ref;
}

static void CALLBACK DSOUND_capture_timer(UINT timerID, UINT msg, DWORD_PTR user,
                                          DWORD_PTR dw1, DWORD_PTR dw2)
{
    DirectSoundCaptureDevice *device = (DirectSoundCaptureDevice*)user;
    UINT32 packet_frames, packet_bytes, avail_bytes;
    DWORD flags;
    BYTE *buf;
    HRESULT hr;

    if(!device->ref)
        return;

    EnterCriticalSection(&device->lock);

    if(!device->capture_buffer || device->state == STATE_STOPPED){
        LeaveCriticalSection(&device->lock);
        return;
    }

    if(device->state == STATE_STOPPING){
        device->state = STATE_STOPPED;
        LeaveCriticalSection(&device->lock);
        return;
    }

    if(device->state == STATE_STARTING)
        device->state = STATE_CAPTURING;

    hr = IAudioCaptureClient_GetBuffer(device->capture, &buf, &packet_frames,
            &flags, NULL, NULL);
    if(FAILED(hr)){
        LeaveCriticalSection(&device->lock);
        WARN("GetBuffer failed: %08x\n", hr);
        return;
    }

    packet_bytes = packet_frames * device->pwfx->nBlockAlign;

    avail_bytes = device->buflen - device->write_pos_bytes;
    if(avail_bytes > packet_bytes)
        avail_bytes = packet_bytes;

    memcpy(device->buffer + device->write_pos_bytes, buf, avail_bytes);
    capture_CheckNotify(device->capture_buffer, device->write_pos_bytes, avail_bytes);

    packet_bytes -= avail_bytes;
    if(packet_bytes > 0){
        if(device->capture_buffer->flags & DSCBSTART_LOOPING){
            memcpy(device->buffer, buf + avail_bytes, packet_bytes);
            capture_CheckNotify(device->capture_buffer, 0, packet_bytes);
        }else{
            device->state = STATE_STOPPED;
            capture_CheckNotify(device->capture_buffer, 0, 0);
        }
    }

    device->write_pos_bytes += avail_bytes + packet_bytes;
    device->write_pos_bytes %= device->buflen;

    hr = IAudioCaptureClient_ReleaseBuffer(device->capture, packet_frames);
    if(FAILED(hr)){
        LeaveCriticalSection(&device->lock);
        WARN("ReleaseBuffer failed: %08x\n", hr);
        return;
    }

    LeaveCriticalSection(&device->lock);
}

static struct _TestFormat {
    DWORD flag;
    DWORD rate;
    DWORD depth;
    WORD channels;
} formats_to_test[] = {
    { WAVE_FORMAT_1M08, 11025, 8, 1 },
    { WAVE_FORMAT_1M16, 11025, 16, 1 },
    { WAVE_FORMAT_1S08, 11025, 8, 2 },
    { WAVE_FORMAT_1S16, 11025, 16, 2 },
    { WAVE_FORMAT_2M08, 22050, 8, 1 },
    { WAVE_FORMAT_2M16, 22050, 16, 1 },
    { WAVE_FORMAT_2S08, 22050, 8, 2 },
    { WAVE_FORMAT_2S16, 22050, 16, 2 },
    { WAVE_FORMAT_4M08, 44100, 8, 1 },
    { WAVE_FORMAT_4M16, 44100, 16, 1 },
    { WAVE_FORMAT_4S08, 44100, 8, 2 },
    { WAVE_FORMAT_4S16, 44100, 16, 2 },
    { WAVE_FORMAT_48M08, 48000, 8, 1 },
    { WAVE_FORMAT_48M16, 48000, 16, 1 },
    { WAVE_FORMAT_48S08, 48000, 8, 2 },
    { WAVE_FORMAT_48S16, 48000, 16, 2 },
    { WAVE_FORMAT_96M08, 96000, 8, 1 },
    { WAVE_FORMAT_96M16, 96000, 16, 1 },
    { WAVE_FORMAT_96S08, 96000, 8, 2 },
    { WAVE_FORMAT_96S16, 96000, 16, 2 },
    {0}
};

static HRESULT DirectSoundCaptureDevice_Initialize(
    DirectSoundCaptureDevice ** ppDevice,
    LPCGUID lpcGUID)
{
    HRESULT hr;
    GUID devGUID;
    IMMDevice *mmdevice;
    struct _TestFormat *fmt;
    DirectSoundCaptureDevice *device;
    IAudioClient *client;

    TRACE("(%p, %s)\n", ppDevice, debugstr_guid(lpcGUID));

    /* Default device? */
    if ( !lpcGUID || IsEqualGUID(lpcGUID, &GUID_NULL) )
        lpcGUID = &DSDEVID_DefaultCapture;

    if(IsEqualGUID(lpcGUID, &DSDEVID_DefaultPlayback) ||
            IsEqualGUID(lpcGUID, &DSDEVID_DefaultVoicePlayback))
        return DSERR_NODRIVER;

    if (GetDeviceID(lpcGUID, &devGUID) != DS_OK) {
        WARN("invalid parameter: lpcGUID\n");
        return DSERR_INVALIDPARAM;
    }

    hr = get_mmdevice(eCapture, &devGUID, &mmdevice);
    if(FAILED(hr))
        return hr;

    EnterCriticalSection(&DSOUND_capturers_lock);

    LIST_FOR_EACH_ENTRY(device, &DSOUND_capturers, DirectSoundCaptureDevice, entry){
        if(IsEqualGUID(&device->guid, &devGUID)){
            IMMDevice_Release(mmdevice);
            LeaveCriticalSection(&DSOUND_capturers_lock);
            return DSERR_ALLOCATED;
        }
    }

    hr = DirectSoundCaptureDevice_Create(&device);
    if (hr != DS_OK) {
        WARN("DirectSoundCaptureDevice_Create failed\n");
        LeaveCriticalSection(&DSOUND_capturers_lock);
        return hr;
    }

    device->guid = devGUID;

    device->mmdevice = mmdevice;

    device->drvcaps.dwFlags = 0;

    device->drvcaps.dwFormats = 0;
    device->drvcaps.dwChannels = 0;
    hr = IMMDevice_Activate(mmdevice, &IID_IAudioClient,
            CLSCTX_INPROC_SERVER, NULL, (void**)&client);
    if(FAILED(hr)){
        device->lock.DebugInfo->Spare[0] = 0;
        DeleteCriticalSection(&device->lock);
        HeapFree(GetProcessHeap(), 0, device);
        LeaveCriticalSection(&DSOUND_capturers_lock);
        return DSERR_NODRIVER;
    }

    for(fmt = formats_to_test; fmt->flag; ++fmt){
        if(DSOUND_check_supported(client, fmt->rate, fmt->depth, fmt->channels)){
            device->drvcaps.dwFormats |= fmt->flag;
            if(fmt->channels > device->drvcaps.dwChannels)
                device->drvcaps.dwChannels = fmt->channels;
        }
    }
    IAudioClient_Release(client);

    device->timerID = DSOUND_create_timer(DSOUND_capture_timer, (DWORD_PTR)device);

    list_add_tail(&DSOUND_capturers, &device->entry);

    *ppDevice = device;

    LeaveCriticalSection(&DSOUND_capturers_lock);

    return S_OK;
}


/*****************************************************************************
 * IDirectSoundCapture implementation structure
 */
struct IDirectSoundCaptureImpl
{
    IDirectSoundCapture      IDirectSoundCapture_iface;
    LONG                     ref;
    DirectSoundCaptureDevice *device;
};

static inline struct IDirectSoundCaptureImpl *impl_from_IDirectSoundCapture(IDirectSoundCapture *iface)
{
    return CONTAINING_RECORD(iface, struct IDirectSoundCaptureImpl, IDirectSoundCapture_iface);
}

/***************************************************************************
 * IDirectSoundCaptureImpl
 */
static HRESULT WINAPI IDirectSoundCaptureImpl_QueryInterface(IDirectSoundCapture *iface,
        REFIID riid, void **ppobj)
{
    TRACE( "(%p,%s,%p)\n", iface, debugstr_guid(riid), ppobj );

    if (ppobj == NULL) {
	WARN("invalid parameter\n");
	return E_INVALIDARG;
    }

    *ppobj = NULL;

    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IDirectSoundCapture)) {
        IDirectSoundCapture_AddRef(iface);
        *ppobj = iface;
        return S_OK;
    }

    WARN("unsupported riid: %s\n", debugstr_guid(riid));
    return E_NOINTERFACE;
}

static ULONG WINAPI IDirectSoundCaptureImpl_AddRef(IDirectSoundCapture *iface)
{
    IDirectSoundCaptureImpl *This = impl_from_IDirectSoundCapture(iface);
    ULONG ref = InterlockedIncrement(&(This->ref));

    TRACE("(%p) ref was %d\n", This, ref - 1);
    return ref;
}

static ULONG WINAPI IDirectSoundCaptureImpl_Release(IDirectSoundCapture *iface)
{
    IDirectSoundCaptureImpl *This = impl_from_IDirectSoundCapture(iface);
    ULONG ref = InterlockedDecrement(&(This->ref));

    TRACE("(%p) ref was %d\n", This, ref + 1);

    if (!ref) {
        if (This->device)
            DirectSoundCaptureDevice_Release(This->device);

        HeapFree( GetProcessHeap(), 0, This );
        TRACE("(%p) released\n", This);
    }
    return ref;
}

static HRESULT WINAPI IDirectSoundCaptureImpl_CreateCaptureBuffer(IDirectSoundCapture *iface,
        LPCDSCBUFFERDESC lpcDSCBufferDesc, IDirectSoundCaptureBuffer **lplpDSCaptureBuffer,
        IUnknown *pUnk)
{
    IDirectSoundCaptureImpl *This = impl_from_IDirectSoundCapture(iface);
    HRESULT hr;

    TRACE( "(%p,%p,%p,%p)\n",iface,lpcDSCBufferDesc,lplpDSCaptureBuffer,pUnk);

    if (lpcDSCBufferDesc == NULL) {
	WARN("invalid parameter: lpcDSCBufferDesc == NULL)\n");
	return DSERR_INVALIDPARAM;
    }

    if (lplpDSCaptureBuffer == NULL) {
	WARN("invalid parameter: lplpDSCaptureBuffer == NULL\n");
	return DSERR_INVALIDPARAM;
    }

    if (pUnk != NULL) {
	WARN("invalid parameter: pUnk != NULL\n");
	return DSERR_INVALIDPARAM;
    }

    /* FIXME: We can only have one buffer so what do we do here? */
    if (This->device->capture_buffer) {
	WARN("invalid parameter: already has buffer\n");
	return DSERR_INVALIDPARAM;    /* DSERR_GENERIC ? */
    }

    hr = IDirectSoundCaptureBufferImpl_Create(This->device,
        (IDirectSoundCaptureBufferImpl **)lplpDSCaptureBuffer, lpcDSCBufferDesc);

    if (hr != DS_OK)
	WARN("IDirectSoundCaptureBufferImpl_Create failed\n");

    return hr;
}

static HRESULT WINAPI IDirectSoundCaptureImpl_GetCaps(IDirectSoundCapture *iface,
        LPDSCCAPS lpDSCCaps)
{
    IDirectSoundCaptureImpl *This = impl_from_IDirectSoundCapture(iface);

    TRACE("(%p,%p)\n",This,lpDSCCaps);

    if (This->device == NULL) {
	WARN("not initialized\n");
	return DSERR_UNINITIALIZED;
    }

    if (lpDSCCaps== NULL) {
	WARN("invalid parameter: lpDSCCaps== NULL\n");
	return DSERR_INVALIDPARAM;
    }

    if (lpDSCCaps->dwSize < sizeof(*lpDSCCaps)) {
	WARN("invalid parameter: lpDSCCaps->dwSize = %d\n", lpDSCCaps->dwSize);
	return DSERR_INVALIDPARAM;
    }

    lpDSCCaps->dwFlags = This->device->drvcaps.dwFlags;
    lpDSCCaps->dwFormats = This->device->drvcaps.dwFormats;
    lpDSCCaps->dwChannels = This->device->drvcaps.dwChannels;

    TRACE("(flags=0x%08x,format=0x%08x,channels=%d)\n",lpDSCCaps->dwFlags,
        lpDSCCaps->dwFormats, lpDSCCaps->dwChannels);

    return DS_OK;
}

static HRESULT WINAPI IDirectSoundCaptureImpl_Initialize(IDirectSoundCapture *iface,
        LPCGUID lpcGUID)
{
    IDirectSoundCaptureImpl *This = impl_from_IDirectSoundCapture(iface);

    TRACE("(%p,%s)\n", This, debugstr_guid(lpcGUID));

    if (This->device != NULL) {
	WARN("already initialized\n");
	return DSERR_ALREADYINITIALIZED;
    }
    return DirectSoundCaptureDevice_Initialize(&This->device, lpcGUID);
}

static const IDirectSoundCaptureVtbl dscvt =
{
    /* IUnknown methods */
    IDirectSoundCaptureImpl_QueryInterface,
    IDirectSoundCaptureImpl_AddRef,
    IDirectSoundCaptureImpl_Release,

    /* IDirectSoundCapture methods */
    IDirectSoundCaptureImpl_CreateCaptureBuffer,
    IDirectSoundCaptureImpl_GetCaps,
    IDirectSoundCaptureImpl_Initialize
};

static HRESULT IDirectSoundCaptureImpl_Create(
    LPDIRECTSOUNDCAPTURE8 * ppDSC)
{
    IDirectSoundCaptureImpl *pDSC;
    TRACE("(%p)\n", ppDSC);

    /* Allocate memory */
    pDSC = HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(IDirectSoundCaptureImpl));
    if (pDSC == NULL) {
        WARN("out of memory\n");
        *ppDSC = NULL;
        return DSERR_OUTOFMEMORY;
    }

    pDSC->IDirectSoundCapture_iface.lpVtbl = &dscvt;
    pDSC->ref    = 0;
    pDSC->device = NULL;

    *ppDSC = (LPDIRECTSOUNDCAPTURE8)pDSC;

    return DS_OK;
}

HRESULT DSOUND_CaptureCreate(REFIID riid, IDirectSoundCapture **ppDSC)
{
    IDirectSoundCapture *pDSC;
    HRESULT hr;
    TRACE("(%s, %p)\n", debugstr_guid(riid), ppDSC);

    if (!IsEqualIID(riid, &IID_IUnknown) &&
        !IsEqualIID(riid, &IID_IDirectSoundCapture)) {
        *ppDSC = 0;
        return E_NOINTERFACE;
    }

    /* Get dsound configuration */
    setup_dsound_options();

    hr = IDirectSoundCaptureImpl_Create(&pDSC);
    if (hr == DS_OK) {
        IDirectSoundCapture_AddRef(pDSC);
        *ppDSC = pDSC;
    } else {
        WARN("IDirectSoundCaptureImpl_Create failed\n");
        *ppDSC = 0;
    }

    return hr;
}

HRESULT DSOUND_CaptureCreate8(
    REFIID riid,
    LPDIRECTSOUNDCAPTURE8 *ppDSC8)
{
    LPDIRECTSOUNDCAPTURE8 pDSC8;
    HRESULT hr;
    TRACE("(%s, %p)\n", debugstr_guid(riid), ppDSC8);

    if (!IsEqualIID(riid, &IID_IUnknown) &&
        !IsEqualIID(riid, &IID_IDirectSoundCapture8)) {
        *ppDSC8 = 0;
        return E_NOINTERFACE;
    }

    /* Get dsound configuration */
    setup_dsound_options();

    hr = IDirectSoundCaptureImpl_Create(&pDSC8);
    if (hr == DS_OK) {
        IDirectSoundCapture_AddRef(pDSC8);
        *ppDSC8 = pDSC8;
    } else {
        WARN("IDirectSoundCaptureImpl_Create failed\n");
        *ppDSC8 = 0;
    }

    return hr;
}

/***************************************************************************
 * DirectSoundCaptureCreate [DSOUND.6]
 *
 * Create and initialize a DirectSoundCapture interface.
 *
 * PARAMS
 *    lpcGUID   [I] Address of the GUID that identifies the sound capture device.
 *    lplpDSC   [O] Address of a variable to receive the interface pointer.
 *    pUnkOuter [I] Must be NULL.
 *
 * RETURNS
 *    Success: DS_OK
 *    Failure: DSERR_NOAGGREGATION, DSERR_ALLOCATED, DSERR_INVALIDPARAM,
 *             DSERR_OUTOFMEMORY
 *
 * NOTES
 *    lpcGUID must be one of the values returned from DirectSoundCaptureEnumerate
 *    or NULL for the default device or DSDEVID_DefaultCapture or
 *    DSDEVID_DefaultVoiceCapture.
 *
 *    DSERR_ALLOCATED is returned for sound devices that do not support full duplex.
 */
HRESULT WINAPI DirectSoundCaptureCreate(LPCGUID lpcGUID, IDirectSoundCapture **ppDSC,
        IUnknown *pUnkOuter)
{
    HRESULT hr;
    IDirectSoundCapture *pDSC;

    TRACE("(%s,%p,%p)\n", debugstr_guid(lpcGUID), ppDSC, pUnkOuter);

    if (ppDSC == NULL) {
	WARN("invalid parameter: ppDSC == NULL\n");
        return DSERR_INVALIDPARAM;
    }

    if (pUnkOuter) {
	WARN("invalid parameter: pUnkOuter != NULL\n");
        *ppDSC = NULL;
        return DSERR_NOAGGREGATION;
    }

    hr = DSOUND_CaptureCreate(&IID_IDirectSoundCapture, &pDSC);
    if (hr == DS_OK) {
        hr = IDirectSoundCapture_Initialize(pDSC, lpcGUID);
        if (hr != DS_OK) {
            IDirectSoundCapture_Release(pDSC);
            pDSC = 0;
        }
    }

    *ppDSC = pDSC;

    return hr;
}

/***************************************************************************
 * DirectSoundCaptureCreate8 [DSOUND.12]
 *
 * Create and initialize a DirectSoundCapture interface.
 *
 * PARAMS
 *    lpcGUID   [I] Address of the GUID that identifies the sound capture device.
 *    lplpDSC   [O] Address of a variable to receive the interface pointer.
 *    pUnkOuter [I] Must be NULL.
 *
 * RETURNS
 *    Success: DS_OK
 *    Failure: DSERR_NOAGGREGATION, DSERR_ALLOCATED, DSERR_INVALIDPARAM,
 *             DSERR_OUTOFMEMORY
 *
 * NOTES
 *    lpcGUID must be one of the values returned from DirectSoundCaptureEnumerate
 *    or NULL for the default device or DSDEVID_DefaultCapture or
 *    DSDEVID_DefaultVoiceCapture.
 *
 *    DSERR_ALLOCATED is returned for sound devices that do not support full duplex.
 */
HRESULT WINAPI DirectSoundCaptureCreate8(
    LPCGUID lpcGUID,
    LPDIRECTSOUNDCAPTURE8 *ppDSC8,
    LPUNKNOWN pUnkOuter)
{
    HRESULT hr;
    LPDIRECTSOUNDCAPTURE8 pDSC8;
    TRACE("(%s,%p,%p)\n", debugstr_guid(lpcGUID), ppDSC8, pUnkOuter);

    if (ppDSC8 == NULL) {
	WARN("invalid parameter: ppDSC8 == NULL\n");
        return DSERR_INVALIDPARAM;
    }

    if (pUnkOuter) {
	WARN("invalid parameter: pUnkOuter != NULL\n");
        *ppDSC8 = NULL;
        return DSERR_NOAGGREGATION;
    }

    hr = DSOUND_CaptureCreate8(&IID_IDirectSoundCapture8, &pDSC8);
    if (hr == DS_OK) {
        hr = IDirectSoundCapture_Initialize(pDSC8, lpcGUID);
        if (hr != DS_OK) {
            IDirectSoundCapture_Release(pDSC8);
            pDSC8 = 0;
        }
    }

    *ppDSC8 = pDSC8;

    return hr;
}

/* -*- tab-width: 8; c-basic-offset: 4 -*- */
/*				   
 * Sample Wine Driver for MCI wave forms
 *
 * Copyright 	1994 Martin Ayotte
 *		1999 Eric Pouech
 */
/*
 * FIXME:
 *	- record/play should and must be done asynchronous
 *	- segmented/linear pointer problems (lpData in waveheaders,W*_DONE cbs)
 */

#include "winuser.h"
#include "driver.h"
#include "mmddk.h"
#include "heap.h"
#include "debugtools.h"

DEFAULT_DEBUG_CHANNEL(mciwave)

typedef struct {
    UINT			wDevID;
    HANDLE			hWave;
    int				nUseCount;	/* Incremented for each shared open */
    BOOL			fShareable;	/* TRUE if first open was shareable */
    WORD			wNotifyDeviceID;/* MCI device ID with a pending notification */
    HANDLE			hCallback;	/* Callback handle for pending notification */
    HMMIO			hFile;		/* mmio file handle open as Element */
    MCI_WAVE_OPEN_PARMSA 	openParms;
    WAVEOPENDESC 		waveDesc;
    WAVEFORMATEX		WaveFormat;
    WAVEHDR			WaveHdr;
    BOOL			fInput;		/* FALSE = Output, TRUE = Input */
    WORD			dwStatus;	/* one from MCI_MODE_xxxx */
    DWORD			dwMciTimeFormat;/* One of the supported MCI_FORMAT_xxxx */
    DWORD			dwFileOffset;   /* Offset of chunk in mmio file */
    DWORD			dwLength;	/* number of bytes in chunk for playing */
    DWORD			dwPosition;	/* position in bytes in chunk for playing */
} WINE_MCIWAVE;

/* ===================================================================
 * ===================================================================
 * FIXME: should be using the new mmThreadXXXX functions from WINMM
 * instead of those
 * it would require to add a wine internal flag to mmThreadCreate
 * in order to pass a 32 bit function instead of a 16 bit
 * ===================================================================
 * =================================================================== */

struct SCA {
    UINT 	wDevID;
    UINT 	wMsg;
    DWORD 	dwParam1;
    DWORD 	dwParam2;
    BOOL	allocatedCopy;
};

/* EPP DWORD WINAPI mciSendCommandA(UINT wDevID, UINT wMsg, DWORD dwParam1, DWORD dwParam2); */

/**************************************************************************
 * 				MCI_SCAStarter			[internal]
 */
static DWORD CALLBACK	MCI_SCAStarter(LPVOID arg)
{
    struct SCA*	sca = (struct SCA*)arg;
    DWORD		ret;

    TRACE("In thread before async command (%08x,%u,%08lx,%08lx)\n",
	  sca->wDevID, sca->wMsg, sca->dwParam1, sca->dwParam2);
    ret = mciSendCommandA(sca->wDevID, sca->wMsg, sca->dwParam1 | MCI_WAIT, sca->dwParam2);
    TRACE("In thread after async command (%08x,%u,%08lx,%08lx)\n",
	  sca->wDevID, sca->wMsg, sca->dwParam1, sca->dwParam2);
    if (sca->allocatedCopy)
	HeapFree(GetProcessHeap(), 0, (LPVOID)sca->dwParam2);
    HeapFree(GetProcessHeap(), 0, sca);
    ExitThread(ret);
    WARN("Should not happen ? what's wrong \n");
    /* should not go after this point */
    return ret;
}

/**************************************************************************
 * 				MCI_SendCommandAsync		[internal]
 */
static	DWORD MCI_SendCommandAsync(UINT wDevID, UINT wMsg, DWORD dwParam1, 
				   DWORD dwParam2, UINT size)
{
    struct SCA*	sca = HeapAlloc(GetProcessHeap(), 0, sizeof(struct SCA));

    if (sca == 0)
	return MCIERR_OUT_OF_MEMORY;

    sca->wDevID   = wDevID;
    sca->wMsg     = wMsg;
    sca->dwParam1 = dwParam1;
    
    if (size) {
	sca->dwParam2 = (DWORD)HeapAlloc(GetProcessHeap(), 0, size);
	if (sca->dwParam2 == 0) {
	    HeapFree(GetProcessHeap(), 0, sca);
	    return MCIERR_OUT_OF_MEMORY;
	}
	sca->allocatedCopy = TRUE;
	/* copy structure passed by program in dwParam2 to be sure 
	 * we can still use it whatever the program does 
	 */
	memcpy((LPVOID)sca->dwParam2, (LPVOID)dwParam2, size);
    } else {
	sca->dwParam2 = dwParam2;
	sca->allocatedCopy = FALSE;
    }

    if (CreateThread(NULL, 0, MCI_SCAStarter, sca, 0, NULL) == 0) {
	WARN("Couldn't allocate thread for async command handling, sending synchonously\n");
	return MCI_SCAStarter(&sca);
    }
    return 0;
}

/*======================================================================*
 *                  	    MCI WAVE implemantation			*
 *======================================================================*/

static DWORD WAVE_mciResume(UINT wDevID, DWORD dwFlags, LPMCI_GENERIC_PARMS lpParms);

/**************************************************************************
 * 				MCIWAVE_drvOpen			[internal]	
 */
static	DWORD	WAVE_drvOpen(LPSTR str, LPMCI_OPEN_DRIVER_PARMSA modp)
{
    WINE_MCIWAVE*	wmw = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(WINE_MCIWAVE));

    if (!wmw)
	return 0;

    wmw->wDevID = modp->wDeviceID;
    mciSetDriverData(wmw->wDevID, (DWORD)wmw);
    modp->wCustomCommandTable = MCI_NO_COMMAND_TABLE;
    modp->wType = MCI_DEVTYPE_WAVEFORM_AUDIO;
    return modp->wDeviceID;
}

/**************************************************************************
 * 				MCIWAVE_drvClose		[internal]	
 */
static	DWORD	WAVE_drvClose(DWORD dwDevID)
{
    WINE_MCIWAVE*  wmw = (WINE_MCIWAVE*)mciGetDriverData(dwDevID);

    if (wmw) {
	HeapFree(GetProcessHeap(), 0, wmw);	
	mciSetDriverData(dwDevID, 0);
	return 1;
    }
    return 0;
}

/**************************************************************************
 * 				WAVE_mciGetOpenDev		[internal]	
 */
static WINE_MCIWAVE*  WAVE_mciGetOpenDev(UINT wDevID)
{
    WINE_MCIWAVE*	wmw = (WINE_MCIWAVE*)mciGetDriverData(wDevID);
    
    if (wmw == NULL || wmw->nUseCount == 0) {
	WARN("Invalid wDevID=%u\n", wDevID);
	return 0;
    }
    return wmw;
}

/**************************************************************************
 * 				WAVE_ConvertByteToTimeFormat	[internal]	
 */
static	DWORD 	WAVE_ConvertByteToTimeFormat(WINE_MCIWAVE* wmw, DWORD val, LPDWORD lpRet)
{
    DWORD	ret = 0;
    
    switch (wmw->dwMciTimeFormat) {
    case MCI_FORMAT_MILLISECONDS:
	ret = (val * 1000) / wmw->WaveFormat.nAvgBytesPerSec;
	break;
    case MCI_FORMAT_BYTES:
	ret = val;
	break;
    case MCI_FORMAT_SAMPLES: /* FIXME: is this correct ? */
	ret = (val * 8) / wmw->WaveFormat.wBitsPerSample;
	break;
    default:
	WARN("Bad time format %lu!\n", wmw->dwMciTimeFormat);
    }
    TRACE("val=%lu=0x%08lx [tf=%lu] => ret=%lu\n", val, val, wmw->dwMciTimeFormat, ret);
    *lpRet = 0;
    return ret;
}

/**************************************************************************
 * 				WAVE_ConvertTimeFormatToByte	[internal]	
 */
static	DWORD 	WAVE_ConvertTimeFormatToByte(WINE_MCIWAVE* wmw, DWORD val)
{
    DWORD	ret = 0;
    
    switch (wmw->dwMciTimeFormat) {
    case MCI_FORMAT_MILLISECONDS:
	ret = (val * wmw->WaveFormat.nAvgBytesPerSec) / 1000;
	break;
    case MCI_FORMAT_BYTES:
	ret = val;
	break;
    case MCI_FORMAT_SAMPLES: /* FIXME: is this correct ? */
	ret = (val * wmw->WaveFormat.wBitsPerSample) / 8;
	break;
    default:
	WARN("Bad time format %lu!\n", wmw->dwMciTimeFormat);
    }
    TRACE("val=%lu=0x%08lx [tf=%lu] => ret=%lu\n", val, val, wmw->dwMciTimeFormat, ret);
    return ret;
}

/**************************************************************************
 * 			WAVE_mciReadFmt	                        [internal]
 */
static	DWORD WAVE_mciReadFmt(WINE_MCIWAVE* wmw, MMCKINFO* pckMainRIFF)
{
    MMCKINFO	mmckInfo;
    
    mmckInfo.ckid = mmioFOURCC('f', 'm', 't', ' ');
    if (mmioDescend(wmw->hFile, &mmckInfo, pckMainRIFF, MMIO_FINDCHUNK) != 0)
	return MCIERR_INVALID_FILE;
    TRACE("Chunk Found ckid=%.4s fccType=%.4s cksize=%08lX \n",
	  (LPSTR)&mmckInfo.ckid, (LPSTR)&mmckInfo.fccType, mmckInfo.cksize);
    if (mmioRead(wmw->hFile, (HPSTR)&wmw->WaveFormat,
		 (long)sizeof(PCMWAVEFORMAT)) != (long)sizeof(PCMWAVEFORMAT))
	return MCIERR_INVALID_FILE;
    
    TRACE("wFormatTag=%04X !\n",   wmw->WaveFormat.wFormatTag);
    TRACE("nChannels=%d \n",       wmw->WaveFormat.nChannels);
    TRACE("nSamplesPerSec=%ld\n",  wmw->WaveFormat.nSamplesPerSec);
    TRACE("nAvgBytesPerSec=%ld\n", wmw->WaveFormat.nAvgBytesPerSec);
    TRACE("nBlockAlign=%d \n",     wmw->WaveFormat.nBlockAlign);
    TRACE("wBitsPerSample=%u !\n", wmw->WaveFormat.wBitsPerSample);

    mmckInfo.ckid = mmioFOURCC('d', 'a', 't', 'a');
    mmioSeek(wmw->hFile, mmckInfo.dwDataOffset + ((mmckInfo.cksize + 1) & ~1), SEEK_SET);
    if (mmioDescend(wmw->hFile, &mmckInfo, pckMainRIFF, MMIO_FINDCHUNK) != 0) {
	TRACE("can't find data chunk\n");
	return MCIERR_INVALID_FILE;
    }
    TRACE("Chunk Found ckid=%.4s fccType=%.4s cksize=%08lX \n",
	  (LPSTR)&mmckInfo.ckid, (LPSTR)&mmckInfo.fccType, mmckInfo.cksize);
    TRACE("nChannels=%d nSamplesPerSec=%ld\n",
	  wmw->WaveFormat.nChannels, wmw->WaveFormat.nSamplesPerSec);
    wmw->dwLength = mmckInfo.cksize;
    wmw->dwFileOffset = mmioSeek(wmw->hFile, 0, SEEK_CUR); /* >= 0 */
    return 0;
}

/**************************************************************************
 * 			WAVE_mciOpen	                        [internal]
 */
static DWORD WAVE_mciOpen(UINT wDevID, DWORD dwFlags, LPMCI_WAVE_OPEN_PARMSA lpOpenParms)
{
    DWORD		dwRet = 0;
    DWORD		dwDeviceID;
    WINE_MCIWAVE*	wmw = (WINE_MCIWAVE*)mciGetDriverData(wDevID);
    
    TRACE("(%04X, %08lX, %p)\n", wDevID, dwFlags, lpOpenParms);
    if (lpOpenParms == NULL)	return MCIERR_NULL_PARAMETER_BLOCK;
    if (wmw == NULL) 		return MCIERR_INVALID_DEVICE_ID;

    if (dwFlags & MCI_OPEN_SHAREABLE)
	return MCIERR_HARDWARE;
    
    if (wmw->nUseCount > 0) {
	/* The driver is already opened on this channel
	 * Wave driver cannot be shared
	 */
	return MCIERR_DEVICE_OPEN;
    }
    wmw->nUseCount++;
    
    dwDeviceID = lpOpenParms->wDeviceID;
    
    wmw->fInput = FALSE;
    wmw->hWave = 0;

    TRACE("wDevID=%04X (lpParams->wDeviceID=%08lX)\n", wDevID, dwDeviceID);
    
    if (dwFlags & MCI_OPEN_ELEMENT) {
	if (dwFlags & MCI_OPEN_ELEMENT_ID) {
	    /* could it be that (DWORD)lpOpenParms->lpstrElementName 
	     * contains the hFile value ? 
	     */
	    dwRet = MCIERR_UNRECOGNIZED_COMMAND;
	} else {
	    LPCSTR	lpstrElementName = lpOpenParms->lpstrElementName;
	    
	    /*FIXME : what should be done id wmw->hFile is already != 0, or the driver is playin' */
	    TRACE("MCI_OPEN_ELEMENT '%s' !\n", lpstrElementName);
	    if (lpstrElementName && (strlen(lpstrElementName) > 0)) {
		wmw->hFile = mmioOpenA((LPSTR)lpstrElementName, NULL, 
				       MMIO_ALLOCBUF | MMIO_READ | MMIO_DENYWRITE);
		if (wmw->hFile == 0) {
		    WARN("can't find file='%s' !\n", lpstrElementName);
		    dwRet = MCIERR_FILE_NOT_FOUND;
		}
	    } else {
		wmw->hFile = 0;
	    }
	}
    }
    TRACE("hFile=%u\n", wmw->hFile);
    
    memcpy(&wmw->openParms, lpOpenParms, sizeof(MCI_WAVE_OPEN_PARMSA));
    wmw->wNotifyDeviceID = dwDeviceID;
    wmw->dwStatus = MCI_MODE_NOT_READY;	/* while loading file contents */
    
    wmw->waveDesc.hWave = 0;
    
    if (dwRet == 0 && wmw->hFile != 0) {
	MMCKINFO	ckMainRIFF;
	
	if (mmioDescend(wmw->hFile, &ckMainRIFF, NULL, 0) != 0) {
	    dwRet = MCIERR_INVALID_FILE;
	} else {
	    TRACE("ParentChunk ckid=%.4s fccType=%.4s cksize=%08lX \n",
		  (LPSTR)&ckMainRIFF.ckid, (LPSTR)&ckMainRIFF.fccType, ckMainRIFF.cksize);
	    if ((ckMainRIFF.ckid != FOURCC_RIFF) ||
		(ckMainRIFF.fccType != mmioFOURCC('W', 'A', 'V', 'E'))) {
		dwRet = MCIERR_INVALID_FILE;
	    } else {
		dwRet = WAVE_mciReadFmt(wmw, &ckMainRIFF);
	    }
	}
    } else {
	wmw->dwLength = 0;
    }
    if (dwRet == 0) {
	wmw->WaveFormat.nAvgBytesPerSec = 
	    wmw->WaveFormat.nSamplesPerSec * wmw->WaveFormat.nBlockAlign;
	wmw->waveDesc.lpFormat = &wmw->WaveFormat;
	wmw->dwPosition = 0;
	
	wmw->dwStatus = MCI_MODE_STOP;
    } else {
	wmw->nUseCount--;
	if (wmw->hFile != 0)
	    mmioClose(wmw->hFile, 0);
	wmw->hFile = 0;
    }
    return dwRet;
}

/**************************************************************************
 *                               WAVE_mciCue             [internal]
 */
static DWORD WAVE_mciCue(UINT wDevID, DWORD dwParam, LPMCI_GENERIC_PARMS lpParms)
{
    /*
      FIXME
      
      This routine is far from complete. At the moment only a check is done on the
      MCI_WAVE_INPUT flag. No explicit check on MCI_WAVE_OUTPUT is done since that
      is the default.
      
      The flags MCI_NOTIFY (and the callback parameter in lpParms) and MCI_WAIT
      are ignored
    */
    
    DWORD		dwRet;
    WINE_MCIWAVE*	wmw = WAVE_mciGetOpenDev(wDevID);
    
    FIXME("(%u, %08lX, %p); likely to fail\n", wDevID, dwParam, lpParms);
    
    if (wmw == NULL)	return MCIERR_INVALID_DEVICE_ID;
    
    /* always close elements ? */    
    if (wmw->hFile != 0) {
	mmioClose(wmw->hFile, 0);
	wmw->hFile = 0;
    }
    
    dwRet = MMSYSERR_NOERROR;  /* assume success */
    
    if ((dwParam & MCI_WAVE_INPUT) && !wmw->fInput) {
	dwRet = waveOutClose(wmw->hWave);
	if (dwRet != MMSYSERR_NOERROR) return MCIERR_INTERNAL;
	wmw->fInput = TRUE;
    } else if (wmw->fInput) {
	dwRet = waveInClose(wmw->hWave);
	if (dwRet != MMSYSERR_NOERROR) return MCIERR_INTERNAL;
	wmw->fInput = FALSE;
    }
    wmw->hWave = 0;
    return (dwRet == MMSYSERR_NOERROR) ? 0 : MCIERR_INTERNAL;
}

/**************************************************************************
 * 				WAVE_mciStop			[internal]
 */
static DWORD WAVE_mciStop(UINT wDevID, DWORD dwFlags, LPMCI_GENERIC_PARMS lpParms)
{
    DWORD 		dwRet;
    WINE_MCIWAVE*	wmw = WAVE_mciGetOpenDev(wDevID);
    
    TRACE("(%u, %08lX, %p);\n", wDevID, dwFlags, lpParms);
    
    if (lpParms == NULL)	return MCIERR_NULL_PARAMETER_BLOCK;
    if (wmw == NULL)		return MCIERR_INVALID_DEVICE_ID;
    
    wmw->dwStatus = MCI_MODE_STOP;
    wmw->dwPosition = 0;
    TRACE("wmw->dwStatus=%d\n", wmw->dwStatus);
    
    if (wmw->fInput)
	dwRet = waveInReset(wmw->hWave);
    else
	dwRet = waveOutReset(wmw->hWave);
    
    if (dwFlags & MCI_NOTIFY) {
	TRACE("MCI_NOTIFY_SUCCESSFUL %08lX !\n", lpParms->dwCallback);
	mciDriverNotify((HWND)LOWORD(lpParms->dwCallback), 
			wmw->wNotifyDeviceID, MCI_NOTIFY_SUCCESSFUL);
    }
    
    return (dwRet == MMSYSERR_NOERROR) ? 0 : MCIERR_INTERNAL;
}

/**************************************************************************
 *				WAVE_mciClose		[internal]
 */
static DWORD WAVE_mciClose(UINT wDevID, DWORD dwFlags, LPMCI_GENERIC_PARMS lpParms)
{
    DWORD		dwRet = 0;
    WINE_MCIWAVE*	wmw = WAVE_mciGetOpenDev(wDevID);
    
    TRACE("(%u, %08lX, %p);\n", wDevID, dwFlags, lpParms);
    
    if (wmw == NULL)		return MCIERR_INVALID_DEVICE_ID;
    
    if (wmw->dwStatus != MCI_MODE_STOP) {
	dwRet = WAVE_mciStop(wDevID, MCI_WAIT, lpParms);
    }
    
    wmw->nUseCount--;
    
    if (wmw->nUseCount == 0) {
	DWORD	mmRet;
	if (wmw->hFile != 0) {
	    mmioClose(wmw->hFile, 0);
	    wmw->hFile = 0;
	}
	mmRet = (wmw->fInput) ? waveInClose(wmw->hWave) : waveOutClose(wmw->hWave);
	
	if (mmRet != MMSYSERR_NOERROR) dwRet = MCIERR_INTERNAL;
    }
    
    if ((dwFlags & MCI_NOTIFY) && lpParms) {
	TRACE("MCI_NOTIFY_SUCCESSFUL %08lX !\n", lpParms->dwCallback);
	mciDriverNotify((HWND)LOWORD(lpParms->dwCallback), 
			wmw->wNotifyDeviceID,
			(dwRet == 0) ? MCI_NOTIFY_SUCCESSFUL : MCI_NOTIFY_FAILURE);
    }
    return 0;
}

/**************************************************************************
 * 				WAVE_mciPlay		[internal]
 */
static DWORD WAVE_mciPlay(UINT wDevID, DWORD dwFlags, LPMCI_PLAY_PARMS lpParms)
{
    DWORD		end;
    LONG		bufsize, count;
    HGLOBAL16		hData;
    DWORD		dwRet;
    WINE_MCIWAVE*	wmw = WAVE_mciGetOpenDev(wDevID);
    
    TRACE("(%u, %08lX, %p);\n", wDevID, dwFlags, lpParms);
    
    if (wmw == NULL)		return MCIERR_INVALID_DEVICE_ID;
    if (lpParms == NULL)	return MCIERR_NULL_PARAMETER_BLOCK;
    
    if (wmw->fInput) {
	WARN("cannot play on input device\n");
	return MCIERR_NONAPPLICABLE_FUNCTION;
    }
    
    if (wmw->hFile == 0) {
	WARN("Can't play: no file='%s' !\n", wmw->openParms.lpstrElementName);
	return MCIERR_FILE_NOT_FOUND;
    }
    
    if (!(dwFlags & MCI_WAIT)) {
	return MCI_SendCommandAsync(wmw->wNotifyDeviceID, MCI_PLAY, dwFlags, 
				    (DWORD)lpParms, sizeof(MCI_PLAY_PARMS));
    }

    if (wmw->dwStatus != MCI_MODE_STOP) {
	if (wmw->dwStatus == MCI_MODE_PAUSE) {
	    /* FIXME: parameters (start/end) in lpParams may not be used */
	    return WAVE_mciResume(wDevID, dwFlags, (LPMCI_GENERIC_PARMS)lpParms);
	}
	return MCIERR_INTERNAL;
    }

    end = 0xFFFFFFFF;
    if (lpParms && (dwFlags & MCI_FROM)) {
	wmw->dwPosition = WAVE_ConvertTimeFormatToByte(wmw, lpParms->dwFrom); 
    }
    if (lpParms && (dwFlags & MCI_TO)) {
	end = WAVE_ConvertTimeFormatToByte(wmw, lpParms->dwTo);
    }
    
    TRACE("Playing from byte=%lu to byte=%lu\n", wmw->dwPosition, end);
    
    /* go back to begining of chunk */
    mmioSeek(wmw->hFile, wmw->dwFileOffset, SEEK_SET); /* >= 0 */
    
    /* By default the device will be opened for output, the MCI_CUE function is there to
     * change from output to input and back
     */
    /* FIXME: how to choose between several output channels ? here 0 is forced */
    dwRet = waveOutOpen(&wmw->hWave, WAVE_MAPPER, (LPWAVEFORMATEX)&wmw->WaveFormat, 0L, 0L, CALLBACK_NULL);
    if (dwRet != 0) {
	TRACE("Can't open low level audio device %ld\n", dwRet);
	return MCIERR_DEVICE_OPEN;
    }

    /* at 22050 bytes per sec => 30 ms by block */
    bufsize = 10240;
    hData = GlobalAlloc16(GMEM_MOVEABLE, bufsize);
    wmw->WaveHdr.lpData = (LPSTR)GlobalLock16(hData);
    
    wmw->dwStatus = MCI_MODE_PLAY;
    
    /* FIXME: this doesn't work if wmw->dwPosition != 0 */
    /* FIXME: use several WaveHdr for smoother playback */
    /* FIXME: use only regular MMSYS functions, not calling directly the driver */
    while (wmw->dwStatus != MCI_MODE_STOP) {
	wmw->WaveHdr.dwUser = 0L;
	wmw->WaveHdr.dwFlags = 0L;
	wmw->WaveHdr.dwLoops = 0L;
	count = mmioRead(wmw->hFile, wmw->WaveHdr.lpData, bufsize);
	TRACE("mmioRead bufsize=%ld count=%ld\n", bufsize, count);
	if (count < 1) 
	    break;
	wmw->WaveHdr.dwBufferLength = count;
	dwRet = waveOutPrepareHeader(wmw->hWave, &wmw->WaveHdr, sizeof(WAVEHDR));
	wmw->WaveHdr.dwBytesRecorded = 0;
	TRACE("before WODM_WRITE lpWaveHdr=%p dwBufferLength=%lu dwBytesRecorded=%lu\n",
	      &wmw->WaveHdr, wmw->WaveHdr.dwBufferLength, wmw->WaveHdr.dwBytesRecorded);
	dwRet = waveOutWrite(wmw->hWave, &wmw->WaveHdr, sizeof(WAVEHDR));
	/* FIXME: should use callback mechanisms from audio driver */
#if 1
	while (!(wmw->WaveHdr.dwFlags & WHDR_DONE))
	    Sleep(1);
#endif
	wmw->dwPosition += count;
	TRACE("after WODM_WRITE dwPosition=%lu\n", wmw->dwPosition);
	dwRet = waveOutUnprepareHeader(wmw->hWave, &wmw->WaveHdr, sizeof(WAVEHDR));
    }
    
    if (wmw->WaveHdr.lpData != NULL) {
	GlobalUnlock16(hData);
	GlobalFree16(hData);
	wmw->WaveHdr.lpData = NULL;
    }

    waveOutReset(wmw->hWave);
    waveOutClose(wmw->hWave);

    wmw->dwStatus = MCI_MODE_STOP;
    if (lpParms && (dwFlags & MCI_NOTIFY)) {
	TRACE("MCI_NOTIFY_SUCCESSFUL %08lX !\n", lpParms->dwCallback);
	mciDriverNotify((HWND)LOWORD(lpParms->dwCallback), 
			wmw->wNotifyDeviceID, MCI_NOTIFY_SUCCESSFUL);
    }
    return 0;
}

/**************************************************************************
 * 				WAVE_mciRecord			[internal]
 */
static DWORD WAVE_mciRecord(UINT wDevID, DWORD dwFlags, LPMCI_RECORD_PARMS lpParms)
{
    int		       	start, end;
    LONG		bufsize;
    HGLOBAL16		hData;
    LPWAVEHDR		lpWaveHdr;
    DWORD		dwRet;
    WINE_MCIWAVE*	wmw = WAVE_mciGetOpenDev(wDevID);
    
    TRACE("(%u, %08lX, %p);\n", wDevID, dwFlags, lpParms);
    
    if (wmw == NULL)		return MCIERR_INVALID_DEVICE_ID;
    if (lpParms == NULL)	return MCIERR_NULL_PARAMETER_BLOCK;
    
    if (!wmw->fInput) {
	WARN("cannot record on output device\n");
	return MCIERR_NONAPPLICABLE_FUNCTION;
    }
    
    if (wmw->hFile == 0) {
	WARN("can't find file='%s' !\n", 
	     wmw->openParms.lpstrElementName);
	return MCIERR_FILE_NOT_FOUND;
    }
    start = 1; 	end = 99999;
    if (dwFlags & MCI_FROM) {
	start = lpParms->dwFrom; 
	TRACE("MCI_FROM=%d \n", start);
    }
    if (dwFlags & MCI_TO) {
	end = lpParms->dwTo;
	TRACE("MCI_TO=%d \n", end);
    }
    bufsize = 64000;
    lpWaveHdr = &wmw->WaveHdr;
    hData = GlobalAlloc16(GMEM_MOVEABLE, bufsize);
    lpWaveHdr->lpData = (LPSTR)GlobalLock16(hData);
    lpWaveHdr->dwBufferLength = bufsize;
    lpWaveHdr->dwUser = 0L;
    lpWaveHdr->dwFlags = 0L;
    lpWaveHdr->dwLoops = 0L;
    dwRet = waveInPrepareHeader(wmw->hWave, lpWaveHdr, sizeof(WAVEHDR));

    for (;;) { /* FIXME: I don't see any waveInAddBuffer ? */
	lpWaveHdr->dwBytesRecorded = 0;
	dwRet = waveInStart(wmw->hWave);
	TRACE("waveInStart => lpWaveHdr=%p dwBytesRecorded=%lu\n",
	      lpWaveHdr, lpWaveHdr->dwBytesRecorded);
	if (lpWaveHdr->dwBytesRecorded == 0) break;
    }
    dwRet = waveInUnprepareHeader(wmw->hWave, lpWaveHdr, sizeof(WAVEHDR));
    if (lpWaveHdr->lpData != NULL) {
	GlobalUnlock16(hData);
	GlobalFree16(hData);
	lpWaveHdr->lpData = NULL;
    }
    if (dwFlags & MCI_NOTIFY) {
	TRACE("MCI_NOTIFY_SUCCESSFUL %08lX !\n", lpParms->dwCallback);
	mciDriverNotify((HWND)LOWORD(lpParms->dwCallback), 
			wmw->wNotifyDeviceID, MCI_NOTIFY_SUCCESSFUL);
    }
    return 0;
}

/**************************************************************************
 * 				WAVE_mciPause			[internal]
 */
static DWORD WAVE_mciPause(UINT wDevID, DWORD dwFlags, LPMCI_GENERIC_PARMS lpParms)
{
    DWORD 		dwRet;
    WINE_MCIWAVE*	wmw = WAVE_mciGetOpenDev(wDevID);
    
    TRACE("(%u, %08lX, %p);\n", wDevID, dwFlags, lpParms);
    
    if (lpParms == NULL)	return MCIERR_NULL_PARAMETER_BLOCK;
    if (wmw == NULL)		return MCIERR_INVALID_DEVICE_ID;
    
    if (wmw->dwStatus == MCI_MODE_PLAY) {
	wmw->dwStatus = MCI_MODE_PAUSE;
    } 
    
    if (wmw->fInput)	dwRet = waveInStop(wmw->hWave);
    else		dwRet = waveOutPause(wmw->hWave);
    
    return (dwRet == MMSYSERR_NOERROR) ? 0 : MCIERR_INTERNAL;
}

/**************************************************************************
 * 				WAVE_mciResume			[internal]
 */
static DWORD WAVE_mciResume(UINT wDevID, DWORD dwFlags, LPMCI_GENERIC_PARMS lpParms)
{
    WINE_MCIWAVE*	wmw = WAVE_mciGetOpenDev(wDevID);
    DWORD		dwRet = 0;
    
    TRACE("(%u, %08lX, %p);\n", wDevID, dwFlags, lpParms);
    
    if (lpParms == NULL)	return MCIERR_NULL_PARAMETER_BLOCK;
    if (wmw == NULL)		return MCIERR_INVALID_DEVICE_ID;
    
    if (wmw->dwStatus == MCI_MODE_PAUSE) {
	wmw->dwStatus = MCI_MODE_PLAY;
    } 
    
    /* FIXME: I doubt WIDM_START is correct */
    if (wmw->fInput)	dwRet = waveInStart(wmw->hWave);
    else		dwRet = waveOutRestart(wmw->hWave);
    return (dwRet == MMSYSERR_NOERROR) ? 0 : MCIERR_INTERNAL;
}

/**************************************************************************
 * 				WAVE_mciSeek			[internal]
 */
static DWORD WAVE_mciSeek(UINT wDevID, DWORD dwFlags, LPMCI_SEEK_PARMS lpParms)
{
    DWORD		ret = 0;
    WINE_MCIWAVE*	wmw = WAVE_mciGetOpenDev(wDevID);
    
    TRACE("(%04X, %08lX, %p);\n", wDevID, dwFlags, lpParms);
    
    if (lpParms == NULL) {
	ret = MCIERR_NULL_PARAMETER_BLOCK;
    } else if (wmw == NULL) {
	ret = MCIERR_INVALID_DEVICE_ID;
    } else {
	WAVE_mciStop(wDevID, MCI_WAIT, 0);
	
	if (dwFlags & MCI_SEEK_TO_START) {
	    wmw->dwPosition = 0;
	} else if (dwFlags & MCI_SEEK_TO_END) {
	    wmw->dwPosition = 0xFFFFFFFF; /* fixme */
	} else if (dwFlags & MCI_TO) {
	    wmw->dwPosition = WAVE_ConvertTimeFormatToByte(wmw, lpParms->dwTo);
	} else {
	    WARN("dwFlag doesn't tell where to seek to...\n");
	    return MCIERR_MISSING_PARAMETER;
	}
	
	TRACE("Seeking to position=%lu bytes\n", wmw->dwPosition);
	
	if (dwFlags & MCI_NOTIFY) {
	    TRACE("MCI_NOTIFY_SUCCESSFUL %08lX !\n", lpParms->dwCallback);
	    mciDriverNotify((HWND)LOWORD(lpParms->dwCallback), 
			    wmw->wNotifyDeviceID, MCI_NOTIFY_SUCCESSFUL);
	}
    }
    return ret;	
}    

/**************************************************************************
 * 				WAVE_mciSet			[internal]
 */
static DWORD WAVE_mciSet(UINT wDevID, DWORD dwFlags, LPMCI_SET_PARMS lpParms)
{
    WINE_MCIWAVE*	wmw = WAVE_mciGetOpenDev(wDevID);
    
    TRACE("(%u, %08lX, %p);\n", wDevID, dwFlags, lpParms);
    
    if (lpParms == NULL)	return MCIERR_NULL_PARAMETER_BLOCK;
    if (wmw == NULL)		return MCIERR_INVALID_DEVICE_ID;
    
    if (dwFlags & MCI_SET_TIME_FORMAT) {
	switch (lpParms->dwTimeFormat) {
	case MCI_FORMAT_MILLISECONDS:
	    TRACE("MCI_FORMAT_MILLISECONDS !\n");
	    wmw->dwMciTimeFormat = MCI_FORMAT_MILLISECONDS;
	    break;
	case MCI_FORMAT_BYTES:
	    TRACE("MCI_FORMAT_BYTES !\n");
	    wmw->dwMciTimeFormat = MCI_FORMAT_BYTES;
	    break;
	case MCI_FORMAT_SAMPLES:
	    TRACE("MCI_FORMAT_SAMPLES !\n");
	    wmw->dwMciTimeFormat = MCI_FORMAT_SAMPLES;
	    break;
	default:
	    WARN("Bad time format %lu!\n", lpParms->dwTimeFormat);
	    return MCIERR_BAD_TIME_FORMAT;
	}
    }
    if (dwFlags & MCI_SET_VIDEO) {
	TRACE("No support for video !\n");
	return MCIERR_UNSUPPORTED_FUNCTION;
    }
    if (dwFlags & MCI_SET_DOOR_OPEN) {
	TRACE("No support for door open !\n");
	return MCIERR_UNSUPPORTED_FUNCTION;
    }
    if (dwFlags & MCI_SET_DOOR_CLOSED) {
	TRACE("No support for door close !\n");
	return MCIERR_UNSUPPORTED_FUNCTION;
    }
    if (dwFlags & MCI_SET_AUDIO) {
	if (dwFlags & MCI_SET_ON) {
	    TRACE("MCI_SET_ON audio !\n");
	} else if (dwFlags & MCI_SET_OFF) {
	    TRACE("MCI_SET_OFF audio !\n");
	} else {
	    WARN("MCI_SET_AUDIO without SET_ON or SET_OFF\n");
	    return MCIERR_BAD_INTEGER;
	}
	
	if (lpParms->dwAudio & MCI_SET_AUDIO_ALL)
	    TRACE("MCI_SET_AUDIO_ALL !\n");
	if (lpParms->dwAudio & MCI_SET_AUDIO_LEFT)
	    TRACE("MCI_SET_AUDIO_LEFT !\n");
	if (lpParms->dwAudio & MCI_SET_AUDIO_RIGHT)
	    TRACE("MCI_SET_AUDIO_RIGHT !\n");
    }
    if (dwFlags & MCI_WAVE_INPUT) 
	TRACE("MCI_WAVE_INPUT !\n");
    if (dwFlags & MCI_WAVE_OUTPUT) 
	TRACE("MCI_WAVE_OUTPUT !\n");
    if (dwFlags & MCI_WAVE_SET_ANYINPUT) 
	TRACE("MCI_WAVE_SET_ANYINPUT !\n");
    if (dwFlags & MCI_WAVE_SET_ANYOUTPUT) 
	TRACE("MCI_WAVE_SET_ANYOUTPUT !\n");
    if (dwFlags & MCI_WAVE_SET_AVGBYTESPERSEC) 
	TRACE("MCI_WAVE_SET_AVGBYTESPERSEC !\n");
    if (dwFlags & MCI_WAVE_SET_BITSPERSAMPLE) 
	TRACE("MCI_WAVE_SET_BITSPERSAMPLE !\n");
    if (dwFlags & MCI_WAVE_SET_BLOCKALIGN) 
	TRACE("MCI_WAVE_SET_BLOCKALIGN !\n");
    if (dwFlags & MCI_WAVE_SET_CHANNELS) 
	TRACE("MCI_WAVE_SET_CHANNELS !\n");
    if (dwFlags & MCI_WAVE_SET_FORMATTAG) 
	TRACE("MCI_WAVE_SET_FORMATTAG !\n");
    if (dwFlags & MCI_WAVE_SET_SAMPLESPERSEC) 
	TRACE("MCI_WAVE_SET_SAMPLESPERSEC !\n");
    return 0;
}

/**************************************************************************
 * 				WAVE_mciStatus		[internal]
 */
static DWORD WAVE_mciStatus(UINT wDevID, DWORD dwFlags, LPMCI_STATUS_PARMS lpParms)
{
    WINE_MCIWAVE*	wmw = WAVE_mciGetOpenDev(wDevID);
    DWORD		ret;

    TRACE("(%u, %08lX, %p);\n", wDevID, dwFlags, lpParms);
    if (lpParms == NULL)	return MCIERR_NULL_PARAMETER_BLOCK;
    if (wmw == NULL)		return MCIERR_INVALID_DEVICE_ID;
    
    if (dwFlags & MCI_STATUS_ITEM) {
	switch (lpParms->dwItem) {
	case MCI_STATUS_CURRENT_TRACK:
	    lpParms->dwReturn = 1;
	    TRACE("MCI_STATUS_CURRENT_TRACK => %lu\n", lpParms->dwReturn);
	    break;
	case MCI_STATUS_LENGTH:
	    /* only one track in file is currently handled, so don't take care of MCI_TRACK flag */
	    lpParms->dwReturn = WAVE_ConvertByteToTimeFormat(wmw, wmw->dwLength, &ret);
	    TRACE("MCI_STATUS_LENGTH => %lu\n", lpParms->dwReturn);
	    break;
	case MCI_STATUS_MODE:
	    TRACE("MCI_STATUS_MODE => %u\n", wmw->dwStatus);
	    lpParms->dwReturn = MAKEMCIRESOURCE(wmw->dwStatus, wmw->dwStatus);
	    ret = MCI_RESOURCE_RETURNED;
	    break;
	case MCI_STATUS_MEDIA_PRESENT:
	    TRACE("MCI_STATUS_MEDIA_PRESENT => TRUE!\n");
	    lpParms->dwReturn = MAKEMCIRESOURCE(TRUE, MCI_TRUE);
	    ret = MCI_RESOURCE_RETURNED;
	    break;
	case MCI_STATUS_NUMBER_OF_TRACKS:
	    /* only one track in file is currently handled, so don't take care of MCI_TRACK flag */
	    lpParms->dwReturn = 1;
	    TRACE("MCI_STATUS_NUMBER_OF_TRACKS => %lu!\n", lpParms->dwReturn);
	    break;
	case MCI_STATUS_POSITION:
	    /* only one track in file is currently handled, so don't take care of MCI_TRACK flag */
	    lpParms->dwReturn = WAVE_ConvertByteToTimeFormat(wmw, 
							     (dwFlags & MCI_STATUS_START) ? 0 : wmw->dwPosition,
							     &ret);
	    TRACE("MCI_STATUS_POSITION %s => %lu\n", 
		  (dwFlags & MCI_STATUS_START) ? "start" : "current", lpParms->dwReturn);
	    break;
	case MCI_STATUS_READY:
	    lpParms->dwReturn = (wmw->dwStatus == MCI_MODE_NOT_READY) ?
		MAKEMCIRESOURCE(FALSE, MCI_FALSE) : MAKEMCIRESOURCE(TRUE, MCI_TRUE);
	    TRACE("MCI_STATUS_READY => %u!\n", LOWORD(lpParms->dwReturn));
	    ret = MCI_RESOURCE_RETURNED;
	    break;
	case MCI_STATUS_TIME_FORMAT:
	    lpParms->dwReturn = MAKEMCIRESOURCE(wmw->dwMciTimeFormat, wmw->dwMciTimeFormat);
	    TRACE("MCI_STATUS_TIME_FORMAT => %lu\n", lpParms->dwReturn);
	    ret = MCI_RESOURCE_RETURNED;
	    break;
	case MCI_WAVE_INPUT:
	    TRACE("MCI_WAVE_INPUT !\n");
	    lpParms->dwReturn = 0;
	    break;
	case MCI_WAVE_OUTPUT:
	    TRACE("MCI_WAVE_OUTPUT !\n");
	    lpParms->dwReturn = 0;
	    break;
	case MCI_WAVE_STATUS_AVGBYTESPERSEC:
	    lpParms->dwReturn = wmw->WaveFormat.nAvgBytesPerSec;
	    TRACE("MCI_WAVE_STATUS_AVGBYTESPERSEC => %lu!\n", lpParms->dwReturn);
	    break;
	case MCI_WAVE_STATUS_BITSPERSAMPLE:
	    lpParms->dwReturn = wmw->WaveFormat.wBitsPerSample;
	    TRACE("MCI_WAVE_STATUS_BITSPERSAMPLE => %lu!\n", lpParms->dwReturn);
	    break;
	case MCI_WAVE_STATUS_BLOCKALIGN:
	    lpParms->dwReturn = wmw->WaveFormat.nBlockAlign;
	    TRACE("MCI_WAVE_STATUS_BLOCKALIGN => %lu!\n", lpParms->dwReturn);
	    break;
	case MCI_WAVE_STATUS_CHANNELS:
	    lpParms->dwReturn = wmw->WaveFormat.nChannels;
	    TRACE("MCI_WAVE_STATUS_CHANNELS => %lu!\n", lpParms->dwReturn);
	    break;
	case MCI_WAVE_STATUS_FORMATTAG:
	    lpParms->dwReturn = wmw->WaveFormat.wFormatTag;
	    TRACE("MCI_WAVE_FORMATTAG => %lu!\n", lpParms->dwReturn);
	    break;
	case MCI_WAVE_STATUS_LEVEL:
	    TRACE("MCI_WAVE_STATUS_LEVEL !\n");
	    lpParms->dwReturn = 0xAAAA5555;
	    break;
	case MCI_WAVE_STATUS_SAMPLESPERSEC:
	    lpParms->dwReturn = wmw->WaveFormat.nSamplesPerSec;
	    TRACE("MCI_WAVE_STATUS_SAMPLESPERSEC => %lu!\n", lpParms->dwReturn);
	    break;
	default:
	    WARN("unknown command %08lX !\n", lpParms->dwItem);
	    return MCIERR_UNRECOGNIZED_COMMAND;
	}
    }
    if (dwFlags & MCI_NOTIFY) {
	TRACE("MCI_NOTIFY_SUCCESSFUL %08lX !\n", lpParms->dwCallback);
	mciDriverNotify((HWND)LOWORD(lpParms->dwCallback), 
			wmw->wNotifyDeviceID, MCI_NOTIFY_SUCCESSFUL);
    }
    return ret;
}

/**************************************************************************
 * 				WAVE_mciGetDevCaps		[internal]
 */
static DWORD WAVE_mciGetDevCaps(UINT wDevID, DWORD dwFlags, 
				LPMCI_GETDEVCAPS_PARMS lpParms)
{
    WINE_MCIWAVE*	wmw = WAVE_mciGetOpenDev(wDevID);
    DWORD		ret = 0;

    TRACE("(%u, %08lX, %p);\n", wDevID, dwFlags, lpParms);
    
    if (lpParms == NULL)	return MCIERR_NULL_PARAMETER_BLOCK;
    if (wmw == NULL)		return MCIERR_INVALID_DEVICE_ID;
    
    if (dwFlags & MCI_GETDEVCAPS_ITEM) {
	switch(lpParms->dwItem) {
	case MCI_GETDEVCAPS_DEVICE_TYPE:
	    lpParms->dwReturn = MAKEMCIRESOURCE(MCI_DEVTYPE_WAVEFORM_AUDIO, MCI_DEVTYPE_WAVEFORM_AUDIO);
	    ret = MCI_RESOURCE_RETURNED;
	    break;
	case MCI_GETDEVCAPS_HAS_AUDIO:
	    lpParms->dwReturn = MAKEMCIRESOURCE(TRUE, MCI_TRUE);
	    ret = MCI_RESOURCE_RETURNED;
	    break;
	case MCI_GETDEVCAPS_HAS_VIDEO:
	    lpParms->dwReturn = MAKEMCIRESOURCE(FALSE, MCI_FALSE);
	    ret = MCI_RESOURCE_RETURNED;
	    break;
	case MCI_GETDEVCAPS_USES_FILES:
	    lpParms->dwReturn = MAKEMCIRESOURCE(TRUE, MCI_TRUE);
	    ret = MCI_RESOURCE_RETURNED;
	    break;
	case MCI_GETDEVCAPS_COMPOUND_DEVICE:
	    lpParms->dwReturn = MAKEMCIRESOURCE(TRUE, MCI_TRUE);
	    ret = MCI_RESOURCE_RETURNED;
	    break;
	case MCI_GETDEVCAPS_CAN_RECORD:
	    lpParms->dwReturn = MAKEMCIRESOURCE(TRUE, MCI_TRUE);
	    ret = MCI_RESOURCE_RETURNED;
	    break;
	case MCI_GETDEVCAPS_CAN_EJECT:
	    lpParms->dwReturn = MAKEMCIRESOURCE(FALSE, MCI_FALSE);
	    ret = MCI_RESOURCE_RETURNED;
	    break;
	case MCI_GETDEVCAPS_CAN_PLAY:
	    lpParms->dwReturn = MAKEMCIRESOURCE(TRUE, MCI_TRUE);
	    ret = MCI_RESOURCE_RETURNED;
	    break;
	case MCI_GETDEVCAPS_CAN_SAVE:
	    lpParms->dwReturn = MAKEMCIRESOURCE(TRUE, MCI_TRUE);
	    ret = MCI_RESOURCE_RETURNED;
	    break;
	case MCI_WAVE_GETDEVCAPS_INPUTS:
	    lpParms->dwReturn = 1;
	    break;
	case MCI_WAVE_GETDEVCAPS_OUTPUTS:
	    lpParms->dwReturn = 1;
	    break;
	default:
	    FIXME("Unknown capability (%08lx) !\n", lpParms->dwItem);
	    return MCIERR_UNRECOGNIZED_COMMAND;
	}
    } else {
	WARN("No GetDevCaps-Item !\n");
	return MCIERR_UNRECOGNIZED_COMMAND;
    }
    return ret;
}

/**************************************************************************
 * 				WAVE_mciInfo			[internal]
 */
static DWORD WAVE_mciInfo(UINT wDevID, DWORD dwFlags, LPMCI_INFO_PARMSA lpParms)
{
    DWORD		ret = 0;
    LPCSTR		str = 0;
    WINE_MCIWAVE*	wmw = WAVE_mciGetOpenDev(wDevID);
    
    TRACE("(%u, %08lX, %p);\n", wDevID, dwFlags, lpParms);
    
    if (lpParms == NULL || lpParms->lpstrReturn == NULL) {
	ret = MCIERR_NULL_PARAMETER_BLOCK;
    } else if (wmw == NULL) {
	ret = MCIERR_INVALID_DEVICE_ID;
    } else {
	TRACE("buf=%p, len=%lu\n", lpParms->lpstrReturn, lpParms->dwRetSize);
	
	switch(dwFlags) {
	case MCI_INFO_PRODUCT:
	    str = "Wine's audio player";
	    break;
	case MCI_INFO_FILE:
	    str = wmw->openParms.lpstrElementName;
	    break;
	case MCI_WAVE_INPUT:
	    str = "Wine Wave In";
	    break;
	case MCI_WAVE_OUTPUT:
	    str = "Wine Wave Out";
	    break;
	default:
	    WARN("Don't know this info command (%lu)\n", dwFlags);
	    ret = MCIERR_UNRECOGNIZED_COMMAND;
	}
    }
    if (str) {
	if (strlen(str) + 1 > lpParms->dwRetSize) {
	    ret = MCIERR_PARAM_OVERFLOW;
	} else {
	    lstrcpynA(lpParms->lpstrReturn, str, lpParms->dwRetSize);
	}
    } else {
	lpParms->lpstrReturn[0] = 0;
    }
    
    return ret;
}

/**************************************************************************
 * 				MCIWAVE_DriverProc		[sample driver]
 */
LONG CALLBACK	MCIWAVE_DriverProc(DWORD dwDevID, HDRVR hDriv, DWORD wMsg, 
				   DWORD dwParam1, DWORD dwParam2)
{
    TRACE("(%08lX, %04X, %08lX, %08lX, %08lX)\n", 
	  dwDevID, hDriv, wMsg, dwParam1, dwParam2);
    
    switch(wMsg) {
    case DRV_LOAD:		return 1;
    case DRV_FREE:		return 1;
    case DRV_OPEN:		return WAVE_drvOpen((LPSTR)dwParam1, (LPMCI_OPEN_DRIVER_PARMSA)dwParam2);
    case DRV_CLOSE:		return WAVE_drvClose(dwDevID);
    case DRV_ENABLE:		return 1;
    case DRV_DISABLE:		return 1;
    case DRV_QUERYCONFIGURE:	return 1;
    case DRV_CONFIGURE:		MessageBoxA(0, "Sample MultiMedia Driver !", "OSS Driver", MB_OK);	return 1;
    case DRV_INSTALL:		return DRVCNF_RESTART;
    case DRV_REMOVE:		return DRVCNF_RESTART;
    case MCI_OPEN_DRIVER:	return WAVE_mciOpen      (dwDevID, dwParam1, (LPMCI_WAVE_OPEN_PARMSA)  dwParam2);
    case MCI_CLOSE_DRIVER:	return WAVE_mciClose     (dwDevID, dwParam1, (LPMCI_GENERIC_PARMS)     dwParam2);
    case MCI_CUE:		return WAVE_mciCue       (dwDevID, dwParam1, (LPMCI_GENERIC_PARMS)     dwParam2);
    case MCI_PLAY:		return WAVE_mciPlay      (dwDevID, dwParam1, (LPMCI_PLAY_PARMS)        dwParam2);
    case MCI_RECORD:		return WAVE_mciRecord    (dwDevID, dwParam1, (LPMCI_RECORD_PARMS)      dwParam2);
    case MCI_STOP:		return WAVE_mciStop      (dwDevID, dwParam1, (LPMCI_GENERIC_PARMS)     dwParam2);
    case MCI_SET:		return WAVE_mciSet       (dwDevID, dwParam1, (LPMCI_SET_PARMS)         dwParam2);
    case MCI_PAUSE:		return WAVE_mciPause     (dwDevID, dwParam1, (LPMCI_GENERIC_PARMS)     dwParam2);
    case MCI_RESUME:		return WAVE_mciResume    (dwDevID, dwParam1, (LPMCI_GENERIC_PARMS)     dwParam2);
    case MCI_STATUS:		return WAVE_mciStatus    (dwDevID, dwParam1, (LPMCI_STATUS_PARMS)      dwParam2);
    case MCI_GETDEVCAPS:	return WAVE_mciGetDevCaps(dwDevID, dwParam1, (LPMCI_GETDEVCAPS_PARMS)  dwParam2);
    case MCI_INFO:		return WAVE_mciInfo      (dwDevID, dwParam1, (LPMCI_INFO_PARMSA)       dwParam2);
    case MCI_SEEK:		return WAVE_mciSeek      (dwDevID, dwParam1, (LPMCI_SEEK_PARMS)        dwParam2);		
	/* commands that should be supported */
    case MCI_LOAD:		
    case MCI_SAVE:		
    case MCI_FREEZE:		
    case MCI_PUT:		
    case MCI_REALIZE:		
    case MCI_UNFREEZE:		
    case MCI_UPDATE:		
    case MCI_WHERE:		
    case MCI_STEP:		
    case MCI_SPIN:		
    case MCI_ESCAPE:		
    case MCI_COPY:		
    case MCI_CUT:		
    case MCI_DELETE:		
    case MCI_PASTE:		
	FIXME("Unsupported yet command [%lu]\n", wMsg);
	break;
    case MCI_WINDOW:		
	TRACE("Unsupported command [%lu]\n", wMsg);
	break;
    case MCI_OPEN:
    case MCI_CLOSE:
	ERR("Shouldn't receive a MCI_OPEN or CLOSE message\n");
	break;
    default:
	FIXME("is probably wrong msg [%lu]\n", wMsg);
	return DefDriverProc(dwDevID, hDriv, wMsg, dwParam1, dwParam2);
    }
    return MCIERR_UNRECOGNIZED_COMMAND;
}

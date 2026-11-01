/*
FxSound
Copyright (C) 2025  FxSound LLC

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU Affero General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Affero General Public License for more details.

You should have received a copy of the GNU Affero General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

/* Standard includes */
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>
#include <stdio.h>
#include <math.h>

#include <mmreg.h>
#include <Mmdeviceapi.h>
#include <Audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <endpointvolume.h>
#include <Propvarutil.h>

#include "codedefs.h"
#include "slout.h"
#include "mry.h"
#include "u_sndDevices.h"
#include "sndDevices.h"

/* Float sample, nominal range -1.0 to 1.0, to a clipped 32 bit signed integer sample. */
static INT32 sndDevicesFloatToInt32(float f)
{
	double d = (double)f * 2147483648.0;

	if( d != d )
		return( 0 );	// NaN
	if( d >= 2147483647.0 )
		return( 2147483647 );
	if( d <= -2147483648.0 )
		return( (INT32)(-2147483647 - 1) );
	return( (INT32)d );
}

/*
 * Converts float samples to the sample format of an exclusive-mode output stream and writes them to pDest.
 * numSamples is the number of samples, ie frames * channels.
 */
static void sndDevicesWriteExclusiveSamples(BYTE *pDest, const float *fpSrc, UINT32 numSamples, int sampleType)
{
	UINT32 i;
	INT32 v;
	double d;

	switch( sampleType )
	{
		case SND_DEVICES_EXCL_FMT_INT32:
		{
			INT32 *p32 = (INT32 *)pDest;
			for(i=0; i<numSamples; i++)
				p32[i] = sndDevicesFloatToInt32(fpSrc[i]);
			break;
		}
		case SND_DEVICES_EXCL_FMT_INT24_IN_32:
		{
			INT32 *p32 = (INT32 *)pDest;
			for(i=0; i<numSamples; i++)
				p32[i] = (INT32)((UINT32)sndDevicesFloatToInt32(fpSrc[i]) & 0xFFFFFF00u);
			break;
		}
		case SND_DEVICES_EXCL_FMT_INT24_PACKED:
		{
			for(i=0; i<numSamples; i++)
			{
				v = sndDevicesFloatToInt32(fpSrc[i]) >> 8;
				pDest[i*3]     = (BYTE)(v & 0xFF);
				pDest[i*3 + 1] = (BYTE)((v >> 8) & 0xFF);
				pDest[i*3 + 2] = (BYTE)((v >> 16) & 0xFF);
			}
			break;
		}
		case SND_DEVICES_EXCL_FMT_INT16:
		{
			INT16 *p16 = (INT16 *)pDest;
			for(i=0; i<numSamples; i++)
			{
				d = floor((double)fpSrc[i] * 32768.0 + 0.5);
				if( d != d )
					d = 0.0;
				if( d > 32767.0 )
					d = 32767.0;
				if( d < -32768.0 )
					d = -32768.0;
				p16[i] = (INT16)d;
			}
			break;
		}
		default:	// SND_DEVICES_EXCL_FMT_FLOAT32
		{
			float *pf = (float *)pDest;
			for(i=0; i<numSamples; i++)
				pf[i] = fpSrc[i];
			break;
		}
	}
}

/*
 * FUNCTION: sndDevicesDoPlayback()
 * DESCRIPTION: Plays back audio buffers to specified playback device.
 * Plays buffer frames that have been queued up by the capture device.
 */
int PT_DECLSPEC sndDevicesDoPlayback(PT_HANDLE *hp_sndDevices, int *ip_resultFlag)
{
	struct sndDevicesHdlType *cast_handle;
	HRESULT hr;
	UINT32 numFramesQueuedUpToPlay;
	UINT32 i, j, index, loopsize;
	DWORD flags = 0;
	int numPlaybackChannels;
	int k;

	float *fptr;

	cast_handle = (struct sndDevicesHdlType *)hp_sndDevices;

	if (cast_handle == NULL)
		return(NOT_OKAY);

	if( cast_handle->pAudioClientPlayback == NULL )
	{
		*ip_resultFlag = SND_DEVICES_NULL_PLAYBACK_CLIENT;
		SND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_NULL_PLAYBACK_CLIENT)
	}

	numPlaybackChannels = cast_handle->wfxPlayback.nChannels;

	// Do upsampling if needed.
	if( cast_handle->upsampleRatio > 1 )
	{
		// Copy channel corrected playback buffers back to capture buffer so we can then re-sample.
		loopsize = cast_handle->capturedFramesCount * numPlaybackChannels;
		for(i=0; i<loopsize; i++)
			cast_handle->fCaptureBuf[i] = cast_handle->fPlaybackBuf[i];

		// Fill playback buff with upsampling
		index = 0;
		for(i=0; i<cast_handle->capturedFramesCount; i++) // Index through frames (sample sets)
		{
			for(j=0; j<cast_handle->upsampleRatio; j++)	 // Repeat frame fills for upsampling
			{
				for(k=0; k<numPlaybackChannels; k++)
				{
					cast_handle->fPlaybackBuf[index] = cast_handle->fCaptureBuf[i * numPlaybackChannels + k];
					index++;
				}
			}
		}
	}

	// This call returns the number of frames still awaiting playback in the playback buffer
	hr = cast_handle->pAudioClientPlayback->GetCurrentPadding(&numFramesQueuedUpToPlay);
	if (FAILED(hr)) SND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_GET_PADDING_FAILED)

	cast_handle->numPlaybackFramesAvailableToFill = cast_handle->bufferFrameSizePlayback - numFramesQueuedUpToPlay;

	// Data size should never come in to this function larger than available space, truncate it if need be.
	if( cast_handle->playbackFrameCount > cast_handle->numPlaybackFramesAvailableToFill )
		cast_handle->playbackFrameCount = cast_handle->numPlaybackFramesAvailableToFill;			

	// If we have playback buffers to write, acquire the requested frame space in the internal playback buffer.
	if( cast_handle->playbackFrameCount > 0 )
	{
		hr = cast_handle->pAudioClientPlaybackRender->GetBuffer(cast_handle->playbackFrameCount, &(cast_handle->pDataPacketPlayback));
		if (FAILED(hr)) SND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_PLAYBACK_RENDER_FAILED)

		// Don't do copy if either buffer is NULL
		if( (cast_handle->pDataPacketPlayback != NULL) && (cast_handle->fPlaybackBuf != NULL) )
		{
			loopsize = cast_handle->playbackFrameCount * numPlaybackChannels;

			if( cast_handle->playbackIsExclusive )
			{
				// Exclusive-mode stream, the device wants its own sample format.
				sndDevicesWriteExclusiveSamples(cast_handle->pDataPacketPlayback, cast_handle->fPlaybackBuf, loopsize, cast_handle->exclusiveSampleType);
			}
			else
			{
				fptr = (float *)(cast_handle->pDataPacketPlayback);

				for(i=0; i<loopsize; i++)
					fptr[i] = cast_handle->fPlaybackBuf[i];
			}
		}

		hr = cast_handle->pAudioClientPlaybackRender->ReleaseBuffer(cast_handle->playbackFrameCount, flags);
		if (FAILED(hr)) SND_DEVICES_SET_STATUS_AND_RETURN_OK(SND_DEVICES_RELEASE_BUFFER_FAILED)
	}

	*ip_resultFlag = SND_DEVICES_CAPTURE_PLAYBACK_SUCCESS;

	return(OKAY);
}

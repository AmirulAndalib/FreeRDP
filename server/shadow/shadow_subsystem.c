/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 *
 * Copyright 2014 Marc-Andre Moreau <marcandre.moreau@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <freerdp/config.h>

#include "shadow.h"

#include "shadow_subsystem.h"

static pfnShadowSubsystemEntry pSubsystemEntry = NULL;

void shadow_subsystem_set_entry(pfnShadowSubsystemEntry pEntry)
{
	pSubsystemEntry = pEntry;
}

static int shadow_subsystem_load_entry_points(RDP_SHADOW_ENTRY_POINTS* pEntryPoints)
{
	WINPR_ASSERT(pEntryPoints);
	ZeroMemory(pEntryPoints, sizeof(RDP_SHADOW_ENTRY_POINTS));

	if (!pSubsystemEntry)
		return -1;

	if (pSubsystemEntry(pEntryPoints) < 0)
		return -1;

	return 1;
}

rdpShadowSubsystem* shadow_subsystem_new(void)
{
	RDP_SHADOW_ENTRY_POINTS ep;
	rdpShadowSubsystem* subsystem = NULL;

	shadow_subsystem_load_entry_points(&ep);

	if (!ep.New)
		return NULL;

	subsystem = ep.New();

	if (!subsystem)
		return NULL;

	CopyMemory(&(subsystem->ep), &ep, sizeof(RDP_SHADOW_ENTRY_POINTS));

	return subsystem;
}

void shadow_subsystem_free(rdpShadowSubsystem* subsystem)
{
	if (subsystem && subsystem->ep.Free)
		subsystem->ep.Free(subsystem);
}

int shadow_subsystem_init(rdpShadowSubsystem* subsystem, rdpShadowServer* server)
{
	int status = -1;

	if (!subsystem || !subsystem->ep.Init)
		return -1;

	subsystem->server = server;
	subsystem->selectedMonitor = server->selectedMonitor;

	if (!(subsystem->MsgPipe = MessagePipe_New()))
		goto fail;

	if (!(subsystem->updateEvent = shadow_multiclient_new()))
		goto fail;

	if ((status = subsystem->ep.Init(subsystem)) >= 0)
		return status;

fail:
	if (subsystem->MsgPipe)
	{
		MessagePipe_Free(subsystem->MsgPipe);
		subsystem->MsgPipe = NULL;
	}

	if (subsystem->updateEvent)
	{
		shadow_multiclient_free(subsystem->updateEvent);
		subsystem->updateEvent = NULL;
	}

	return status;
}

static void shadow_subsystem_free_queued_message(void* obj)
{
	wMessage* message = (wMessage*)obj;
	if (message->Free)
	{
		message->Free(message);
		message->Free = NULL;
	}
}

void shadow_subsystem_uninit(rdpShadowSubsystem* subsystem)
{
	if (!subsystem)
		return;

	if (subsystem->ep.Uninit)
		subsystem->ep.Uninit(subsystem);

	if (subsystem->MsgPipe)
	{
		wObject* obj1 = NULL;
		wObject* obj2 = NULL;
		/* Release resource in messages before free */
		obj1 = MessageQueue_Object(subsystem->MsgPipe->In);

		obj1->fnObjectFree = shadow_subsystem_free_queued_message;
		MessageQueue_Clear(subsystem->MsgPipe->In);

		obj2 = MessageQueue_Object(subsystem->MsgPipe->Out);
		obj2->fnObjectFree = shadow_subsystem_free_queued_message;
		MessageQueue_Clear(subsystem->MsgPipe->Out);
		MessagePipe_Free(subsystem->MsgPipe);
		subsystem->MsgPipe = NULL;
	}

	if (subsystem->updateEvent)
	{
		shadow_multiclient_free(subsystem->updateEvent);
		subsystem->updateEvent = NULL;
	}
}

int shadow_subsystem_start(rdpShadowSubsystem* subsystem)
{
	int status = 0;

	if (!subsystem || !subsystem->ep.Start)
		return -1;

	status = subsystem->ep.Start(subsystem);

	return status;
}

int shadow_subsystem_stop(rdpShadowSubsystem* subsystem)
{
	int status = 0;

	if (!subsystem || !subsystem->ep.Stop)
		return -1;

	status = subsystem->ep.Stop(subsystem);

	return status;
}

UINT32 shadow_enum_monitors(MONITOR_DEF* monitors, UINT32 maxMonitors)
{
	UINT32 numMonitors = 0;
	RDP_SHADOW_ENTRY_POINTS ep;

	if (shadow_subsystem_load_entry_points(&ep) < 0)
		return 0;

	numMonitors = ep.EnumMonitors(monitors, maxMonitors);

	return numMonitors;
}

/**
 * Common function for subsystem implementation.
 * This function convert 32bit ARGB format pixels to xormask data
 * and andmask data and fill into SHADOW_MSG_OUT_POINTER_ALPHA_UPDATE
 * Caller should free the andMaskData and xorMaskData later.
 */
#if !defined(WITHOUT_FREERDP_3x_DEPRECATED)
int shadow_subsystem_pointer_convert_alpha_pointer_data(
    const BYTE* WINPR_RESTRICT pixels, BOOL premultiplied, UINT32 width, UINT32 height,
    SHADOW_MSG_OUT_POINTER_ALPHA_UPDATE* WINPR_RESTRICT pointerColor)
{
	return shadow_subsystem_pointer_convert_alpha_pointer_data_to_format(
	    pixels, PIXEL_FORMAT_BGRX32, premultiplied, width, height, pointerColor);
}
#endif

int shadow_subsystem_pointer_convert_alpha_pointer_data_to_format(
    const BYTE* pixels, UINT32 format, BOOL premultiplied, UINT32 width, UINT32 height,
    SHADOW_MSG_OUT_POINTER_ALPHA_UPDATE* pointerColor)
{
	UINT32 xorStep = 0;
	UINT32 andStep = 0;
	UINT32 andBit = 0;
	BYTE* andBits = NULL;
	UINT32 andPixel = 0;
	const size_t bpp = FreeRDPGetBytesPerPixel(format);

	xorStep = (width * 3);
	xorStep += (xorStep % 2);

	andStep = ((width + 7) / 8);
	andStep += (andStep % 2);

	pointerColor->lengthXorMask = height * xorStep;
	pointerColor->xorMaskData = (BYTE*)calloc(1, pointerColor->lengthXorMask);

	if (!pointerColor->xorMaskData)
		return -1;

	pointerColor->lengthAndMask = height * andStep;
	pointerColor->andMaskData = (BYTE*)calloc(1, pointerColor->lengthAndMask);

	if (!pointerColor->andMaskData)
	{
		free(pointerColor->xorMaskData);
		pointerColor->xorMaskData = NULL;
		return -1;
	}

	for (size_t y = 0; y < height; y++)
	{
		const BYTE* pSrc8 = &pixels[(width * bpp) * (height - 1 - y)];
		BYTE* pDst8 = &(pointerColor->xorMaskData[y * xorStep]);

		andBit = 0x80;
		andBits = &(pointerColor->andMaskData[andStep * y]);

		for (size_t x = 0; x < width; x++)
		{
			BYTE B = 0;
			BYTE G = 0;
			BYTE R = 0;
			BYTE A = 0;

			const UINT32 color = FreeRDPReadColor(&pSrc8[x * bpp], format);
			FreeRDPSplitColor(color, format, &R, &G, &B, &A, NULL);

			andPixel = 0;

			if (A < 64)
				A = 0; /* pixel cannot be partially transparent */

			if (!A)
			{
				/* transparent pixel: XOR = black, AND = 1 */
				andPixel = 1;
				B = G = R = 0;
			}
			else
			{
				if (premultiplied)
				{
					B = (B * 0xFF) / A;
					G = (G * 0xFF) / A;
					R = (R * 0xFF) / A;
				}
			}

			*pDst8++ = B;
			*pDst8++ = G;
			*pDst8++ = R;

			if (andPixel)
				*andBits |= andBit;
			if (!(andBit >>= 1))
			{
				andBits++;
				andBit = 0x80;
			}
		}
	}

	return 1;
}

void shadow_subsystem_frame_update(rdpShadowSubsystem* subsystem)
{
	shadow_multiclient_publish_and_wait(subsystem->updateEvent);
}

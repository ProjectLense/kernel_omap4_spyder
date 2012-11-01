/**********************************************************************
 *
 * Copyright (C) Imagination Technologies Ltd. All rights reserved.
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 * 
 * This program is distributed in the hope it will be useful but, except 
 * as otherwise stated in writing, without any warranty; without even the 
 * implied warranty of merchantability or fitness for a particular purpose. 
 * See the GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
 * 
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 * Contact Information:
 * Imagination Technologies Ltd. <gpl-support@imgtec.com>
 * Home Park Estate, Kings Langley, Herts, WD4 8LZ, UK 
 *
 ******************************************************************************/

#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/console.h>
#include <linux/fb.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/notifier.h>
#include <linux/bltsville.h>
#include <linux/bvinternal.h>
#include <linux/bv_gc2d.h>

#include "img_defs.h"
#include "servicesext.h"
#include "kerneldisplay.h"
#include "omaplfb.h"

#if defined(CONFIG_ION_OMAP)
#include <linux/ion.h>
#include <linux/omap_ion.h>
extern struct ion_client *gpsIONClient;
#endif
#if defined(CONFIG_TI_TILER)
#include <mach/tiler.h>
#include <video/dsscomp.h>
#include <plat/dsscomp.h>
#endif
#include <video/omap_hwc.h>

#define OMAPLFB_COMMAND_COUNT		1

#define	OMAPLFB_VSYNC_SETTLE_COUNT	5

#if defined(CONFIG_OMAPLFB_CLONE_VRAM_TO_TILER)
#include <plat/dma.h>

#define DMA_TX_TIMEOUT   (1000)
static int  OMAPLFBMotDmaInit(void);
static void OMAPLFBMotDmaTerm(void);
static int  OMAPLFBMotDmaConfig(int w, int h, int bytespp,
					int src_stride, int dst_stride);
static int  OMAPLFBMotDmaPerform(unsigned long src, unsigned long dst);
static void OMAPLFBMotDmaCallback(int lch, u16 ch_status, void *pdata);
static OMAPLFB_BOOL OMAPLFBMotAllocTiler2d(void);
static OMAPLFB_BOOL OMAPLFBMotDeallocTiler2d(void);
static IMG_UINT32 fb_to_phys(const OMAPLFB_DEVINFO *psDevInfo,
					IMG_UINT32 addr);
static IMG_SYS_PHYADDR sTilerMemAddr[2];
struct omap_ion_tiler_alloc_data sTilerAllocData;
static IMG_SYS_PHYADDR *sPageList;
static unsigned int sDestStride;
struct OMAPLFBMotDmaData {
	u32               en;
	u32               fn;
	u32               src_fi;
	u32               dst_fi;

	int               id;
	int               ch;
	bool              complete;
	wait_queue_head_t wait;
};
static struct OMAPLFBMotDmaData dmadata;
#endif /* CONFIG_OMAPLFB_CLONE_VRAM_TO_TILER */

static OMAPLFB_DEVINFO *gapsDevInfo[OMAPLFB_MAX_NUM_DEVICES];

static PFN_DC_GET_PVRJTABLE gpfnGetPVRJTable = NULL;

static int bv_gc2d_is_present;
static struct bventry bv_gc2d_entry = {NULL, NULL, NULL};

static inline unsigned long RoundUpToMultiple(unsigned long x, unsigned long y)
{
	unsigned long div = x / y;
	unsigned long rem = x % y;

	return (div + ((rem == 0) ? 0 : 1)) * y;
}

static unsigned long GCD(unsigned long x, unsigned long y)
{
	while (y != 0)
	{
		unsigned long r = x % y;
		x = y;
		y = r;
	}

	return x;
}

static unsigned long LCM(unsigned long x, unsigned long y)
{
	unsigned long gcd = GCD(x, y);

	return (gcd == 0) ? 0 : ((x / gcd) * y);
}

unsigned OMAPLFBMaxFBDevIDPlusOne(void)
{
	return OMAPLFB_MAX_NUM_DEVICES;
}

OMAPLFB_DEVINFO *OMAPLFBGetDevInfoPtr(unsigned uiFBDevID)
{
	WARN_ON(uiFBDevID >= OMAPLFBMaxFBDevIDPlusOne());

	if (uiFBDevID >= OMAPLFB_MAX_NUM_DEVICES)
	{
		return NULL;
	}

	return gapsDevInfo[uiFBDevID];
}

static inline void OMAPLFBSetDevInfoPtr(unsigned uiFBDevID, OMAPLFB_DEVINFO *psDevInfo)
{
	WARN_ON(uiFBDevID >= OMAPLFB_MAX_NUM_DEVICES);

	if (uiFBDevID < OMAPLFB_MAX_NUM_DEVICES)
	{
		gapsDevInfo[uiFBDevID] = psDevInfo;
	}
}

static inline OMAPLFB_BOOL SwapChainHasChanged(OMAPLFB_DEVINFO *psDevInfo, OMAPLFB_SWAPCHAIN *psSwapChain)
{
	return (psDevInfo->psSwapChain != psSwapChain) ||
		(psDevInfo->uiSwapChainID != psSwapChain->uiSwapChainID);
}

static inline OMAPLFB_BOOL DontWaitForVSync(OMAPLFB_DEVINFO *psDevInfo)
{
	OMAPLFB_BOOL bDontWait;

	if (psDevInfo->ignore_sync)
		return OMAPLFB_TRUE;

	bDontWait = OMAPLFBAtomicBoolRead(&psDevInfo->sBlanked) ||
			OMAPLFBAtomicBoolRead(&psDevInfo->sFlushCommands);

#if defined(CONFIG_HAS_EARLYSUSPEND)
	bDontWait = bDontWait || OMAPLFBAtomicBoolRead(&psDevInfo->sEarlySuspendFlag);
#endif
#if defined(SUPPORT_DRI_DRM)
	bDontWait = bDontWait || OMAPLFBAtomicBoolRead(&psDevInfo->sLeaveVT);
#endif
	return bDontWait;
}

static IMG_VOID SetDCState(IMG_HANDLE hDevice, IMG_UINT32 ui32State)
{
	OMAPLFB_DEVINFO *psDevInfo = (OMAPLFB_DEVINFO *)hDevice;

	switch (ui32State)
	{
		case DC_STATE_FLUSH_COMMANDS:
			OMAPLFBAtomicBoolSet(&psDevInfo->sFlushCommands, OMAPLFB_TRUE);
			break;
		case DC_STATE_NO_FLUSH_COMMANDS:
			OMAPLFBAtomicBoolSet(&psDevInfo->sFlushCommands, OMAPLFB_FALSE);
			break;
		default:
			break;
	}
}

static PVRSRV_ERROR OpenDCDevice(IMG_UINT32 uiPVRDevID,
                                 IMG_HANDLE *phDevice,
                                 PVRSRV_SYNC_DATA* psSystemBufferSyncData)
{
	OMAPLFB_DEVINFO *psDevInfo;
	unsigned uiMaxFBDevIDPlusOne = OMAPLFBMaxFBDevIDPlusOne();
	unsigned i;

	for (i = 0; i < uiMaxFBDevIDPlusOne; i++)
	{
		psDevInfo = OMAPLFBGetDevInfoPtr(i);
		if (psDevInfo != NULL && psDevInfo->uiPVRDevID == uiPVRDevID)
		{
			break;
		}
	}
	if (i == uiMaxFBDevIDPlusOne)
	{
		DEBUG_PRINTK((KERN_WARNING DRIVER_PREFIX
			": %s: PVR Device %u not found\n", __FUNCTION__, uiPVRDevID));
		return PVRSRV_ERROR_INVALID_DEVICE;
	}

	
	psDevInfo->sSystemBuffer.psSyncData = psSystemBufferSyncData;
	
	*phDevice = (IMG_HANDLE)psDevInfo;
	
	return PVRSRV_OK;
}

static PVRSRV_ERROR CloseDCDevice(IMG_HANDLE hDevice)
{
#if defined(SUPPORT_DRI_DRM)
	OMAPLFB_DEVINFO *psDevInfo = (OMAPLFB_DEVINFO *)hDevice;

	OMAPLFBAtomicBoolSet(&psDevInfo->sLeaveVT, OMAPLFB_FALSE);
	(void) OMAPLFBUnblankDisplay(psDevInfo);
#else
	UNREFERENCED_PARAMETER(hDevice);
#endif
	return PVRSRV_OK;
}

static PVRSRV_ERROR EnumDCFormats(IMG_HANDLE hDevice,
                                  IMG_UINT32 *pui32NumFormats,
                                  DISPLAY_FORMAT *psFormat)
{
	OMAPLFB_DEVINFO	*psDevInfo;
	
	if(!hDevice || !pui32NumFormats)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	psDevInfo = (OMAPLFB_DEVINFO*)hDevice;
	
	*pui32NumFormats = 1;
	
	if(psFormat)
	{
		psFormat[0] = psDevInfo->sDisplayFormat;
	}

	return PVRSRV_OK;
}

static PVRSRV_ERROR EnumDCDims(IMG_HANDLE hDevice, 
                               DISPLAY_FORMAT *psFormat,
                               IMG_UINT32 *pui32NumDims,
                               DISPLAY_DIMS *psDim)
{
	OMAPLFB_DEVINFO	*psDevInfo;

	if(!hDevice || !psFormat || !pui32NumDims)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	psDevInfo = (OMAPLFB_DEVINFO*)hDevice;

	*pui32NumDims = 1;

	
	if(psDim)
	{
		psDim[0] = psDevInfo->sDisplayDim;
	}
	
	return PVRSRV_OK;
}


static PVRSRV_ERROR GetDCSystemBuffer(IMG_HANDLE hDevice, IMG_HANDLE *phBuffer)
{
	OMAPLFB_DEVINFO	*psDevInfo;
	
	if(!hDevice || !phBuffer)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	psDevInfo = (OMAPLFB_DEVINFO*)hDevice;

	*phBuffer = (IMG_HANDLE)&psDevInfo->sSystemBuffer;

	return PVRSRV_OK;
}


static PVRSRV_ERROR GetDCInfo(IMG_HANDLE hDevice, DISPLAY_INFO *psDCInfo)
{
	OMAPLFB_DEVINFO	*psDevInfo;
	
	if(!hDevice || !psDCInfo)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	psDevInfo = (OMAPLFB_DEVINFO*)hDevice;

	*psDCInfo = psDevInfo->sDisplayInfo;

	return PVRSRV_OK;
}

static PVRSRV_ERROR GetDCBufferAddr(IMG_HANDLE        hDevice,
                                    IMG_HANDLE        hBuffer, 
                                    IMG_SYS_PHYADDR   **ppsSysAddr,
                                    IMG_UINT32        *pui32ByteSize,
                                    IMG_VOID          **ppvCpuVAddr,
                                    IMG_HANDLE        *phOSMapInfo,
                                    IMG_BOOL          *pbIsContiguous,
	                                IMG_UINT32		  *pui32TilingStride)
{
	OMAPLFB_DEVINFO	*psDevInfo;
	OMAPLFB_BUFFER *psSystemBuffer;

	UNREFERENCED_PARAMETER(pui32TilingStride);

	if(!hDevice)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	if(!hBuffer)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	if (!ppsSysAddr)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	if (!pui32ByteSize)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	psDevInfo = (OMAPLFB_DEVINFO*)hDevice;

	psSystemBuffer = (OMAPLFB_BUFFER *)hBuffer;

	*ppsSysAddr = &psSystemBuffer->sSysAddr;

	*pui32ByteSize = (IMG_UINT32)psDevInfo->sFBInfo.ulBufferSize;

	if (ppvCpuVAddr)
	{
		*ppvCpuVAddr = psDevInfo->sFBInfo.bIs2D ? NULL : psSystemBuffer->sCPUVAddr;
	}

	if (phOSMapInfo)
	{
		*phOSMapInfo = (IMG_HANDLE)0;
	}

	if (pbIsContiguous)
	{
		*pbIsContiguous = !psDevInfo->sFBInfo.bIs2D;
	}

#if defined(CONFIG_TI_TILER)
	if (psDevInfo->sFBInfo.bIs2D) {
		int i = (psSystemBuffer->sSysAddr.uiAddr - psDevInfo->sFBInfo.psPageList->uiAddr) >> PAGE_SHIFT;
		*ppsSysAddr = psDevInfo->sFBInfo.psPageList + psDevInfo->sFBInfo.ulHeight * i;
	}
#endif

	return PVRSRV_OK;
}

static PVRSRV_ERROR CreateDCSwapChain(IMG_HANDLE hDevice,
                                      IMG_UINT32 ui32Flags,
                                      DISPLAY_SURF_ATTRIBUTES *psDstSurfAttrib,
                                      DISPLAY_SURF_ATTRIBUTES *psSrcSurfAttrib,
                                      IMG_UINT32 ui32BufferCount,
                                      PVRSRV_SYNC_DATA **ppsSyncData,
                                      IMG_UINT32 ui32OEMFlags,
                                      IMG_HANDLE *phSwapChain,
                                      IMG_UINT32 *pui32SwapChainID)
{
	OMAPLFB_DEVINFO	*psDevInfo;
	OMAPLFB_SWAPCHAIN *psSwapChain;
	OMAPLFB_BUFFER *psBuffer;
	IMG_UINT32 i;
	PVRSRV_ERROR eError;
	IMG_UINT32 ui32BuffersToSkip;
	struct bventry *bv_entry;

	UNREFERENCED_PARAMETER(ui32OEMFlags);
	
	
	if(!hDevice
	|| !psDstSurfAttrib
	|| !psSrcSurfAttrib
	|| !ppsSyncData
	|| !phSwapChain)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}

	psDevInfo = (OMAPLFB_DEVINFO*)hDevice;
	
	
	if (psDevInfo->sDisplayInfo.ui32MaxSwapChains == 0)
	{
		return PVRSRV_ERROR_NOT_SUPPORTED;
	}

	OMAPLFBCreateSwapChainLock(psDevInfo);

	
	if(psDevInfo->psSwapChain != NULL)
	{
		eError = PVRSRV_ERROR_FLIP_CHAIN_EXISTS;
		goto ExitUnLock;
	}
	
	
	if(ui32BufferCount > psDevInfo->sDisplayInfo.ui32MaxSwapChainBuffers)
	{
		eError = PVRSRV_ERROR_TOOMANYBUFFERS;
		goto ExitUnLock;
	}
	
	if ((psDevInfo->sFBInfo.ulRoundedBufferSize * (unsigned long)ui32BufferCount) > psDevInfo->sFBInfo.ulFBSize)
	{
		eError = PVRSRV_ERROR_TOOMANYBUFFERS;
		goto ExitUnLock;
	}

	
	ui32BuffersToSkip = psDevInfo->sDisplayInfo.ui32MaxSwapChainBuffers - ui32BufferCount;

	
	if(psDstSurfAttrib->pixelformat != psDevInfo->sDisplayFormat.pixelformat
	|| psDstSurfAttrib->sDims.ui32ByteStride != psDevInfo->sDisplayDim.ui32ByteStride
	|| psDstSurfAttrib->sDims.ui32Width != psDevInfo->sDisplayDim.ui32Width
	|| psDstSurfAttrib->sDims.ui32Height != psDevInfo->sDisplayDim.ui32Height)
	{
		
		eError = PVRSRV_ERROR_INVALID_PARAMS;
		goto ExitUnLock;
	}		

	if(psDstSurfAttrib->pixelformat != psSrcSurfAttrib->pixelformat
	|| psDstSurfAttrib->sDims.ui32ByteStride != psSrcSurfAttrib->sDims.ui32ByteStride
	|| psDstSurfAttrib->sDims.ui32Width != psSrcSurfAttrib->sDims.ui32Width
	|| psDstSurfAttrib->sDims.ui32Height != psSrcSurfAttrib->sDims.ui32Height)
	{
		
		eError = PVRSRV_ERROR_INVALID_PARAMS;
		goto ExitUnLock;
	}		

	UNREFERENCED_PARAMETER(ui32Flags);
	
#if defined(PVR_OMAPFB3_UPDATE_MODE)
	if (!OMAPLFBSetUpdateMode(psDevInfo, PVR_OMAPFB3_UPDATE_MODE))
	{
		printk(KERN_WARNING DRIVER_PREFIX ": %s: Device %u: Couldn't set frame buffer update mode %d\n", __FUNCTION__, psDevInfo->uiFBDevID, PVR_OMAPFB3_UPDATE_MODE);
	}
#endif
	
	psSwapChain = (OMAPLFB_SWAPCHAIN*)OMAPLFBAllocKernelMem(sizeof(OMAPLFB_SWAPCHAIN));
	if(!psSwapChain)
	{
		eError = PVRSRV_ERROR_OUT_OF_MEMORY;
		goto ExitUnLock;
	}

	psBuffer = (OMAPLFB_BUFFER*)OMAPLFBAllocKernelMem(sizeof(OMAPLFB_BUFFER) * ui32BufferCount);
	if(!psBuffer)
	{
		eError = PVRSRV_ERROR_OUT_OF_MEMORY;
		goto ErrorFreeSwapChain;
	}

	psSwapChain->ulBufferCount = (unsigned long)ui32BufferCount;
	psSwapChain->psBuffer = psBuffer;
	psSwapChain->bNotVSynced = OMAPLFB_TRUE;
	psSwapChain->uiFBDevID = psDevInfo->uiFBDevID;

	
	for(i=0; i<ui32BufferCount-1; i++)
	{
		psBuffer[i].psNext = &psBuffer[i+1];
	}
	
	psBuffer[i].psNext = &psBuffer[0];

	for(i=0; i<ui32BufferCount; i++)
	{
		IMG_UINT32 ui32SwapBuffer = i + ui32BuffersToSkip;
		IMG_UINT32 ui32BufferOffset = ui32SwapBuffer * (IMG_UINT32)psDevInfo->sFBInfo.ulRoundedBufferSize;
		if (psDevInfo->sFBInfo.bIs2D)
		{
			ui32BufferOffset = 0;
		}

		psBuffer[i].psSyncData = ppsSyncData[i];
		psBuffer[i].sSysAddr.uiAddr = psDevInfo->sFBInfo.sSysAddr.uiAddr + ui32BufferOffset;
		psBuffer[i].sCPUVAddr = psDevInfo->sFBInfo.sCPUVAddr + ui32BufferOffset;
		psBuffer[i].ulYOffset = ui32BufferOffset / psDevInfo->sFBInfo.ulByteStride;
		if (psDevInfo->sFBInfo.bIs2D)
		{
			psBuffer[i].sSysAddr.uiAddr += ui32SwapBuffer *
				ALIGN((IMG_UINT32)psDevInfo->sFBInfo.ulWidth * psDevInfo->sFBInfo.uiBytesPerPixel, PAGE_SIZE);
		}

		DEBUG_PRINTK((KERN_INFO DRIVER_PREFIX
				": %s: VRAM buf num [%u] addr = %08x, offset = %u\n",
				__FUNCTION__, i, psBuffer[i].sSysAddr.uiAddr, ui32BufferOffset));
		psBuffer[i].psDevInfo = psDevInfo;
		OMAPLFBInitBufferForSwap(&psBuffer[i]);
		psBuffer[i].bvmap_handle = NULL;
	}

	if (!bv_gc2d_is_present)
		goto skip_bv_map;
	else
		bv_entry = &bv_gc2d_entry;

	for (i = 0; i < ui32BufferCount; i++) {
		OMAPLFB_FBINFO *psPVRFBInfo = &psDevInfo->sFBInfo;
		unsigned long phy_addr = psBuffer[i].sSysAddr.uiAddr;
		unsigned int num_pages;
		unsigned long *page_addrs;
		enum bverror bv_error;
		int j;
		struct bvbuffdesc *buffdesc;

		if (psPVRFBInfo->bIs2D) {
			int pages_per_row =
				psPVRFBInfo->ulByteStride >> PAGE_SHIFT;
			num_pages = pages_per_row * psPVRFBInfo->ulHeight;
		} else {
			num_pages = ((psPVRFBInfo->ulByteStride *
				psPVRFBInfo->ulHeight) + PAGE_SIZE - 1) >>
				PAGE_SHIFT;
		}

		page_addrs = kzalloc(sizeof(*page_addrs) *
			num_pages, GFP_KERNEL);
		if (!page_addrs) {
			WARN(1, "%s: Out of memory\n", __func__);
			continue;
		}

		buffdesc = kzalloc(sizeof(*buffdesc), GFP_KERNEL);
		if (!buffdesc) {
			WARN(1, "%s: Out of memory\n", __func__);
			kfree(page_addrs);
			continue;
		}

		if (psDevInfo->sFBInfo.bIs2D) {
			int pages_offset = (phy_addr -
				psPVRFBInfo->psPageList->uiAddr) >> PAGE_SHIFT;
			IMG_SYS_PHYADDR *base_sysaddr =
				psPVRFBInfo->psPageList +
				psPVRFBInfo->ulHeight * pages_offset;
			for (j = 0; j < num_pages; j++) {
				page_addrs[j] = base_sysaddr->uiAddr;
				base_sysaddr++;
			}
		} else {
			for (j = 0; j < num_pages; j++)
				page_addrs[j] = phy_addr + (j * PAGE_SIZE);
		}

		buffdesc->structsize = sizeof(*buffdesc);
		buffdesc->pagesize = PAGE_SIZE;
		buffdesc->pagearray = page_addrs;
		buffdesc->pagecount = num_pages;

		bv_error = bv_entry->bv_map(buffdesc);
		if (bv_error) {
			WARN(1, "%s: BV map swapchain buffer failed %d\n",
				__func__, bv_error);
			psBuffer[i].bvmap_handle = NULL;
			kfree(buffdesc);
		} else
			psBuffer[i].bvmap_handle = buffdesc;

		kfree(page_addrs);
	}
skip_bv_map:

	if (OMAPLFBCreateSwapQueue(psSwapChain) != OMAPLFB_OK)
	{ 
		printk(KERN_WARNING DRIVER_PREFIX ": %s: Device %u: Failed to create workqueue\n", __FUNCTION__, psDevInfo->uiFBDevID);
		eError = PVRSRV_ERROR_UNABLE_TO_INSTALL_ISR;
		goto ErrorFreeBuffers;
	}

	if (OMAPLFBEnableLFBEventNotification(psDevInfo)!= OMAPLFB_OK)
	{
		eError = PVRSRV_ERROR_UNABLE_TO_ENABLE_EVENT;
		printk(KERN_WARNING DRIVER_PREFIX ": %s: Device %u: Couldn't enable framebuffer event notification\n", __FUNCTION__, psDevInfo->uiFBDevID);
		goto ErrorDestroySwapQueue;
	}

	psDevInfo->uiSwapChainID++;
	if (psDevInfo->uiSwapChainID == 0)
	{
		psDevInfo->uiSwapChainID++;
	}

	psSwapChain->uiSwapChainID = psDevInfo->uiSwapChainID;

	psDevInfo->psSwapChain = psSwapChain;

	*pui32SwapChainID = psDevInfo->uiSwapChainID;

	*phSwapChain = (IMG_HANDLE)psSwapChain;

	eError = PVRSRV_OK;
	goto ExitUnLock;

ErrorDestroySwapQueue:
	OMAPLFBDestroySwapQueue(psSwapChain);
ErrorFreeBuffers:
	OMAPLFBFreeKernelMem(psBuffer);
ErrorFreeSwapChain:
	OMAPLFBFreeKernelMem(psSwapChain);
ExitUnLock:
	OMAPLFBCreateSwapChainUnLock(psDevInfo);
	return eError;
}

static PVRSRV_ERROR DestroyDCSwapChain(IMG_HANDLE hDevice,
	IMG_HANDLE hSwapChain)
{
	OMAPLFB_DEVINFO	*psDevInfo;
	OMAPLFB_SWAPCHAIN *psSwapChain;
	OMAPLFB_ERROR eError;
	int i;
	
	if(!hDevice || !hSwapChain)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
	
	psDevInfo = (OMAPLFB_DEVINFO*)hDevice;
	psSwapChain = (OMAPLFB_SWAPCHAIN*)hSwapChain;

	OMAPLFBCreateSwapChainLock(psDevInfo);

	if (SwapChainHasChanged(psDevInfo, psSwapChain))
	{
		printk(KERN_WARNING DRIVER_PREFIX
			": %s: Device %u: Swap chain mismatch\n", __FUNCTION__, psDevInfo->uiFBDevID);

		eError = PVRSRV_ERROR_INVALID_PARAMS;
		goto ExitUnLock;
	}

	
	OMAPLFBDestroySwapQueue(psSwapChain);

	eError = OMAPLFBDisableLFBEventNotification(psDevInfo);
	if (eError != OMAPLFB_OK)
	{
		printk(KERN_WARNING DRIVER_PREFIX ": %s: Device %u: Couldn't disable framebuffer event notification\n", __FUNCTION__, psDevInfo->uiFBDevID);
	}

	if (bv_gc2d_is_present) {
		struct bventry *bv_entry = &bv_gc2d_entry;

		for (i = 0; i < psSwapChain->ulBufferCount; i++) {
			struct bvbuffdesc *buffdesc;

			if (!psSwapChain->psBuffer[i].bvmap_handle)
				continue;

			buffdesc = psSwapChain->psBuffer[i].bvmap_handle;
			bv_entry->bv_unmap(buffdesc);
			kfree(buffdesc);
		}
	}

	OMAPLFBFreeKernelMem(psSwapChain->psBuffer);
	OMAPLFBFreeKernelMem(psSwapChain);

	psDevInfo->psSwapChain = NULL;

	OMAPLFBFlip(psDevInfo, &psDevInfo->sSystemBuffer);
	(void) OMAPLFBCheckModeAndSync(psDevInfo);

	eError = PVRSRV_OK;

ExitUnLock:
	OMAPLFBCreateSwapChainUnLock(psDevInfo);

	return eError;
}

static PVRSRV_ERROR SetDCDstRect(IMG_HANDLE hDevice,
	IMG_HANDLE hSwapChain,
	IMG_RECT *psRect)
{
	UNREFERENCED_PARAMETER(hDevice);
	UNREFERENCED_PARAMETER(hSwapChain);
	UNREFERENCED_PARAMETER(psRect);

	
	
	return PVRSRV_ERROR_NOT_SUPPORTED;
}

static PVRSRV_ERROR SetDCSrcRect(IMG_HANDLE hDevice,
                                 IMG_HANDLE hSwapChain,
                                 IMG_RECT *psRect)
{
	UNREFERENCED_PARAMETER(hDevice);
	UNREFERENCED_PARAMETER(hSwapChain);
	UNREFERENCED_PARAMETER(psRect);

	

	return PVRSRV_ERROR_NOT_SUPPORTED;
}

static PVRSRV_ERROR SetDCDstColourKey(IMG_HANDLE hDevice,
                                      IMG_HANDLE hSwapChain,
                                      IMG_UINT32 ui32CKColour)
{
	UNREFERENCED_PARAMETER(hDevice);
	UNREFERENCED_PARAMETER(hSwapChain);
	UNREFERENCED_PARAMETER(ui32CKColour);

	

	return PVRSRV_ERROR_NOT_SUPPORTED;
}

static PVRSRV_ERROR SetDCSrcColourKey(IMG_HANDLE hDevice,
                                      IMG_HANDLE hSwapChain,
                                      IMG_UINT32 ui32CKColour)
{
	UNREFERENCED_PARAMETER(hDevice);
	UNREFERENCED_PARAMETER(hSwapChain);
	UNREFERENCED_PARAMETER(ui32CKColour);

	

	return PVRSRV_ERROR_NOT_SUPPORTED;
}

static PVRSRV_ERROR GetDCBuffers(IMG_HANDLE hDevice,
                                 IMG_HANDLE hSwapChain,
                                 IMG_UINT32 *pui32BufferCount,
                                 IMG_HANDLE *phBuffer)
{
	OMAPLFB_DEVINFO   *psDevInfo;
	OMAPLFB_SWAPCHAIN *psSwapChain;
	PVRSRV_ERROR eError;
	unsigned i;
	
	
	if(!hDevice 
	|| !hSwapChain
	|| !pui32BufferCount
	|| !phBuffer)
	{
		return PVRSRV_ERROR_INVALID_PARAMS;
	}
	
	psDevInfo = (OMAPLFB_DEVINFO*)hDevice;
	psSwapChain = (OMAPLFB_SWAPCHAIN*)hSwapChain;

	OMAPLFBCreateSwapChainLock(psDevInfo);

	if (SwapChainHasChanged(psDevInfo, psSwapChain))
	{
		printk(KERN_WARNING DRIVER_PREFIX
			": %s: Device %u: Swap chain mismatch\n", __FUNCTION__, psDevInfo->uiFBDevID);

		eError = PVRSRV_ERROR_INVALID_PARAMS;
		goto Exit;
	}
	
	
	*pui32BufferCount = (IMG_UINT32)psSwapChain->ulBufferCount;
	
	
	for(i=0; i<psSwapChain->ulBufferCount; i++)
	{
		phBuffer[i] = (IMG_HANDLE)&psSwapChain->psBuffer[i];
	}
	
	eError = PVRSRV_OK;

Exit:
	OMAPLFBCreateSwapChainUnLock(psDevInfo);

	return eError;
}

static PVRSRV_ERROR SwapToDCBuffer(IMG_HANDLE hDevice,
                                   IMG_HANDLE hBuffer,
                                   IMG_UINT32 ui32SwapInterval,
                                   IMG_HANDLE hPrivateTag,
                                   IMG_UINT32 ui32ClipRectCount,
                                   IMG_RECT *psClipRect)
{
	UNREFERENCED_PARAMETER(hDevice);
	UNREFERENCED_PARAMETER(hBuffer);
	UNREFERENCED_PARAMETER(ui32SwapInterval);
	UNREFERENCED_PARAMETER(hPrivateTag);
	UNREFERENCED_PARAMETER(ui32ClipRectCount);
	UNREFERENCED_PARAMETER(psClipRect);
	
	

	return PVRSRV_OK;
}

static PVRSRV_ERROR SwapToDCSystem(IMG_HANDLE hDevice,
                                   IMG_HANDLE hSwapChain)
{
	UNREFERENCED_PARAMETER(hDevice);
	UNREFERENCED_PARAMETER(hSwapChain);
	
	
	return PVRSRV_OK;
}

static OMAPLFB_BOOL WaitForVSyncSettle(OMAPLFB_DEVINFO *psDevInfo)
{
		unsigned i;
		for(i = 0; i < OMAPLFB_VSYNC_SETTLE_COUNT; i++)
		{
			if (DontWaitForVSync(psDevInfo) || !OMAPLFBWaitForVSync(psDevInfo))
			{
				return OMAPLFB_FALSE;
			}
		}

		return OMAPLFB_TRUE;
}

void OMAPLFBSwapHandler(OMAPLFB_BUFFER *psBuffer)
{
	OMAPLFB_DEVINFO *psDevInfo = psBuffer->psDevInfo;
	OMAPLFB_SWAPCHAIN *psSwapChain = psDevInfo->psSwapChain;
	OMAPLFB_BOOL bPreviouslyNotVSynced;

#if defined(SUPPORT_DRI_DRM)
	if (!OMAPLFBAtomicBoolRead(&psDevInfo->sLeaveVT))
#endif
	{
		OMAPLFBFlip(psDevInfo, psBuffer);
	}

	bPreviouslyNotVSynced = psSwapChain->bNotVSynced;
	psSwapChain->bNotVSynced = OMAPLFB_TRUE;


	if (!DontWaitForVSync(psDevInfo))
	{
		OMAPLFB_UPDATE_MODE eMode = OMAPLFBGetUpdateMode(psDevInfo);
		int iBlankEvents = OMAPLFBAtomicIntRead(&psDevInfo->sBlankEvents);

		switch(eMode)
		{
			case OMAPLFB_UPDATE_MODE_AUTO:
				psSwapChain->bNotVSynced = OMAPLFB_FALSE;

				if (bPreviouslyNotVSynced || psSwapChain->iBlankEvents != iBlankEvents)
				{
					psSwapChain->iBlankEvents = iBlankEvents;
					psSwapChain->bNotVSynced = !WaitForVSyncSettle(psDevInfo);
				} else if (psBuffer->ulSwapInterval != 0)
				{
					psSwapChain->bNotVSynced = !OMAPLFBWaitForVSync(psDevInfo);
				}
				break;
#if defined(PVR_OMAPFB3_MANUAL_UPDATE_SYNC_IN_SWAP)
			case OMAPLFB_UPDATE_MODE_MANUAL:
				if (psBuffer->ulSwapInterval != 0)
				{
					(void) OMAPLFBManualSync(psDevInfo);
				}
				break;
#endif
			default:
				break;
		}
	}

	psDevInfo->sPVRJTable.pfnPVRSRVCmdComplete((IMG_HANDLE)psBuffer->hCmdComplete, IMG_TRUE);
}

#if defined(CONFIG_DSSCOMP)

#include <mach/tiler.h>
#include <video/dsscomp.h>
#include <plat/dsscomp.h>
#include "../services_headers.h"

void sgx_idle_log_flip(void);

static void dsscomp_proxy_cmdcomplete(void * cookie, int i)
{
	COMMAND_COMPLETE_DATA *psCmdCompleteData = (COMMAND_COMPLETE_DATA *)cookie;

	sgx_idle_log_flip();
	/* XXX: assumes that there is only one display */
	/* LK: Check if we are destroying sync objects. In this case
	 * we are now in "late" callback, so we can't allow callback
	 * to access sync objects memory.To do this just set zero to
	 * count of source and destination sync objects.
	 */
	if (NULL == gapsDevInfo[0]->psSwapChain) {
		psCmdCompleteData->ui32SrcSyncCount = 0;
		psCmdCompleteData->ui32DstSyncCount = 0;
	}
	gapsDevInfo[0]->sPVRJTable.pfnPVRSRVCmdComplete(cookie, i);
}

static IMG_BOOL ProcessFlipV1(IMG_HANDLE hCmdCookie,
							  OMAPLFB_DEVINFO *psDevInfo,
							  OMAPLFB_SWAPCHAIN *psSwapChain,
							  OMAPLFB_BUFFER *psBuffer,
							  unsigned long ulSwapInterval)
{
	OMAPLFBCreateSwapChainLock(psDevInfo);

	
	if (SwapChainHasChanged(psDevInfo, psSwapChain))
	{
		DEBUG_PRINTK((KERN_WARNING DRIVER_PREFIX
			": %s: Device %u (PVR Device ID %u): The swap chain has been destroyed\n",
			__FUNCTION__, psDevInfo->uiFBDevID, psDevInfo->uiPVRDevID));
	}
	else
	{
		psBuffer->hCmdComplete = (OMAPLFB_HANDLE)hCmdCookie;
		psBuffer->ulSwapInterval = ulSwapInterval;
		if (is_tiler_addr(psBuffer->sSysAddr.uiAddr)) {
			IMG_UINT32 w = psBuffer->psDevInfo->sDisplayDim.ui32Width;
			IMG_UINT32 h = psBuffer->psDevInfo->sDisplayDim.ui32Height;
			struct dsscomp_setup_dispc_data comp = {
				.num_mgrs = 1,
				.mgrs[0].alpha_blending = 1,
				.num_ovls = 1,
				.ovls[0].cfg = {
					.width = w,
					.win.w = w,
					.crop.w = w,
					.height = h,
					.win.h = h,
					.crop.h = h,
					.stride = psBuffer->psDevInfo->sDisplayDim.ui32ByteStride,
					.color_mode = OMAP_DSS_COLOR_ARGB32,
					.enabled = 1,
					.global_alpha = 255,
				},
				.mode = DSSCOMP_SETUP_DISPLAY,
			};
			struct tiler_pa_info *pas[1] = { NULL };
			comp.ovls[0].ba = (u32) psBuffer->sSysAddr.uiAddr;
			if (!psDevInfo->ignore_sync) {
				dsscomp_gralloc_queue(&comp, pas, true,
					      dsscomp_proxy_cmdcomplete,
					      (void *) psBuffer->hCmdComplete);
			} else {
				dsscomp_gralloc_queue(&comp, pas, true,
						      NULL, NULL);
				dsscomp_proxy_cmdcomplete(
					      psBuffer->hCmdComplete, IMG_TRUE);
			}
		} else {
			OMAPLFBQueueBufferForSwap(psSwapChain, psBuffer);
		}
	}

	OMAPLFBCreateSwapChainUnLock(psDevInfo);

	return IMG_TRUE;
}

#include "servicesint.h"
#include "services.h"
#include "mm.h"

/* FIXME: Manual clipping, this should be done in bltsville not here */
static void clip_rects(struct bvbltparams *pParmsIn)
{
	if (pParmsIn->dstrect.left < 0 || pParmsIn->dstrect.top < 0 ||
		(pParmsIn->dstrect.left + pParmsIn->dstrect.width) >
			pParmsIn->dstgeom->width ||
		(pParmsIn->dstrect.top + pParmsIn->dstrect.height) >
			pParmsIn->dstgeom->height) {

		/* Adjust src1 */
		if (pParmsIn->dstrect.left < 0) {
			pParmsIn->src1rect.left = abs(pParmsIn->dstrect.left);
			pParmsIn->src1rect.width -= pParmsIn->src1rect.left;
			pParmsIn->dstrect.left = 0;
		}

		if (pParmsIn->dstrect.top < 0) {
			pParmsIn->src1rect.top = abs(pParmsIn->dstrect.top);
			pParmsIn->src1rect.height -= pParmsIn->src1rect.top;
			pParmsIn->dstrect.top = 0;
		}

		if (pParmsIn->dstrect.left + pParmsIn->dstrect.width >
			pParmsIn->dstgeom->width) {
			pParmsIn->dstrect.width -= (pParmsIn->dstrect.left +
				pParmsIn->dstrect.width) -
				pParmsIn->dstgeom->width;
			pParmsIn->src1rect.width = pParmsIn->dstrect.width;
		}

		if (pParmsIn->dstrect.top + pParmsIn->dstrect.height >
			pParmsIn->dstgeom->height) {
			pParmsIn->dstrect.height -= (pParmsIn->dstrect.top +
				pParmsIn->dstrect.height) -
				pParmsIn->dstgeom->height;
			pParmsIn->src1rect.height = pParmsIn->dstrect.height;
		}

		/* Adjust src2 */
		if (pParmsIn->dstrect.left < 0) {
			pParmsIn->src2rect.left = abs(pParmsIn->dstrect.left);
			pParmsIn->src2rect.width -= pParmsIn->src2rect.left;
			pParmsIn->dstrect.left = 0;
		}

		if (pParmsIn->dstrect.top < 0) {
			pParmsIn->src2rect.top = abs(pParmsIn->dstrect.top);
			pParmsIn->src2rect.height -= pParmsIn->src2rect.top;
			pParmsIn->dstrect.top = 0;
		}

		if (pParmsIn->dstrect.left + pParmsIn->dstrect.width >
			pParmsIn->dstgeom->width) {
			pParmsIn->dstrect.width -= (pParmsIn->dstrect.left +
				pParmsIn->dstrect.width) -
				pParmsIn->dstgeom->width;
			pParmsIn->src2rect.width = pParmsIn->dstrect.width;
		}

		if (pParmsIn->dstrect.top + pParmsIn->dstrect.height >
			pParmsIn->dstgeom->height) {
			pParmsIn->dstrect.height -= (pParmsIn->dstrect.top +
				pParmsIn->dstrect.height) -
				pParmsIn->dstgeom->height;
			pParmsIn->src2rect.height = pParmsIn->dstrect.height;
		}
	}
}

/* XXX: Useful for debugging blit parameters sent from HWC */
#if 0
static void print_bvparams(struct bvbltparams *bltparams)
{
	printk(KERN_INFO "%s: blit param rop %x, flags %d\n", __func__,
		bltparams->op.rop, bltparams->flags);

	printk(KERN_INFO "%s: blit dst %d,%d pos %d,%d region %d,%d"
		" stride %d\n", __func__,
		bltparams->dstgeom->width,
		bltparams->dstgeom->height,
		bltparams->dstrect.left, bltparams->dstrect.top,
		bltparams->dstrect.width, bltparams->dstrect.height,
		bltparams->dstgeom->physstride);

	printk(KERN_INFO "%s: blit src1 %d,%d pos %d,%d region %d,%d"
		" stride %d, meminfo %d\n", __func__,
		bltparams->src1geom->width,
		bltparams->src1geom->height, bltparams->src1rect.left,
		bltparams->src1rect.top, bltparams->src1rect.width,
		bltparams->src1rect.height, bltparams->src1geom->physstride,
		(int)bltparams->src1.desc->virtaddr);

	if (!(bltparams->flags & BVFLAG_BLEND))
		return;

	printk(KERN_INFO "%s: blit src2 %d,%d pos %d,%d region %d,%d"
		" stride %d, meminfo %d\n", __func__,
		bltparams->src2geom->width,
		bltparams->src2geom->height, bltparams->src2rect.left,
		bltparams->src2rect.top, bltparams->src2rect.width,
		bltparams->src2rect.height, bltparams->src2geom->physstride,
		(int)bltparams->src2.desc->virtaddr);
}
#endif

static enum bverror bv_map_meminfo(OMAPLFB_DEVINFO *psDevInfo,
	struct bventry *bv_entry, struct bvbuffdesc *buffdesc,
	PDC_MEM_INFO *meminfo)
{
	IMG_CPU_PHYADDR phyAddr;
	IMG_UINT32 ui32NumPages;
	IMG_SIZE_T uByteSize;
	unsigned long *page_addrs;
	enum bverror bv_error;
	int i;

	psDevInfo->sPVRJTable.pfnPVRSRVDCMemInfoGetByteSize(*meminfo,
		&uByteSize);
	ui32NumPages = (uByteSize + PAGE_SIZE - 1) >> PAGE_SHIFT;
	page_addrs = kzalloc(sizeof(*page_addrs) * ui32NumPages, GFP_KERNEL);
	if (!page_addrs) {
		WARN(1, "%s: Out of memory\n", __func__);
		return BVERR_OOM;
	}

	for (i = 0; i < ui32NumPages; i++) {
		psDevInfo->sPVRJTable.pfnPVRSRVDCMemInfoGetCpuPAddr(
			*meminfo, i << PAGE_SHIFT, &phyAddr);
		page_addrs[i] = (u32)phyAddr.uiAddr;
	}

	/* Assume the structsize and length is already assigned */
	buffdesc->map = NULL;
	buffdesc->pagesize = PAGE_SIZE;
	buffdesc->pagearray = page_addrs;
	buffdesc->pagecount = ui32NumPages;
	buffdesc->pageoffset = 0;

	bv_error = bv_entry->bv_map(buffdesc);

	kfree(page_addrs);
	return bv_error;
}

static void get_fb_bvmap(OMAPLFB_DEVINFO *psDevInfo, PDC_MEM_INFO *meminfo,
	struct bvbuffdesc **buffdesc)
{
	OMAPLFB_SWAPCHAIN *psSwapChain = psDevInfo->psSwapChain;
	IMG_CPU_PHYADDR phyAddr;
	int i;

	*buffdesc = NULL;

	if (!bv_gc2d_is_present || !psSwapChain)
		return;

	psDevInfo->sPVRJTable.pfnPVRSRVDCMemInfoGetCpuPAddr(*meminfo,
		0, &phyAddr);
	for (i = psSwapChain->ulBufferCount - 1; i >= 0; i--) {

		if (!psSwapChain->psBuffer[i].bvmap_handle)
			continue;

		if (phyAddr.uiAddr ==
			psSwapChain->psBuffer[i].sSysAddr.uiAddr) {
			*buffdesc = psSwapChain->psBuffer[i].bvmap_handle;
			break;
		}
	}
}

static inline int meminfo_idx_valid(int meminfo_ix, int num_meminfos)
{
	if (meminfo_ix < 0 || meminfo_ix >= num_meminfos) {
		WARN(1, "%s: Invalid meminfo index %d, max %d\n",
			__func__, meminfo_ix, num_meminfos);
		return 0;
	}
	return 1;
}

static IMG_BOOL ProcessFlipV2(IMG_HANDLE hCmdCookie,
							  OMAPLFB_DEVINFO *psDevInfo,
							  PDC_MEM_INFO *ppsMemInfos,
							  IMG_UINT32 ui32NumMemInfos,
							  struct omap_hwc_data *psHwcData,
							  IMG_UINT32 uiHwcDataSz)
{
	struct tiler_pa_info *apsTilerPAs[5];
	IMG_UINT32 i, k, j;
	struct {
		IMG_UINTPTR_T uiAddr;
		IMG_UINTPTR_T uiUVAddr;
		struct tiler_pa_info *psTilerInfo;
	} asMemInfo[5];

	/* Framebuffer info just used to get FB geometry, the address to
	 * use for blitting (dst buffer) is the first meminfo
	 */
	struct rgz_blt_entry *entry_list;
	struct bventry *bv_entry = &bv_gc2d_entry;
	struct bvbuffdesc src1desc;
	struct bvbuffdesc src2desc;
	struct bvbuffdesc *dstdesc;
	struct bvsurfgeom src1geom;
	struct bvsurfgeom src2geom;
	struct bvsurfgeom dstgeom;
	struct bvbltparams bltparams;
	int rgz_items;
	int calcsz;
	struct dsscomp_setup_dispc_data *psDssData = &(psHwcData->dsscomp_data);
#if defined(CONFIG_OMAPLFB_CLONE_VRAM_TO_TILER)
	unsigned int requires_cloning = 0;
	static unsigned int allocate_tiler, num_frames;
#endif

	if (uiHwcDataSz <= offsetof(struct omap_hwc_data, blit_data))
		rgz_items = 0;
	else
		rgz_items = psHwcData->blit_data.rgz_items;


	psDssData = &(psHwcData->dsscomp_data);
	calcsz = sizeof(*psHwcData) +
		(sizeof(struct rgz_blt_entry) * rgz_items);

	if (rgz_items > 0 && !bv_gc2d_is_present) {
		/* We cannot blit if BV GC2D is not present!, likely a bug */
		WARN(1, "Trying to blit when BV GC2D is not present");
		rgz_items = 0; /* Prevent blits */
	}

	memset(asMemInfo, 0, sizeof(asMemInfo));

	/* Check the size of private data along with the blit operations */
	if (uiHwcDataSz != calcsz)
	{
		WARN(1, "invalid size of private data (%d vs %d)",
		     uiHwcDataSz, calcsz);
	}

	if(psDssData->num_ovls == 0 || ui32NumMemInfos == 0)
	{
		WARN(1, "must have at least one layer");
		return IMG_FALSE;
	}

	for (i = k = 0; i < ui32NumMemInfos && k < ARRAY_SIZE(apsTilerPAs) &&
		k < psDssData->num_ovls; i++, k++) {

		struct tiler_pa_info *psTilerInfo;
		IMG_CPU_VIRTADDR virtAddr;
		IMG_CPU_PHYADDR phyAddr;
		IMG_UINT32 ui32NumPages;
		IMG_SIZE_T uByteSize;
		int j;

		psDevInfo->sPVRJTable.pfnPVRSRVDCMemInfoGetByteSize(ppsMemInfos[i], &uByteSize);
		ui32NumPages = (uByteSize + PAGE_SIZE - 1) >> PAGE_SHIFT;

		psDevInfo->sPVRJTable.pfnPVRSRVDCMemInfoGetCpuPAddr(ppsMemInfos[i], 0, &phyAddr);

		/* TILER buffers do not need meminfos */
		if(is_tiler_addr((u32)phyAddr.uiAddr))
		{
			asMemInfo[k].uiAddr = phyAddr.uiAddr;
			if (tiler_fmt((u32)phyAddr.uiAddr) == TILFMT_8BIT) {

#if defined(SUPPORT_NV12_FROM_2_HWADDRS)
				/* NV12 buffers have 2 meminfos */
				BUG_ON(i + 1 >= ui32NumMemInfos);
				i++;
				psDevInfo->sPVRJTable.pfnPVRSRVDCMemInfoGetCpuPAddr(ppsMemInfos[i],
											0, &phyAddr);
				asMemInfo[k].uiUVAddr = phyAddr.uiAddr;
#else
				psDevInfo->sPVRJTable.pfnPVRSRVDCMemInfoGetCpuPAddr(ppsMemInfos[i],
									(uByteSize * 2) / 3, &phyAddr);
				asMemInfo[k].uiUVAddr = phyAddr.uiAddr;
#endif
			}
			continue;
		}

		if (phyAddr.uiAddr >= psDevInfo->psLINFBInfo->fix.smem_start &&
		    phyAddr.uiAddr < (psDevInfo->psLINFBInfo->fix.smem_start + psDevInfo->psLINFBInfo->fix.smem_len))
		{
			asMemInfo[k].uiAddr = phyAddr.uiAddr;
			continue;
		}

		/* normal gralloc layer */
		psTilerInfo = kzalloc(sizeof(*psTilerInfo), GFP_KERNEL);
		if(!psTilerInfo)
		{
			continue;
		}

		psTilerInfo->mem = kzalloc(sizeof(*psTilerInfo->mem) * ui32NumPages, GFP_KERNEL);
		if(!psTilerInfo->mem)
		{
			kfree(psTilerInfo);
			continue;
		}

		psTilerInfo->num_pg = ui32NumPages;
		psTilerInfo->memtype = TILER_MEM_USING;
		for(j = 0; j < ui32NumPages; j++)
		{
			psDevInfo->sPVRJTable.pfnPVRSRVDCMemInfoGetCpuPAddr(ppsMemInfos[i], j << PAGE_SHIFT, &phyAddr);
			psTilerInfo->mem[j] = (u32)phyAddr.uiAddr;
		}

		/* need base address for in-page offset */
		psDevInfo->sPVRJTable.pfnPVRSRVDCMemInfoGetCpuVAddr(ppsMemInfos[i], &virtAddr);
		asMemInfo[k].uiAddr = (IMG_UINTPTR_T) virtAddr;
		asMemInfo[k].psTilerInfo = psTilerInfo;
	}

	for(i = 0; i < psDssData->num_ovls; i++)
	{
		unsigned int ix;
#if defined(CONFIG_OMAPLFB_CLONE_VRAM_TO_TILER)
		unsigned int fb_phys_addr;
#endif
		apsTilerPAs[i] = NULL;

		/* only supporting Post2, cloned and fbmem layers */
		if (psDssData->ovls[i].addressing != OMAP_DSS_BUFADDR_LAYER_IX &&
		    psDssData->ovls[i].addressing != OMAP_DSS_BUFADDR_OVL_IX &&
		    psDssData->ovls[i].addressing != OMAP_DSS_BUFADDR_FB)
			psDssData->ovls[i].cfg.enabled = false;

		if (psDssData->ovls[i].addressing != OMAP_DSS_BUFADDR_LAYER_IX &&
		    psDssData->ovls[i].addressing != OMAP_DSS_BUFADDR_OVL_IX)
			continue;

		/* Post2 layers */
		ix = psDssData->ovls[i].ba;
		if (ix >= k)
		{
			WARN(1, "Invalid Post2 layer (%u)", ix);
			psDssData->ovls[i].cfg.enabled = false;
			continue;
		}

		if (psDssData->ovls[i].addressing == OMAP_DSS_BUFADDR_LAYER_IX) {
			psDssData->ovls[i].addressing = OMAP_DSS_BUFADDR_DIRECT;
			psDssData->ovls[i].ba = (u32) asMemInfo[ix].uiAddr;
			psDssData->ovls[i].uv = (u32) asMemInfo[ix].uiUVAddr;
			apsTilerPAs[i] = asMemInfo[ix].psTilerInfo;
		}

#if defined(CONFIG_OMAPLFB_CLONE_VRAM_TO_TILER)
		if (psDssData->ovls[i].addressing == OMAP_DSS_BUFADDR_OVL_IX)
			fb_phys_addr = fb_to_phys(psDevInfo, psDssData->ovls[ix].ba);
		else
			fb_phys_addr = fb_to_phys(psDevInfo, psDssData->ovls[i].ba);

		if (fb_phys_addr && !is_tiler_addr(fb_phys_addr) &&
			(psDssData->ovls[i].cfg.rotation & 1)) {
			static unsigned int flip;
			unsigned int dest_buf;
			requires_cloning++;

			if (!allocate_tiler) {
				if (OMAPLFBMotAllocTiler2d() == false) {
					printk(KERN_ERR DRIVER_PREFIX
					"Tiler: %s: Device %u: TILER memory could not allocated; HDMI mirroring fails\n",
					__FUNCTION__, psDevInfo->uiFBDevID);
				}
				allocate_tiler++;

				printk(KERN_INFO DRIVER_PREFIX
					": %s: Device %u (PVR Device ID %u): HDMI cloning enabled\n",
					__FUNCTION__, psDevInfo->uiFBDevID, psDevInfo->uiPVRDevID);
				/* config dma ch */
				OMAPLFBMotDmaConfig(psDevInfo->sFBInfo.ulWidth, psDevInfo->sFBInfo.ulHeight,
				(psDevInfo->psLINFBInfo->var.bits_per_pixel >> 3), psDevInfo->sFBInfo.ulByteStride, 32768);
			}

			dest_buf = sTilerMemAddr[flip].uiAddr;

			DEBUG_PRINTK((KERN_INFO DRIVER_PREFIX
					": Tiler: fb_phys_addr = %0x, dest_buf = %0x\n",
					fb_phys_addr, dest_buf));

			if (!fb_phys_addr || !dest_buf) {
				printk(KERN_ERR DRIVER_PREFIX
					"Tiler: %s: Device %u (PVR Device ID %u): Invalid vram or tiler memory\n",
					__FUNCTION__, psDevInfo->uiFBDevID, psDevInfo->uiPVRDevID);
			} else if (OMAPLFBMotDmaPerform(fb_phys_addr, dest_buf)) {
				printk(KERN_ERR DRIVER_PREFIX
					"Tiler: %s: Device %u (PVR Device ID %u): DMA copy failure\n",
					__FUNCTION__, psDevInfo->uiFBDevID, psDevInfo->uiPVRDevID);
			} else {
				tiler_pa_free(apsTilerPAs[i]);
				apsTilerPAs[i] = NULL;
				psDssData->ovls[i].addressing = OMAP_DSS_BUFADDR_DIRECT;
				psDssData->ovls[i].ba = dest_buf;
				psDssData->ovls[i].uv = 0;
				flip ^= 1;
			}
		}
#endif
	}

	/* DSS pipes are setup up to this point, we can begin blitting here */
	entry_list = (struct rgz_blt_entry *) (psHwcData->blit_data.rgz_blts);
	for (j = 0; j < rgz_items; j++) {
		struct rgz_blt_entry *entry = &entry_list[j];
		enum bverror bv_error = 0;
		int meminfo_ix;
		int src2_mapped = 0;

		/* BV Parameters data */
		bltparams = entry->bp;

		/* Src1 buffer data */
		src1desc = entry->src1desc;
		src1geom = entry->src1geom;
		src1geom.physstride = src1geom.virtstride;

		meminfo_ix = (int)src1desc.virtaddr;
		if (!meminfo_idx_valid(meminfo_ix, ui32NumMemInfos))
			continue;

		bv_error = bv_map_meminfo(psDevInfo, bv_entry, &src1desc,
			&ppsMemInfos[meminfo_ix]);
		if (bv_error) {
			WARN(1, "%s: BV map src1 failed %d\n",
				__func__, bv_error);
			continue;
		}

		/* Dst buffer data, assume meminfo 0 is the FB */
		dstgeom = entry->dstgeom;
		dstgeom.physstride = psDevInfo->sDisplayDim.ui32ByteStride;
		dstgeom.virtstride = dstgeom.physstride;

		get_fb_bvmap(psDevInfo, &ppsMemInfos[0], &dstdesc);
		if (!dstdesc) {
			WARN(1, "%s: BV map dst not found\n", __func__);
			goto unmap_srcs;
		}

		/* Src2 buffer data
		 * Check if this blit involves src2 as the FB or another
		 * buffer, if the last case is true then map the src2 buffer
		 */
		if (bltparams.flags & BVFLAG_BLEND) {
			if (entry->src2desc.virtaddr == 0) {
				/* Blending with destination (FB) */
				src2desc = *dstdesc;
				src2geom = dstgeom;
				src2_mapped = 0;
			} else {
				/* Blending with other buffer */
				src2desc = entry->src2desc;
				src2geom = entry->src2geom;
				src2geom.physstride = src2geom.virtstride;

				meminfo_ix = (int)src2desc.virtaddr;
				if (!meminfo_idx_valid(meminfo_ix,
					ui32NumMemInfos))
					goto unmap_srcs;

				bv_error = bv_map_meminfo(psDevInfo, bv_entry,
					&src2desc, &ppsMemInfos[meminfo_ix]);
				if (bv_error) {
					WARN(1, "%s: BV map dst failed %d\n",
						__func__, bv_error);
					goto unmap_srcs;
				}
				src2_mapped = 1;
			}
		}

		bltparams.dstdesc = dstdesc;
		bltparams.dstgeom = &dstgeom;
		bltparams.src1.desc = &src1desc;
		bltparams.src1geom = &src1geom;
		bltparams.src2.desc = &src2desc;
		bltparams.src2geom = &src2geom;

		/* FIXME: BV GC2D clipping support is not done properly,
		 * clip manually while this is fixed
		 */
		clip_rects(&bltparams);
#if 0
		print_bvparams(&bltparams);
#endif
		bv_error = bv_entry->bv_blt(&bltparams);
		if (bv_error)
			printk(KERN_ERR "%s: blit failed %d\n",
				__func__, bv_error);
unmap_srcs:
		bv_entry->bv_unmap(&src1desc);
		if (src2_mapped)
			bv_entry->bv_unmap(&src2desc);
	}

	if (!psDevInfo->ignore_sync)
	{
		dsscomp_gralloc_queue(psDssData, apsTilerPAs, false,
						  dsscomp_proxy_cmdcomplete,
						  (void *)hCmdCookie);
	} else {
		dsscomp_gralloc_queue(psDssData, apsTilerPAs, false,
							  NULL, NULL);
		dsscomp_proxy_cmdcomplete(hCmdCookie, IMG_TRUE);
	}
#if defined(CONFIG_OMAPLFB_CLONE_VRAM_TO_TILER)
	if (requires_cloning) {
		num_frames = 0;
	/*
	 * wait for 2 frames front/back
	 * buffer rendering complete before
	 * deallocate when HDMI plugged out
	 */
	} else if (allocate_tiler && ++num_frames >= 2) {
		OMAPLFBMotDeallocTiler2d();
		allocate_tiler = 0;
		printk(KERN_INFO DRIVER_PREFIX
			": %s: Device %u (PVR Device ID %u): HDMI cloning disabled\n",
			__FUNCTION__, psDevInfo->uiFBDevID, psDevInfo->uiPVRDevID);
	}
#endif
	for(i = 0; i < k; i++)
	{
		tiler_pa_free(apsTilerPAs[i]);
	}

	return IMG_TRUE;
}

#endif 

static IMG_BOOL ProcessFlip(IMG_HANDLE  hCmdCookie,
                            IMG_UINT32  ui32DataSize,
                            IMG_VOID   *pvData)
{
	DISPLAYCLASS_FLIP_COMMAND *psFlipCmd;
	OMAPLFB_DEVINFO *psDevInfo;

	if(!hCmdCookie || !pvData)
	{
		return IMG_FALSE;
	}

	psFlipCmd = (DISPLAYCLASS_FLIP_COMMAND*)pvData;

	if (psFlipCmd == IMG_NULL)
	{
		return IMG_FALSE;
	}

	psDevInfo = (OMAPLFB_DEVINFO*)psFlipCmd->hExtDevice;

	if(psFlipCmd->hExtBuffer)
	{
		return ProcessFlipV1(hCmdCookie,
							 psDevInfo,
							 psFlipCmd->hExtSwapChain,
							 psFlipCmd->hExtBuffer,
							 psFlipCmd->ui32SwapInterval);
	}
	else
	{
#if defined(CONFIG_DSSCOMP)
		DISPLAYCLASS_FLIP_COMMAND2 *psFlipCmd2;
		psFlipCmd2 = (DISPLAYCLASS_FLIP_COMMAND2 *)pvData;
		return ProcessFlipV2(hCmdCookie,
							 psDevInfo,
							 psFlipCmd2->ppsMemInfos,
							 psFlipCmd2->ui32NumMemInfos,
							 psFlipCmd2->pvPrivData,
							 psFlipCmd2->ui32PrivDataLength);
#else
		BUG();
#endif
	}
}

static OMAPLFB_ERROR OMAPLFBInitFBDev(OMAPLFB_DEVINFO *psDevInfo)
{
	struct fb_info *psLINFBInfo;
	struct module *psLINFBOwner;
	OMAPLFB_FBINFO *psPVRFBInfo = &psDevInfo->sFBInfo;
	OMAPLFB_ERROR eError = OMAPLFB_ERROR_GENERIC;
	unsigned long FBSize;
	unsigned long ulLCM;
	unsigned uiFBDevID = psDevInfo->uiFBDevID;

	OMAPLFB_CONSOLE_LOCK();

	psLINFBInfo = registered_fb[uiFBDevID];
	if (psLINFBInfo == NULL)
	{
		eError = OMAPLFB_ERROR_INVALID_DEVICE;
		goto ErrorRelSem;
	}

	FBSize = (psLINFBInfo->screen_size) != 0 ?
					psLINFBInfo->screen_size :
					psLINFBInfo->fix.smem_len;

	
	if (FBSize == 0 || psLINFBInfo->fix.line_length == 0)
	{
		eError = OMAPLFB_ERROR_INVALID_DEVICE;
		goto ErrorRelSem;
	}

	psLINFBOwner = psLINFBInfo->fbops->owner;
	if (!try_module_get(psLINFBOwner))
	{
		printk(KERN_INFO DRIVER_PREFIX
			": %s: Device %u: Couldn't get framebuffer module\n", __FUNCTION__, uiFBDevID);

		goto ErrorRelSem;
	}

	if (psLINFBInfo->fbops->fb_open != NULL)
	{
		int res;

		res = psLINFBInfo->fbops->fb_open(psLINFBInfo, 0);
		if (res != 0)
		{
			printk(KERN_INFO DRIVER_PREFIX
				" %s: Device %u: Couldn't open framebuffer(%d)\n", __FUNCTION__, uiFBDevID, res);

			goto ErrorModPut;
		}
	}

	psDevInfo->psLINFBInfo = psLINFBInfo;

	ulLCM = LCM(psLINFBInfo->fix.line_length, OMAPLFB_PAGE_SIZE);

	DEBUG_PRINTK((KERN_INFO DRIVER_PREFIX
			": Device %u: Framebuffer physical address: 0x%lx\n",
			psDevInfo->uiFBDevID, psLINFBInfo->fix.smem_start));
	DEBUG_PRINTK((KERN_INFO DRIVER_PREFIX
			": Device %u: Framebuffer virtual address: 0x%lx\n",
			psDevInfo->uiFBDevID, (unsigned long)psLINFBInfo->screen_base));
	DEBUG_PRINTK((KERN_INFO DRIVER_PREFIX
			": Device %u: Framebuffer size: %lu\n",
			psDevInfo->uiFBDevID, FBSize));
	DEBUG_PRINTK((KERN_INFO DRIVER_PREFIX
			": Device %u: Framebuffer virtual width: %u\n",
			psDevInfo->uiFBDevID, psLINFBInfo->var.xres_virtual));
	DEBUG_PRINTK((KERN_INFO DRIVER_PREFIX
			": Device %u: Framebuffer virtual height: %u\n",
			psDevInfo->uiFBDevID, psLINFBInfo->var.yres_virtual));
	DEBUG_PRINTK((KERN_INFO DRIVER_PREFIX
			": Device %u: Framebuffer width: %u\n",
			psDevInfo->uiFBDevID, psLINFBInfo->var.xres));
	DEBUG_PRINTK((KERN_INFO DRIVER_PREFIX
			": Device %u: Framebuffer height: %u\n",
			psDevInfo->uiFBDevID, psLINFBInfo->var.yres));
	DEBUG_PRINTK((KERN_INFO DRIVER_PREFIX
			": Device %u: Framebuffer stride: %u\n",
			psDevInfo->uiFBDevID, psLINFBInfo->fix.line_length));
	DEBUG_PRINTK((KERN_INFO DRIVER_PREFIX
			": Device %u: LCM of stride and page size: %lu\n",
			psDevInfo->uiFBDevID, ulLCM));

	
	OMAPLFBPrintInfo(psDevInfo);

	/* hijack LINFB */
#if defined(CONFIG_ION_OMAP)
	if (cpu_is_omap446x())
	{
		/* for some reason we need at least 3 buffers in the swap chain */
		int n = FBSize / RoundUpToMultiple(psLINFBInfo->fix.line_length * psLINFBInfo->var.yres_virtual, ulLCM);
		int res;
		int i, x, y, w;
		ion_phys_addr_t phys;
		size_t size;
		struct tiler_view_t view;

		struct omap_ion_tiler_alloc_data sAllocData = {
			/* TILER will align width to 128-bytes */
			/* however, SGX must have full page width */
			.w = ALIGN(psLINFBInfo->var.xres_virtual, PAGE_SIZE / (psLINFBInfo->var.bits_per_pixel / 8)),
			.h = psLINFBInfo->var.yres_virtual,
			.fmt = psLINFBInfo->var.bits_per_pixel == 16 ? TILER_PIXEL_FMT_16BIT : TILER_PIXEL_FMT_32BIT,
			.flags = 0,
		};

		printk(KERN_DEBUG DRIVER_PREFIX
			" %s: Device %u: Requesting %d TILER 2D framebuffers\n", __FUNCTION__, uiFBDevID, n);
		/* HACK: limit to MAX 3 FBs to save TILER container space */
		if (n > 3)
			n = 3;
		sAllocData.w *= n;

		psPVRFBInfo->uiBytesPerPixel = psLINFBInfo->var.bits_per_pixel >> 3;
		psPVRFBInfo->bIs2D = OMAPLFB_TRUE;

		res = omap_ion_nonsecure_tiler_alloc(gpsIONClient, &sAllocData);
		if (res < 0)
		{
			res = omap_ion_tiler_alloc(gpsIONClient, &sAllocData);
		}
		psPVRFBInfo->psIONHandle = sAllocData.handle;
		if (res < 0)
		{
			printk(KERN_ERR DRIVER_PREFIX
				" %s: Device %u: Could not allocate 2D framebuffer(%d)\n", __FUNCTION__, uiFBDevID, res);
			goto ErrorModPut;
		}

		ion_phys(gpsIONClient, sAllocData.handle, &phys, &size);

		psPVRFBInfo->sSysAddr.uiAddr = phys;
		psPVRFBInfo->sCPUVAddr = 0;

		psPVRFBInfo->ulWidth = psLINFBInfo->var.xres_virtual;
		psPVRFBInfo->ulHeight = psLINFBInfo->var.yres_virtual;
		psPVRFBInfo->ulByteStride = PAGE_ALIGN(psPVRFBInfo->ulWidth * psPVRFBInfo->uiBytesPerPixel);
		w = psPVRFBInfo->ulByteStride >> PAGE_SHIFT;

		/* this is an "effective" FB size to get correct number of buffers */
		psPVRFBInfo->ulFBSize = n * RoundUpToMultiple(sAllocData.h * psPVRFBInfo->ulByteStride, ulLCM);
		psPVRFBInfo->psPageList = kzalloc(w * n * psPVRFBInfo->ulHeight * sizeof(*psPVRFBInfo->psPageList), GFP_KERNEL);
		if (!psPVRFBInfo->psPageList)
		{
			printk(KERN_WARNING DRIVER_PREFIX ": %s: Device %u: Could not allocate page list\n", __FUNCTION__, psDevInfo->uiFBDevID);
			ion_free(gpsIONClient, sAllocData.handle);
			goto ErrorModPut;
		}

		tilview_create(&view, phys, psDevInfo->sFBInfo.ulWidth, psDevInfo->sFBInfo.ulHeight);
		for(i=0; i<n; i++)
		{
			for(y=0; y<psDevInfo->sFBInfo.ulHeight; y++)
			{
				for(x=0; x<w; x++)
				{
					psPVRFBInfo->psPageList[i * psDevInfo->sFBInfo.ulHeight * w + y * w + x].uiAddr =
						phys + view.v_inc * y + ((x + i * w) << PAGE_SHIFT);
				}
			}
		}
	}
	else
#endif
	{
		psPVRFBInfo->sSysAddr.uiAddr = psLINFBInfo->fix.smem_start;
		psPVRFBInfo->sCPUVAddr = psLINFBInfo->screen_base;

		psPVRFBInfo->ulWidth = psLINFBInfo->var.xres_virtual;
		psPVRFBInfo->ulHeight = psLINFBInfo->var.yres_virtual;
		psPVRFBInfo->ulByteStride =  psLINFBInfo->fix.line_length;
		psPVRFBInfo->ulFBSize = FBSize;
		psPVRFBInfo->bIs2D = OMAPLFB_FALSE;
		psPVRFBInfo->psPageList = IMG_NULL;
		DEBUG_PRINTK((KERN_INFO DRIVER_PREFIX
				"Tiler: %s: VRAM address: FB addr[dev id:%u] = %0x\n",
				__FUNCTION__, psDevInfo->uiFBDevID, psPVRFBInfo->sSysAddr.uiAddr));
	}
	psPVRFBInfo->ulBufferSize = psPVRFBInfo->ulHeight * psPVRFBInfo->ulByteStride;
	
	psPVRFBInfo->ulRoundedBufferSize = RoundUpToMultiple(psPVRFBInfo->ulBufferSize, ulLCM);

	if(psLINFBInfo->var.bits_per_pixel == 16)
	{
		if((psLINFBInfo->var.red.length == 5) &&
			(psLINFBInfo->var.green.length == 6) && 
			(psLINFBInfo->var.blue.length == 5) && 
			(psLINFBInfo->var.red.offset == 11) &&
			(psLINFBInfo->var.green.offset == 5) && 
			(psLINFBInfo->var.blue.offset == 0) && 
			(psLINFBInfo->var.red.msb_right == 0))
		{
			psPVRFBInfo->ePixelFormat = PVRSRV_PIXEL_FORMAT_RGB565;
		}
		else
		{
			printk(KERN_INFO DRIVER_PREFIX ": %s: Device %u: Unknown FB format\n", __FUNCTION__, uiFBDevID);
		}
	}
	else if(psLINFBInfo->var.bits_per_pixel == 32)
	{
		if((psLINFBInfo->var.red.length == 8) &&
			(psLINFBInfo->var.green.length == 8) && 
			(psLINFBInfo->var.blue.length == 8) && 
			(psLINFBInfo->var.red.offset == 16) &&
			(psLINFBInfo->var.green.offset == 8) && 
			(psLINFBInfo->var.blue.offset == 0) && 
			(psLINFBInfo->var.red.msb_right == 0))
		{
			psPVRFBInfo->ePixelFormat = PVRSRV_PIXEL_FORMAT_ARGB8888;
		}
		else
		{
			printk(KERN_INFO DRIVER_PREFIX ": %s: Device %u: Unknown FB format\n", __FUNCTION__, uiFBDevID);
		}
	}	
	else
	{
		printk(KERN_INFO DRIVER_PREFIX ": %s: Device %u: Unknown FB format\n", __FUNCTION__, uiFBDevID);
	}

	psDevInfo->sFBInfo.ulPhysicalWidthmm =
		((int)psLINFBInfo->var.width  > 0) ? psLINFBInfo->var.width  : 90;

	psDevInfo->sFBInfo.ulPhysicalHeightmm =
		((int)psLINFBInfo->var.height > 0) ? psLINFBInfo->var.height : 54;

	
	psDevInfo->sFBInfo.sSysAddr.uiAddr = psPVRFBInfo->sSysAddr.uiAddr;
	psDevInfo->sFBInfo.sCPUVAddr = psPVRFBInfo->sCPUVAddr;

#if defined(CONFIG_OMAPLFB_CLONE_VRAM_TO_TILER)
	/* Init DMA channel */
	if (psDevInfo->uiFBDevID == 0) {
		if (OMAPLFBMotDmaInit() != 0) {
			printk(KERN_ERR DRIVER_PREFIX ": %s: Device %u: DMA Init failed; HDMI mirroring fails\n",
				__FUNCTION__, uiFBDevID);
		}
	}
#endif /* defined(CONFIG_OMAPLFB_CLONE_VRAM_TO_TILER) */

	eError = OMAPLFB_OK;
	goto ErrorRelSem;

ErrorModPut:
	module_put(psLINFBOwner);
ErrorRelSem:
	OMAPLFB_CONSOLE_UNLOCK();

	return eError;
}

static void OMAPLFBDeInitFBDev(OMAPLFB_DEVINFO *psDevInfo)
{
	struct fb_info *psLINFBInfo = psDevInfo->psLINFBInfo;
	OMAPLFB_FBINFO *psPVRFBInfo = &psDevInfo->sFBInfo;
	struct module *psLINFBOwner;

	OMAPLFB_CONSOLE_LOCK();

	psLINFBOwner = psLINFBInfo->fbops->owner;

	kfree(psPVRFBInfo->psPageList);
#if defined(CONFIG_ION_OMAP)
	if (psPVRFBInfo->psIONHandle)
	{
		ion_free(gpsIONClient, psPVRFBInfo->psIONHandle);
	}
#endif
	if (psLINFBInfo->fbops->fb_release != NULL) 
	{
		(void) psLINFBInfo->fbops->fb_release(psLINFBInfo, 0);
	}

#if defined(CONFIG_OMAPLFB_CLONE_VRAM_TO_TILER)
	if (psDevInfo->uiFBDevID == 0) {
		OMAPLFBMotDmaTerm();
		OMAPLFBMotDeallocTiler2d();
	}
#endif /* defined(CONFIG_OMAPLFB_CLONE_VRAM_TO_TILER) */

	module_put(psLINFBOwner);

	OMAPLFB_CONSOLE_UNLOCK();
}

static OMAPLFB_DEVINFO *OMAPLFBInitDev(unsigned uiFBDevID)
{
	PFN_CMD_PROC	 	pfnCmdProcList[OMAPLFB_COMMAND_COUNT];
	IMG_UINT32		aui32SyncCountList[OMAPLFB_COMMAND_COUNT][2];
	OMAPLFB_DEVINFO		*psDevInfo = NULL;

	
	psDevInfo = (OMAPLFB_DEVINFO *)OMAPLFBAllocKernelMem(sizeof(OMAPLFB_DEVINFO));

	if(psDevInfo == NULL)
	{
		printk(KERN_ERR DRIVER_PREFIX
			": %s: Device %u: Couldn't allocate device information structure\n", __FUNCTION__, uiFBDevID);

		goto ErrorExit;
	}

	
	memset(psDevInfo, 0, sizeof(OMAPLFB_DEVINFO));

	psDevInfo->uiFBDevID = uiFBDevID;

	
	if(!(*gpfnGetPVRJTable)(&psDevInfo->sPVRJTable))
	{
		goto ErrorFreeDevInfo;
	}

	
	if(OMAPLFBInitFBDev(psDevInfo) != OMAPLFB_OK)
	{
		
		goto ErrorFreeDevInfo;
	}

	psDevInfo->sDisplayInfo.ui32MaxSwapChainBuffers = (IMG_UINT32)(psDevInfo->sFBInfo.ulFBSize / psDevInfo->sFBInfo.ulRoundedBufferSize);
	if (psDevInfo->sDisplayInfo.ui32MaxSwapChainBuffers != 0)
	{
		psDevInfo->sDisplayInfo.ui32MaxSwapChains = 1;
		psDevInfo->sDisplayInfo.ui32MaxSwapInterval = 1;
	}

	psDevInfo->sDisplayInfo.ui32PhysicalWidthmm = psDevInfo->sFBInfo.ulPhysicalWidthmm;
	psDevInfo->sDisplayInfo.ui32PhysicalHeightmm = psDevInfo->sFBInfo.ulPhysicalHeightmm;

	strncpy(psDevInfo->sDisplayInfo.szDisplayName, DISPLAY_DEVICE_NAME, MAX_DISPLAY_NAME_SIZE);

	psDevInfo->sDisplayFormat.pixelformat = psDevInfo->sFBInfo.ePixelFormat;
	psDevInfo->sDisplayDim.ui32Width      = (IMG_UINT32)psDevInfo->sFBInfo.ulWidth;
	psDevInfo->sDisplayDim.ui32Height     = (IMG_UINT32)psDevInfo->sFBInfo.ulHeight;
	psDevInfo->sDisplayDim.ui32ByteStride = (IMG_UINT32)psDevInfo->sFBInfo.ulByteStride;

	DEBUG_PRINTK((KERN_INFO DRIVER_PREFIX
		": Device %u: Maximum number of swap chain buffers: %u\n",
		psDevInfo->uiFBDevID, psDevInfo->sDisplayInfo.ui32MaxSwapChainBuffers));

	
	psDevInfo->sSystemBuffer.sSysAddr = psDevInfo->sFBInfo.sSysAddr;
	psDevInfo->sSystemBuffer.sCPUVAddr = psDevInfo->sFBInfo.sCPUVAddr;
	psDevInfo->sSystemBuffer.psDevInfo = psDevInfo;

	OMAPLFBInitBufferForSwap(&psDevInfo->sSystemBuffer);

	

	psDevInfo->sDCJTable.ui32TableSize = sizeof(PVRSRV_DC_SRV2DISP_KMJTABLE);
	psDevInfo->sDCJTable.pfnOpenDCDevice = OpenDCDevice;
	psDevInfo->sDCJTable.pfnCloseDCDevice = CloseDCDevice;
	psDevInfo->sDCJTable.pfnEnumDCFormats = EnumDCFormats;
	psDevInfo->sDCJTable.pfnEnumDCDims = EnumDCDims;
	psDevInfo->sDCJTable.pfnGetDCSystemBuffer = GetDCSystemBuffer;
	psDevInfo->sDCJTable.pfnGetDCInfo = GetDCInfo;
	psDevInfo->sDCJTable.pfnGetBufferAddr = GetDCBufferAddr;
	psDevInfo->sDCJTable.pfnCreateDCSwapChain = CreateDCSwapChain;
	psDevInfo->sDCJTable.pfnDestroyDCSwapChain = DestroyDCSwapChain;
	psDevInfo->sDCJTable.pfnSetDCDstRect = SetDCDstRect;
	psDevInfo->sDCJTable.pfnSetDCSrcRect = SetDCSrcRect;
	psDevInfo->sDCJTable.pfnSetDCDstColourKey = SetDCDstColourKey;
	psDevInfo->sDCJTable.pfnSetDCSrcColourKey = SetDCSrcColourKey;
	psDevInfo->sDCJTable.pfnGetDCBuffers = GetDCBuffers;
	psDevInfo->sDCJTable.pfnSwapToDCBuffer = SwapToDCBuffer;
	psDevInfo->sDCJTable.pfnSwapToDCSystem = SwapToDCSystem;
	psDevInfo->sDCJTable.pfnSetDCState = SetDCState;

	
	if(psDevInfo->sPVRJTable.pfnPVRSRVRegisterDCDevice(
		&psDevInfo->sDCJTable,
		&psDevInfo->uiPVRDevID) != PVRSRV_OK)
	{
		printk(KERN_ERR DRIVER_PREFIX
			": %s: Device %u: PVR Services device registration failed\n", __FUNCTION__, uiFBDevID);

		goto ErrorDeInitFBDev;
	}
	DEBUG_PRINTK((KERN_INFO DRIVER_PREFIX
		": Device %u: PVR Device ID: %u\n",
		psDevInfo->uiFBDevID, psDevInfo->uiPVRDevID));
	
	
	pfnCmdProcList[DC_FLIP_COMMAND] = ProcessFlip;

	
	aui32SyncCountList[DC_FLIP_COMMAND][0] = 0; 
	aui32SyncCountList[DC_FLIP_COMMAND][1] = 10; 

	



	if (psDevInfo->sPVRJTable.pfnPVRSRVRegisterCmdProcList(psDevInfo->uiPVRDevID,
															&pfnCmdProcList[0],
															aui32SyncCountList,
															OMAPLFB_COMMAND_COUNT) != PVRSRV_OK)
	{
		printk(KERN_ERR DRIVER_PREFIX
			": %s: Device %u: Couldn't register command processing functions with PVR Services\n", __FUNCTION__, uiFBDevID);
		goto ErrorUnregisterDevice;
	}

	OMAPLFBCreateSwapChainLockInit(psDevInfo);

	OMAPLFBAtomicBoolInit(&psDevInfo->sBlanked, OMAPLFB_FALSE);
	OMAPLFBAtomicIntInit(&psDevInfo->sBlankEvents, 0);
	OMAPLFBAtomicBoolInit(&psDevInfo->sFlushCommands, OMAPLFB_FALSE);
#if defined(CONFIG_HAS_EARLYSUSPEND)
	OMAPLFBAtomicBoolInit(&psDevInfo->sEarlySuspendFlag, OMAPLFB_FALSE);
#endif
#if defined(SUPPORT_DRI_DRM)
	OMAPLFBAtomicBoolInit(&psDevInfo->sLeaveVT, OMAPLFB_FALSE);
#endif
	return psDevInfo;

ErrorUnregisterDevice:
	(void)psDevInfo->sPVRJTable.pfnPVRSRVRemoveDCDevice(psDevInfo->uiPVRDevID);
ErrorDeInitFBDev:
	OMAPLFBDeInitFBDev(psDevInfo);
ErrorFreeDevInfo:
	OMAPLFBFreeKernelMem(psDevInfo);
ErrorExit:
	return NULL;
}

OMAPLFB_ERROR OMAPLFBInit(struct omaplfb_device *omaplfb_dev)

{
	unsigned uiMaxFBDevIDPlusOne = OMAPLFBMaxFBDevIDPlusOne();
	unsigned i;
	unsigned uiDevicesFound = 0;

	if(OMAPLFBGetLibFuncAddr ("PVRGetDisplayClassJTable", &gpfnGetPVRJTable) != OMAPLFB_OK)
	{
		return OMAPLFB_ERROR_INIT_FAILURE;
	}

#if defined(CONFIG_GC_CORE)
	/* Get the GC2D Bltsville implementation */
	bv_gc2d_getentry(&bv_gc2d_entry);
	bv_gc2d_is_present = bv_gc2d_entry.bv_map ? 1 : 0;
#else
	bv_gc2d_is_present = 0;
#endif

	if (bv_gc2d_is_present)
		printk(KERN_INFO DRIVER_PREFIX "%s: Blitsville gc2d "
			"present, blits enabled\n", __func__);
	else
		printk(KERN_INFO DRIVER_PREFIX "%s: Blitsville gc2d "
			"not present, blits disabled\n", __func__);
	
	for(i = uiMaxFBDevIDPlusOne; i-- != 0;)
	{
		OMAPLFB_DEVINFO *psDevInfo = OMAPLFBInitDev(i);
		if (omaplfb_dev)
		{
			omaplfb_dev->display_info_list[i] = NULL;
		}
		if (psDevInfo != NULL)
		{
			
			OMAPLFBSetDevInfoPtr(psDevInfo->uiFBDevID, psDevInfo);
			uiDevicesFound++;
			if (omaplfb_dev)
			{
				omaplfb_dev->display_info_list[i] =
					psDevInfo;
			}
		}
	}

	return (uiDevicesFound != 0) ? OMAPLFB_OK : OMAPLFB_ERROR_INIT_FAILURE;
}

static OMAPLFB_BOOL OMAPLFBDeInitDev(OMAPLFB_DEVINFO *psDevInfo)
{
	PVRSRV_DC_DISP2SRV_KMJTABLE *psPVRJTable = &psDevInfo->sPVRJTable;

	OMAPLFBCreateSwapChainLockDeInit(psDevInfo);

	OMAPLFBAtomicBoolDeInit(&psDevInfo->sBlanked);
	OMAPLFBAtomicIntDeInit(&psDevInfo->sBlankEvents);
	OMAPLFBAtomicBoolDeInit(&psDevInfo->sFlushCommands);
#if defined(CONFIG_HAS_EARLYSUSPEND)
	OMAPLFBAtomicBoolDeInit(&psDevInfo->sEarlySuspendFlag);
#endif
#if defined(SUPPORT_DRI_DRM)
	OMAPLFBAtomicBoolDeInit(&psDevInfo->sLeaveVT);
#endif
	psPVRJTable = &psDevInfo->sPVRJTable;

	if (psPVRJTable->pfnPVRSRVRemoveCmdProcList (psDevInfo->uiPVRDevID, OMAPLFB_COMMAND_COUNT) != PVRSRV_OK)
	{
		printk(KERN_ERR DRIVER_PREFIX
			": %s: Device %u: PVR Device %u: Couldn't unregister command processing functions\n", __FUNCTION__, psDevInfo->uiFBDevID, psDevInfo->uiPVRDevID);
		return OMAPLFB_FALSE;
	}

	
	if (psPVRJTable->pfnPVRSRVRemoveDCDevice(psDevInfo->uiPVRDevID) != PVRSRV_OK)
	{
		printk(KERN_ERR DRIVER_PREFIX
			": %s: Device %u: PVR Device %u: Couldn't remove device from PVR Services\n", __FUNCTION__, psDevInfo->uiFBDevID, psDevInfo->uiPVRDevID);
		return OMAPLFB_FALSE;
	}
	
	OMAPLFBDeInitFBDev(psDevInfo);

	OMAPLFBSetDevInfoPtr(psDevInfo->uiFBDevID, NULL);

	
	OMAPLFBFreeKernelMem(psDevInfo);

	return OMAPLFB_TRUE;
}

OMAPLFB_ERROR OMAPLFBDeInit(void)
{
	unsigned uiMaxFBDevIDPlusOne = OMAPLFBMaxFBDevIDPlusOne();
	unsigned i;
	OMAPLFB_BOOL bError = OMAPLFB_FALSE;

	for(i = 0; i < uiMaxFBDevIDPlusOne; i++)
	{
		OMAPLFB_DEVINFO *psDevInfo = OMAPLFBGetDevInfoPtr(i);

		if (psDevInfo != NULL)
		{
			bError |= !OMAPLFBDeInitDev(psDevInfo);
		}
	}

	return (bError) ? OMAPLFB_ERROR_INIT_FAILURE : OMAPLFB_OK;
}

/* MOT code for Mirroring */
#if defined(CONFIG_OMAPLFB_CLONE_VRAM_TO_TILER)
int OMAPLFBMotDmaInit(void)
{
	int rc = 0;

	dmadata.id = OMAP_DMA_NO_DEVICE;
	dmadata.ch = -1;
	rc = omap_request_dma(dmadata.id, "MIRROR DMA", OMAPLFBMotDmaCallback,
				(void *) &dmadata, &dmadata.ch);
	if (rc != 0) {
		printk(KERN_ERR DRIVER_PREFIX "%s: OMAPLFBMotDmaInit: DMA Alloc Failed\n", __FUNCTION__);
		rc = -1;
		goto failed_dma;
	}
	init_waitqueue_head(&dmadata.wait);

failed_dma:
	return rc;
}

void OMAPLFBMotDmaTerm(void)
{
	omap_free_dma(dmadata.ch);
}

int OMAPLFBMotDmaConfig(int w, int h, int bytespp, int src_stride, int dst_stride)
{
	int rc = 0;
	DEBUG_PRINTK((KERN_INFO DRIVER_PREFIX
			"%s : Tiler: w = %i, h = %i, bytespp = %i, src stride = %i, dest stride = %i\n",
			__FUNCTION__, w, h, bytespp, src_stride, dst_stride));

	dmadata.en     = (w * bytespp) / 4; /* 32 bit ES */
	dmadata.fn     = h;
	dmadata.src_fi = src_stride - (dmadata.en * 4) + 1;
	dmadata.dst_fi = dst_stride - (dmadata.en * 4) + 1;

	return rc;
}

int OMAPLFBMotDmaPerform(unsigned long src, unsigned long dst)
{
	DEBUG_PRINTK((KERN_INFO DRIVER_PREFIX
			" Tiler:  %s:  %08x/%08x/%d/%d/%d/%d\n",
			__FUNCTION__, (uint32_t)src, (uint32_t)dst, dmadata.en, dmadata.fn, dmadata.src_fi, dmadata.dst_fi));

	omap_set_dma_transfer_params(dmadata.ch, OMAP_DMA_DATA_TYPE_S32,
			dmadata.en, dmadata.fn, OMAP_DMA_SYNC_ELEMENT,
			dmadata.id, 0x0);
	omap_set_dma_src_params(dmadata.ch, 0, OMAP_DMA_AMODE_DOUBLE_IDX,
			src, 1, dmadata.src_fi);
	omap_set_dma_src_data_pack(dmadata.ch, 1);
	omap_set_dma_src_burst_mode(dmadata.ch, OMAP_DMA_DATA_BURST_16);
	omap_set_dma_dest_params(dmadata.ch, 0, OMAP_DMA_AMODE_DOUBLE_IDX,
			dst, 1, dmadata.dst_fi);
	omap_set_dma_dest_data_pack(dmadata.ch, 1);
	omap_set_dma_dest_burst_mode(dmadata.ch, OMAP_DMA_DATA_BURST_16);
	omap_dma_set_prio_lch(dmadata.ch, 1, 1);
	omap_dma_set_global_params(DMA_DEFAULT_ARB_RATE, 0xFF, 0);

	dmadata.complete = false;
	omap_start_dma(dmadata.ch);
	wait_event_interruptible_timeout(dmadata.wait,
					dmadata.complete, DMA_TX_TIMEOUT);

	if (!dmadata.complete) {
		printk(KERN_WARNING DRIVER_PREFIX "%s : OMAPLFBMotDmaPerform: DMA timeout\n", __FUNCTION__);
		omap_stop_dma(dmadata.ch);
		return -EIO;
	}

	return 0;
}

/* This functions wakes up the application once the DMA transfer to
 * Tiler space is completed.
 */
void OMAPLFBMotDmaCallback(int lch, u16 ch_status, void *pdata)
{
	struct OMAPLFBMotDmaData *data;
	DEBUG_PRINTK((KERN_INFO DRIVER_PREFIX "Tiler:  %s: dma ch status = %u\n", __FUNCTION__, ch_status));
	data = (struct OMAPLFBMotDmaData *) pdata;

	data->complete = true;
	wake_up_interruptible(&data->wait);
}

/*
 * This function dynamically allocates Tiler2d memory
 * for the size corresponds to source FB0 height,
 * format and stride for DMA copy.
 */
OMAPLFB_BOOL OMAPLFBMotAllocTiler2d(void)
{
	OMAPLFB_BOOL rc = false;

#if defined(CONFIG_ION_OMAP)
	int n = 2; /* number of buffers */
	int res;
	int w;
	ion_phys_addr_t phys;
	size_t size;
	struct tiler_view_t view;
	unsigned long ulWidth, ulHeight, ulByteStride, ulFBSize;
	IMG_UINT32 uiBytesPerPixel;
	OMAPLFB_DEVINFO *psDevInfo;
	OMAPLFB_FBINFO *psPVRFBInfo;
	struct omap_ion_tiler_alloc_data sAllocData;
	unsigned int buffer_offset;

	/* Get DeviceInfo corresponds to fb0 */
	struct fb_info *psLINFBInfo = NULL;

	sDestStride = 0;
	sTilerMemAddr[0].uiAddr = 0;
	sTilerMemAddr[1].uiAddr = 0;

	psDevInfo = OMAPLFBGetDevInfoPtr(0);
	if (psDevInfo == NULL) {
		printk(KERN_ERR DRIVER_PREFIX
			" %s: Tiler: Device: 0: not available", __FUNCTION__);
		return rc;
	}

	psLINFBInfo = psDevInfo->psLINFBInfo;
	if (psLINFBInfo == NULL) {
		printk(KERN_ERR DRIVER_PREFIX
			" %s: Tiler: FBINFO for fb0: not available", __FUNCTION__);
		return rc;
	}

	psPVRFBInfo = &psDevInfo->sFBInfo;
	if (psPVRFBInfo == NULL) {
		printk(KERN_ERR DRIVER_PREFIX
			" %s: Tiler: PVRFBINFO for fb0: not available", __FUNCTION__);
		return rc;
	}

	/* TILER will align width to 1024 bytes */
	/* however, SGX must have full page width */
	sAllocData.w = ALIGN(psLINFBInfo->var.xres_virtual, PAGE_SIZE / (psLINFBInfo->var.bits_per_pixel / 8));
	sAllocData.h = psLINFBInfo->var.yres_virtual;
	sAllocData.fmt = psLINFBInfo->var.bits_per_pixel == 16 ? TILER_PIXEL_FMT_16BIT : TILER_PIXEL_FMT_32BIT;
	sAllocData.flags = 0;

	DEBUG_PRINTK((KERN_DEBUG DRIVER_PREFIX
		" %s: Tiler: Request allocation of Tiler2d buffers\n", __FUNCTION__));

	/* limit to MAX 2 buffers to save TILER container space */
	sAllocData.w *= n;

	uiBytesPerPixel = psLINFBInfo->var.bits_per_pixel >> 3;

	res = omap_ion_nonsecure_tiler_alloc(gpsIONClient, &sAllocData);
	if (res < 0) {
		res = omap_ion_tiler_alloc(gpsIONClient, &sAllocData);
	}
	sTilerAllocData.handle = sAllocData.handle;

	if (res < 0) {
		printk(KERN_ERR DRIVER_PREFIX
			":%s: Tiler: Could not allocate 2D meomry\n", __FUNCTION__);
		sTilerMemAddr[0].uiAddr = 0;
		sTilerMemAddr[1].uiAddr = 0;
		sDestStride = 0;
		return rc;
	}

	ion_phys(gpsIONClient, sAllocData.handle, &phys, &size);

	sTilerMemAddr[0].uiAddr = phys;
	buffer_offset = ALIGN(psLINFBInfo->var.xres_virtual * (psLINFBInfo->var.bits_per_pixel >> 3), PAGE_SIZE);
	sTilerMemAddr[1].uiAddr = phys + buffer_offset;
	ulWidth = psLINFBInfo->var.xres_virtual;
	ulHeight = psLINFBInfo->var.yres_virtual;
	ulByteStride = PAGE_ALIGN(psPVRFBInfo->ulWidth * psPVRFBInfo->uiBytesPerPixel);
	w = psPVRFBInfo->ulByteStride >> PAGE_SHIFT;
	sDestStride = 32768; /*FIXME: api needed */

	DEBUG_PRINTK((KERN_INFO DRIVER_PREFIX
		": %s: Tiler start address buf1 = %x, buf2 = %x, size = %u, stride = %u\n",
		__FUNCTION__, sTilerMemAddr[0].uiAddr, sTilerMemAddr[1].uiAddr, size, sDestStride));


	/* this is an "effective" buffer size to get correct number of buffers */
	ulFBSize = sAllocData.h * n * psPVRFBInfo->ulByteStride;
	sPageList = kzalloc(w * n * psPVRFBInfo->ulHeight * sizeof(*psPVRFBInfo->psPageList), GFP_KERNEL);
	if (!sPageList) {
		printk(KERN_WARNING DRIVER_PREFIX ": %s: Tiler: Could not allocate page list\n", __FUNCTION__);
		ion_free(gpsIONClient, sAllocData.handle);
		sTilerMemAddr[0].uiAddr = 0;
		sTilerMemAddr[1].uiAddr = 0;
		sDestStride = 0;
		return rc;
	}

	tilview_create(&view, phys, psDevInfo->sFBInfo.ulWidth, psDevInfo->sFBInfo.ulHeight);
	return true;
#endif
	printk(KERN_ERR DRIVER_PREFIX
	" %s: Tiler: ION mem config not enabled\n", __FUNCTION__);
	return rc;
}

OMAPLFB_BOOL OMAPLFBMotDeallocTiler2d(void)
{
	OMAPLFB_BOOL rc = false;
	if (sPageList) {
		DEBUG_PRINTK((KERN_INFO DRIVER_PREFIX ": %s: Tiler: freeing page list\n", __FUNCTION__));
		ion_free(gpsIONClient, sTilerAllocData.handle);
		kfree(sPageList);
		sPageList = 0;
		sTilerMemAddr[0].uiAddr = 0;
		sTilerMemAddr[1].uiAddr = 0;
		return true;
	} else {
		printk(KERN_WARNING DRIVER_PREFIX ": %s: Tiler: no page list available to free\n", __FUNCTION__);
		return rc;
	}
}

IMG_UINT32 fb_to_phys(const OMAPLFB_DEVINFO *psDevInfo, IMG_UINT32 addr)
{
	IMG_UINT32 paddr = 0;
	IMG_UINT32 fb_vaddr_begin = (IMG_UINT32)psDevInfo->sFBInfo.sCPUVAddr;
	IMG_UINT32 fb_vaddr_end = fb_vaddr_begin + psDevInfo->psLINFBInfo->fix.smem_len;
	IMG_UINT32 fb_paddr_begin = (IMG_UINT32)psDevInfo->sFBInfo.sSysAddr.uiAddr;
	IMG_UINT32 fb_paddr_end = fb_paddr_begin + psDevInfo->sFBInfo.ulFBSize;
	if (fb_vaddr_begin <= addr && addr < fb_vaddr_end)
		paddr = addr - fb_vaddr_begin + (IMG_UINT32)psDevInfo->sFBInfo.sSysAddr.uiAddr;
	else if (fb_paddr_begin <= addr && addr < fb_paddr_end)
		paddr = addr;
	return paddr;
}
#endif /* CONFIG_OMAPLFB_CLONE_VRAM_TO_TILER */

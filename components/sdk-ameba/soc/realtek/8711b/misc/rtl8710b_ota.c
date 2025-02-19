/**
  ******************************************************************************
  * @file    rtl8711b_ota.c
  * @author
  * @version V1.0.0
  * @date    2016-12-19
  * @brief   This file contains the code which is used to update firmware over the air(OTA) in local area network
  *
  *  @verbatim
  *      
  *          ===================================================================
  *                                 How to use the local OTA upgrade code
  *          ===================================================================
  *          1. Firstly, read the OTA related documents to know about how the code to realize updating firmware
  *              over the air(OTA), and some protocol included in it.
  *
  *          2. Read the source code and APIs in this file.
  *
  *          3. Porting this code in this file to the specified cloud platform according to the upgrade flow and parameters of
  *              the specified cloud service providers
  *
  *          4. Test the code after porting on the specified cloud platform.
  *
  *          5. Generate the release verision that will run in AmebaZ. 
  *
  *          ===================================================================
  *                                 the basic flow of the local ota upgrade code
  *          ===================================================================
  *          (1) AmebaZ connects to server
  *
  *          (2) Receive newer firmware file header from server
  *
  *          (3) Parse firmware file header and get the target OTA image header
  *
  *          (4) Erase flash space for new firmware
  *
  *          (5) Download new firmware from server and write it to flash
  *
  *          (6) Verify checksum and update signature
  *
  *	      (7) OTA upgrade successfully, restart device
  *  @endverbatim
  *
  ******************************************************************************
  * @attention
  *
  * This module is a confidential and proprietary property of RealTek and
  * possession or use of this module requires written permission of RealTek.
  *
  * Copyright(c) 2016, Realtek Semiconductor Corporation. All rights reserved.
  ****************************************************************************** 
  */

#include <stdlib.h>
#include <string.h>
#include <FreeRTOS.h>
#include <task.h>
#include <lwip/sockets.h>
#include <sys.h>

#include "flash_api.h"
#include <device_lock.h>
#include "rtl8710b_ota.h"
#include "lwip/netdb.h"

#define OTA2_DEFAULT_ADDR   (0x08080000)

#if CONFIG_CUSTOM_SIGNATURE
/* ---------------------------------------------------
  * Customized Signature
  * This signature can be used to verify the correctness of the image
  * It will be located in fixed location in application image:
  * (after 32B IMG2 header )
  * ---------------------------------------------------*/
IMAGE2_CUSTOM_SIGNATURE
const unsigned char cus_sig_demo[32] = "Customer Signature-modelxxx";
#endif

#define STACK_SIZE		1024
#define TASK_PRIORITY	tskIDLE_PRIORITY + 1
#define ETH_ALEN	6

#define SERVER_LOCAL	1
#define SERVER_CLOUD	2
#define UPDATE_DBG		1

#if (SERVER_TYPE == SERVER_LOCAL)
typedef struct
{
	uint32_t	ip_addr;
	uint16_t	port;
}update_cfg_local_t;
const update_file_img_id OtaImgId[2] = 
{
	{"OTA1"},
	{"OTA2"}
};

const u8 RdpImg[4] = "RDP";
#endif

#if (SERVER_TYPE == SERVER_CLOUD)
#define REPOSITORY_LEN	16
#define FILE_PATH_LEN	64
typedef struct
{
	uint8_t	 	repository[REPOSITORY_LEN];
	uint8_t		file_path[FILE_PATH_LEN];
}update_cfg_cloud_t;
#endif

sys_thread_t TaskOTA = NULL;
//---------------------------------------------------------------------
void* ota_update_malloc(unsigned int size)
{
	return pvPortMalloc(size);
}

//---------------------------------------------------------------------
void ota_update_free(void *buf)
{
	vPortFree(buf);
}

//---------------------------------------------------------------------
#if (SERVER_TYPE == SERVER_LOCAL)
/**
    reset CPU
  */
void ota_platform_reset(void)
{
	//wifi_off();

	/*  Set processor clock to default before system reset */
	//HAL_WRITE32(SYSTEM_CTRL_BASE, REG_SYS_CLK_CTRL1, 0x00000021);
	CPU_ClkSet(CLK_31_25M);
	osDelay(100);


	/* CPU reset: Cortex-M3 SCB->AIRCR*/
	NVIC_SystemReset();	
}

/**
    write OTA2 address to the first Dword in the system data space
    ota_addr: OTA2 address
  */
int ota_write_ota2_addr(uint32_t ota_addr)
{
	uint32_t data;
	/* Get upgraded image 2 addr from offset */
	data = HAL_READ32(SPI_FLASH_BASE, OFFSET_DATA);
	printf("\n\r[%s] data 0x%x ota_addr 0x%x", __FUNCTION__, data, ota_addr);

	device_mutex_lock(RT_DEV_LOCK_FLASH);
	/* erase old data if needed, write new data*/
	if ((data != 0xFFFFFFFF) && (data != ota_addr)) {
		FLASH_EreaseDwordsXIP(OFFSET_DATA, 1);
		FLASH_TxData12BXIP(OFFSET_DATA, 4, (u8*)&ota_addr);
	}
	device_mutex_unlock(RT_DEV_LOCK_FLASH);

	return 0;
}

/* get current image2 OTA index: OTA_INDEX_1 or OTA_INDEX_2 */
u32 ota_get_cur_index(void)
{
	u32 check_addr = (u32)cus_sig_demo;

	if (check_addr == OTA1_ADDR + IMAGE_HEADER_LEN)
		return OTA_INDEX_1;
	else
		return OTA_INDEX_2;
}

#if SINGLE_IMG_OTA_UPGRADE

/**
    receive file_info from server. This operation is patched for the compatibility with ameba1
    Recvbuf: point for receiving buffer
    len: length of file info
    socket: socket handle
  */
u32 recv_file_info_from_server(u8 * Recvbuf, u32 len, int socket)
{
	int read_bytes = 0;
	u32 TempLen;
	u8 * buf;
	
	/*read 4 Dwords from server, get image header number and header length*/
	buf = Recvbuf;
	TempLen = len;
	while(TempLen > 0) {
		read_bytes = read(socket, buf, TempLen);
		if(read_bytes < 0){
			printf("\n\r[%s] read socket failed\n", __FUNCTION__);
			goto error;
		}
		if(read_bytes == 0) {
			break;
		}
		TempLen -= read_bytes;
		buf += read_bytes;
	}

	return 1;
error:
	return 0;
}

/**
    receive OTA firmware file header from server
    Recvbuf: pointer to buffer for receiving OTA header of firmware file
    len: data length to be received from server
    pOtaTgtHdr: point to target image OTA  header
    socket: socket handle
  */
u32 recv_ota_file_hdr(u8 * Recvbuf, u32 * len, update_ota_target_hdr * pOtaTgtHdr, int socket)
{
	int read_bytes = 0;
	u32 TempLen;
	u8 * buf;
	update_file_hdr * pOtaFileHdr;
	update_file_img_hdr * pOtaFileImgHdr;
	
	/*read 4 Dwords from server, get image header number and header length*/
	buf = Recvbuf;
	TempLen = 16;
	while(TempLen > 0) {
		read_bytes = read(socket, buf, TempLen);
		if(read_bytes < 0){
			printf("\n\r[%s] read socket failed\n", __FUNCTION__);
			goto error;
		}
		if(read_bytes == 0) {
			break;
		}
		TempLen -= read_bytes;
		buf += read_bytes;
	}

	pOtaFileHdr = (update_file_hdr *)Recvbuf;
	pOtaFileImgHdr = (update_file_img_hdr *)(Recvbuf + 8);

	pOtaTgtHdr->FileHdr.FwVer = pOtaFileHdr->FwVer;
	pOtaTgtHdr->FileHdr.HdrNum = pOtaFileHdr->HdrNum;

	/*read the remaining Header info*/
	buf = Recvbuf + 16;
	TempLen =  (pOtaFileHdr->HdrNum * pOtaFileImgHdr->ImgHdrLen) - 8;
	while(TempLen > 0) {
		read_bytes = read(socket, buf, TempLen);
		if(read_bytes < 0){
			printf("\n\r[%s] read socket failed\n", __FUNCTION__);
			goto error;
		}
		if(read_bytes == 0) {
			break;
		}
		TempLen -= read_bytes;
		buf += read_bytes;
	}

	*len = (pOtaFileHdr->HdrNum * pOtaFileImgHdr->ImgHdrLen) + 8;

	return 1;
error:
	return 0;
}

/**
    parse firmware file header and get the desired OTA header
    buf: point to buffer for receiving OTA header of new firmware
    len: data length to be received from server
    pOtaTgtHdr:point to target image OTA  header
    ImgId: point to image identification strings
  */
u32 get_ota_tartget_header(u8* buf, u32 len, update_ota_target_hdr * pOtaTgtHdr, u8 * ImgId)
{
	update_file_img_hdr * ImgHdr;
	update_file_hdr * FileHdr;
	u8 * pTempAddr;
	u32 i;
	u32 ImageFg = 0;

	pOtaTgtHdr->RdpStatus = DISABLE;

	/*check if buf and len is valid or not*/
	if((len < (sizeof(update_file_img_hdr) + 8)) || (!buf)) {
		goto error;
	}

	FileHdr = (update_file_hdr *)buf;
	ImgHdr = (update_file_img_hdr *)(buf + 8);
	pTempAddr = buf + 8;

	if(len < (FileHdr->HdrNum * ImgHdr->ImgHdrLen + 8)) {
		goto error;
	}

	/*get the target OTA header from the new firmware file header*/
	for(i = 0; i < FileHdr->HdrNum; i++) {
		pTempAddr = buf + 8 + ImgHdr->ImgHdrLen * i;

		/*check OTA header*/
		if(strncmp(ImgId, pTempAddr, 4) == 0) {
			ImageFg = 1;
			_memcpy((u8*)(&pOtaTgtHdr->FileImgHdr), pTempAddr, sizeof(update_file_img_hdr));
			continue;
		}

		/*check RDP header*/
		if(strncmp(RdpImg, pTempAddr, 3) == 0) {
			DBG_8195A("there exists RDP image in firmware file\n");
			pOtaTgtHdr->RdpStatus = ENABLE;
			ImageFg = 1;
			_memcpy((u8*)(&pOtaTgtHdr->FileRdpHdr), pTempAddr, sizeof(update_file_img_hdr));
			continue;
		}
		
		if(i == FileHdr->HdrNum - 1) {
			if(ImageFg == 0) {
				printf("\n\r[%s] no matched image\n", __FUNCTION__);
				goto error;
			}
		}
	}

	return 1;
error:
	return 0;
}

/**
    erase the flash space for new firmware
    addr: new image address
    len: new image length
  */
void erase_ota_target_flash( u32 addr, u32 len)
{
	u32 NewImg2BlkSize;
	u32 i;
	flash_t * flash;
	/* Erase upgraded image 2 region */
	printf("\n\r[%s] NewImg2Len %d  ", __FUNCTION__, len);
	NewImg2BlkSize = ((len - 1)/4096) + 1;

	printf("\n\r[%s] NewImg2BlkSize %d  0x%8x", __FUNCTION__, NewImg2BlkSize, NewImg2BlkSize);
	device_mutex_lock(RT_DEV_LOCK_FLASH);
	for( i = 0; i < NewImg2BlkSize; i++)
		flash_erase_sector(flash, addr -SPI_FLASH_BASE + i * 4096);
	device_mutex_unlock(RT_DEV_LOCK_FLASH);
}

/**
    download new firmware from server and write it to flash
    addr: new image address
    socket: socket handle
    pOtaTgtHdr: point to target image OTA  header
    signature: point to signature strings
  */
s32 download_new_fw_from_server(u32 addr, int socket, update_ota_target_hdr * pOtaTgtHdr, u8 * signature)
{
	u8 * alloc;
	u8 * buf;
	s32 size = 0;
	int read_bytes;
	int read_bytes_buf;
	u32 TempLen;
	flash_t flash;
	u32 ImageCnt;
	update_dw_info DownloadInfo[2];
	/*initialize the variables used in downloading procedure*/
	u32 OtaFg = 0;
	u32 IncFg = 0;
	s32 RemainBytes;
	u32 SigCnt = 0;
	u32 TempCnt = 0;	
	u32 i;
	s32 OtaImgSize;

	/*acllocate buffer for downloading image from server*/
	alloc = ota_update_malloc(BUF_SIZE);

	buf = alloc;

	/*init download information buffer*/
	memset((u8 *)&DownloadInfo, 0, 2*sizeof(update_dw_info));

	/*arrange OTA/RDP image download information*/
	if(pOtaTgtHdr->RdpStatus == ENABLE) {
		ImageCnt = 2;
		if(pOtaTgtHdr->FileImgHdr.Offset < pOtaTgtHdr->FileRdpHdr.Offset) {
			DownloadInfo[0].ImgId = OTA_IMAG;
			/* get OTA image and Write New Image to flash, skip the signature, 
			not write signature first for power down protection*/
			DownloadInfo[0].FlashAddr = addr -SPI_FLASH_BASE + 8;
			DownloadInfo[0].ImageLen = pOtaTgtHdr->FileImgHdr.ImgLen - 8;/*skip the signature*/
			DownloadInfo[0].ImgOffset = pOtaTgtHdr->FileImgHdr.Offset;
			DownloadInfo[1].ImgId = RDP_IMAG;
			DownloadInfo[1].FlashAddr = RDP_FLASH_ADDR - SPI_FLASH_BASE;
			DownloadInfo[1].ImageLen = pOtaTgtHdr->FileRdpHdr.ImgLen;
			DownloadInfo[1].ImgOffset = pOtaTgtHdr->FileRdpHdr.Offset;
		} else {
			DownloadInfo[0].ImgId = RDP_IMAG;
			DownloadInfo[0].FlashAddr = RDP_FLASH_ADDR - SPI_FLASH_BASE;
			DownloadInfo[0].ImageLen = pOtaTgtHdr->FileRdpHdr.ImgLen;
			DownloadInfo[0].ImgOffset = pOtaTgtHdr->FileRdpHdr.Offset;	
			DownloadInfo[1].ImgId = OTA_IMAG;
			/* get OTA image and Write New Image to flash, skip the signature, 
			not write signature first for power down protection*/
			DownloadInfo[1].FlashAddr = addr -SPI_FLASH_BASE + 8;
			DownloadInfo[1].ImageLen = pOtaTgtHdr->FileImgHdr.ImgLen - 8;/*skip the signature*/
			DownloadInfo[1].ImgOffset = pOtaTgtHdr->FileImgHdr.Offset;
		}
	}else {
			ImageCnt = 1;
			DownloadInfo[0].ImgId = OTA_IMAG;
			/* get OTA image and Write New Image to flash, skip the signature, 
			not write signature first for power down protection*/
			DownloadInfo[0].FlashAddr = addr -SPI_FLASH_BASE + 8;
			DownloadInfo[0].ImageLen = pOtaTgtHdr->FileImgHdr.ImgLen - 8;/*skip the signature*/
			DownloadInfo[0].ImgOffset = pOtaTgtHdr->FileImgHdr.Offset;		
	}

	/*initialize the reveiving counter*/
	TempLen = (pOtaTgtHdr->FileHdr.HdrNum * pOtaTgtHdr->FileImgHdr.ImgHdrLen) + sizeof(update_file_hdr);

	printf("\n\r OTA Image Address = %x\n", addr);
	if(pOtaTgtHdr->RdpStatus == ENABLE) {	
		printf("\n\r RDP Image Address = %x\n", RDP_FLASH_ADDR);
	}
	
	/*downloading parse the OTA and RDP image from the data stream sent by server*/
	for(i = 0; i < ImageCnt; i++) {
		/*the next image length*/
		RemainBytes = DownloadInfo[i].ImageLen;
		/*download the new firmware from server*/
		while(RemainBytes > 0){
			buf = alloc;
			if(IncFg == 1) {
				IncFg = 0;
				read_bytes = read_bytes_buf;
			} else {
				memset(buf, 0, BUF_SIZE);
				read_bytes = read(socket, buf, BUF_SIZE);
				if(read_bytes == 0){
					break; // Read end			
				}
				if(read_bytes < 0){
					OtaImgSize = -1;
					printf("\n\r[%s] Read socket failed", __FUNCTION__);
					goto exit;
				}
				read_bytes_buf = read_bytes;
				TempLen += read_bytes;
			}
			
			if(TempLen > DownloadInfo[i].ImgOffset) {
				if(!OtaFg) {   				/*reach the desired image, the first packet process*/
					OtaFg = 1;
					TempCnt = TempLen -DownloadInfo[i].ImgOffset;
					if(DownloadInfo[i].ImgId == OTA_IMAG) {
						if(TempCnt < 8) {
							SigCnt = TempCnt;
						} else {
							SigCnt = 8;
						}

						_memcpy(signature, buf + read_bytes -TempCnt, SigCnt);

						if((SigCnt < 8) || (TempCnt -8 == 0)) {
							continue;
						}

						buf = buf + (read_bytes -TempCnt + 8);
						read_bytes = TempCnt -8;
					} else {
						buf = buf + read_bytes -TempCnt;
						read_bytes = TempCnt;
					}
				} else {					/*normal packet process*/
					if(DownloadInfo[i].ImgId == OTA_IMAG) {
						if(SigCnt < 8) {
							if(read_bytes < (8 -SigCnt)) {
								_memcpy(signature + SigCnt, buf, read_bytes);
								SigCnt += read_bytes;
								continue;
							} else {
								_memcpy(signature + SigCnt, buf, (8 -SigCnt));
								buf = buf + (8 - SigCnt);
								read_bytes -= (8 - SigCnt) ;
								SigCnt = 8;
								if(!read_bytes) {
									continue;
								}
							}
						}
					}
				}
				
				RemainBytes -= read_bytes;
				if(RemainBytes < 0) {
					read_bytes = read_bytes -(-RemainBytes);
				}
				device_mutex_lock(RT_DEV_LOCK_FLASH);
				if(flash_stream_write(&flash, DownloadInfo[i].FlashAddr + size, read_bytes, buf) < 0){
					printf("\n\r[%s] Write sector failed", __FUNCTION__);
					device_mutex_unlock(RT_DEV_LOCK_FLASH);
					goto exit;
				}
				device_mutex_unlock(RT_DEV_LOCK_FLASH);
				size += read_bytes;
			}

		}
		/*if complete downloading OTA image, acquire the image size*/
		if(DownloadInfo[i].ImgId == OTA_IMAG) {
			OtaImgSize = size;
		}
		/*update flag status*/
		size = 0;
		OtaFg = 0;
		IncFg = 1;
	}

exit:

	ota_update_free(alloc);

	return OtaImgSize;
	
}

/**
    verify new firmware checksum
    addr: new image address
    len: new image length
    signature: point to signature strings
    pOtaTgtHdr: point to target image OTA  header
  */
u32 verify_ota_checksum(u32 addr, u32 len, u8 * signature, update_ota_target_hdr * pOtaTgtHdr)
{
	u32 i;
	flash_t flash;
	u32 flash_checksum=0;
	u32 rdp_checksum=0;
#if CONFIG_CUSTOM_SIGNATURE
	uint32_t read_custom_sig[8];
#endif
	u32 NewImg2BlkSize;
	NewImg2BlkSize = ((len - 1)/4096) + 1;
	u8 * pTempbuf;
	int k;
	int rlen;

#if CONFIG_CUSTOM_SIGNATURE
	/*unmask the upgrade space to read the signature in new firmware*/
	OTF_Mask(1, (addr - SPI_FLASH_BASE), NewImg2BlkSize, 0);

	/* check custom signature */
	_memcpy(read_custom_sig, (void const*)(addr + IMAGE_HEADER_LEN), 32);
	printf("\n\r[%s] read_custom_sig %s\n", __FUNCTION__ , (char*)read_custom_sig);
	Cache_Flush();

	if (strcmp((char*)read_custom_sig, cus_sig_demo)) {
		goto error;
	}
#endif

	/*the upgrade space should be masked, because the encrypt firmware is used 
	for checksum calculation*/
	OTF_Mask(1, (addr - SPI_FLASH_BASE), NewImg2BlkSize, 1);

	pTempbuf = ota_update_malloc(BUF_SIZE);

	/* read flash data back and calculate checksum */
	for(i=0;i<len;i+=BUF_SIZE){
		rlen = (len-i)>BUF_SIZE?BUF_SIZE:(len-i);
		flash_stream_read(&flash, addr - SPI_FLASH_BASE+i+8, rlen, pTempbuf);
		Cache_Flush();
		for(k=0;k<rlen;k++)
			flash_checksum+=pTempbuf[k];
	}
	/*add signature's checksum*/
	for(i = 0; i < 8; i++) {
		flash_checksum += signature[i];
	}

	printf("\n\rUpdate file size = %d flash checksum 0x%8x attached checksum 0x%8x\n", len, flash_checksum, pOtaTgtHdr->FileImgHdr.Checksum);

	/*mask the rdp flash space to read the rdp image and calculate the checksum*/
	if(pOtaTgtHdr->RdpStatus == ENABLE) {
		for(i = 0; i < pOtaTgtHdr->FileRdpHdr.ImgLen; i++) {
			flash_stream_read(&flash, RDP_FLASH_ADDR - SPI_FLASH_BASE+i, 1, pTempbuf);
			Cache_Flush();
			rdp_checksum += pTempbuf[0];
		}
	}

	OTF_Mask(1, (addr - SPI_FLASH_BASE), NewImg2BlkSize, 0);

	ota_update_free(pTempbuf);

	if(flash_checksum != pOtaTgtHdr->FileImgHdr.Checksum) {
		printf("\n\r OTA image checksum error!!!\n");
		goto error;
	} else {
		printf("\n\r OTA image checksum ok!!!\n");
	}

	if(pOtaTgtHdr->RdpStatus == ENABLE) {
		if(rdp_checksum != pOtaTgtHdr->FileRdpHdr.Checksum) {
			printf("\n\r RDP image checksum error!!!\n");
			goto error;
		} else {
			printf("\n\r RDP image checksum ok!!!\n");
		}
	}

	return 1;
error:
	return 0;	
}

/**
    update signature
    addr: new image address
  */
u32 change_ota_signature(u32 addr, u8 * signature, u32 ota_target_index)
{
	flash_t flash;
	device_mutex_lock(RT_DEV_LOCK_FLASH);
	/*write the signature finally*/
	if(flash_stream_write(&flash, addr - SPI_FLASH_BASE, 8, signature) < 0){
		printf("\n\r[%s] Write sector failed", __FUNCTION__);
		device_mutex_unlock(RT_DEV_LOCK_FLASH);
		goto error;
	}
	OTA_Change(ota_target_index);
	device_mutex_unlock(RT_DEV_LOCK_FLASH);
	printf("\n\r[%s] Update OTA success!", __FUNCTION__);	
	return 1;
error:
	return 0;
}

/**
    get the address for arranging new firmware and check new address validity
  */
u32 get_ota_address(u32 ota_target_index, u32 * new_addr, update_ota_target_hdr * pOtaTgtHdr)
{
	IMAGE_HEADER *OTA1Hdr = NULL;
	uint32_t OTA1Len = 0;
	IMAGE_HEADER *FlashImgDataHdr = NULL;
	uint32_t ota2_addr;

	ota2_addr = HAL_READ32(SPI_FLASH_BASE, OFFSET_DATA);

	printf("ota2_addr = %x\n", ota2_addr);

	/*if the OTA2 address is not programmed in system data zone, the default OTA2 
	address is used.	This operation is just used in the local OTA update demo. For the 
	cloud OTA upgrade based on this demo, this operation may not be used.*/
	if(ota2_addr == 0xffffffff) {
		ota_write_ota2_addr(OTA2_DEFAULT_ADDR);
		ota2_addr = HAL_READ32(SPI_FLASH_BASE, OFFSET_DATA);
	}

	if((ota2_addr%4096) != 0) {
		printf("\n\r[%s] ota addr in sys data space not 4k aligned 0x%x", __FUNCTION__, ota2_addr);
		goto error;
	}

	if(ota_target_index == OTA_INDEX_2) {
		/* OAT2 address should not in OTA1 image & should 4K alignment */
		OTA1Hdr = (IMAGE_HEADER *)(OTA1_ADDR);
		OTA1Len = OTA1Hdr->image_size;	
		FlashImgDataHdr = (IMAGE_HEADER *)((u32)OTA1Hdr + OTA1Len + IMAGE_HEADER_LEN);
		if ((ota2_addr < ((u32)FlashImgDataHdr + FlashImgDataHdr->image_size + IMAGE_HEADER_LEN)) && ((ota2_addr & 0xfff) == 0)) {
			printf("\n\r[%s] illegal ota addr 0x%x", __FUNCTION__, ota2_addr);
			goto error;
		}
		*new_addr = ota2_addr;
	} else {
		*new_addr = OTA1_ADDR;
		/* if OTA1 will be update, image size should not cross OTA2 */
		if(pOtaTgtHdr->FileImgHdr.ImgLen > (ota2_addr - *new_addr)) {
			printf("\n\r[%s] illegal new image length 0x%x", __FUNCTION__, pOtaTgtHdr->FileImgHdr.ImgLen);
			goto error;
		}
	}

	/*if the flash offset parameter of the corresponding OTA index in file firmware header received from 
	server is needed in cloud OTA upgrade based on this demo, add code here*/
	#if 1
		/*check the OTA Image Address*/
		if((*new_addr) != pOtaTgtHdr->FileImgHdr.FlashOffset){
			printf("\n\r[%s] pOtaTgtHdr->FileImgHdr.FlashOffset = %p\n", __FUNCTION__, pOtaTgtHdr->FileImgHdr.FlashOffset);
			/*add code for cloud OTA upgrade use*/
			#if 1
				goto error;
			#endif
		}
	#endif

	if(*new_addr == 0xFFFFFFFF) {
		printf("\n\r[%s] update address is invalid \n", __FUNCTION__);
		goto error;	
	}

	return 1;
	
error:
	return 0;	
}

/**
  * @brief    OTA upgrade task for single image method
  *              step1: connect to server
  *              step2: receive firmware file header
  *              step3: parse firmware file header and get the target OTA image header
  *              step4: erase flash space for new firmware
  *              step5: download new firmware from server and write it to flash
  *              step6: verify checksum and update signature
  *              step7: OTA upgrade successfully, restart device 
  * @retval None
  */
static void ota_update_single_img_local_task(void *param)
{
	int server_socket;
	struct sockaddr_in server_addr;
	unsigned char *buf, *alloc;
	int read_bytes = 0, size = 0;
	update_cfg_local_t *cfg = (update_cfg_local_t *)param;
	flash_t flash;
	uint32_t NewImg2Len = 0, NewImg2Addr = 0, file_info[3];
	int ret = -1 ;
	uint32_t ota_target_index = OTA_INDEX_2;
	update_ota_target_hdr OtaTargetHdr;
	char * pImgId;
	u32 RevHdrLen;
	u8 signature[9] = {0};

	memset((u8 *)&OtaTargetHdr, 0, sizeof(update_ota_target_hdr));

	printf("\n\r[%s] Update task start\n", __FUNCTION__);
	alloc = ota_update_malloc(BUF_SIZE);
	if(!alloc){
		printf("\n\r[%s] Alloc buffer failed", __FUNCTION__);
		goto update_ota_exit;
	}

	 /*-------------------step1: connect to server-------------------*/
	server_socket = socket(AF_INET, SOCK_STREAM, 0);
	if(server_socket < 0){
		printf("\n\r[%s] Create socket failed", __FUNCTION__);
		goto update_ota_exit;
	}
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = cfg->ip_addr;
	server_addr.sin_port = cfg->port;

	if(connect(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1){
		printf("\n\r[%s] socket connect failed", __FUNCTION__);
		goto update_ota_exit;
	}
	
	DBG_INFO_MSG_OFF(_DBG_SPI_FLASH_);
	/* check OTA index we should update */
	if (ota_get_cur_index() == OTA_INDEX_1) {
		ota_target_index = OTA_INDEX_2;
		#if OTA_DEBUG_INFO
			DBG_8195A("OTA2 address space will be upgraded\n");
		#endif
	} else {
		ota_target_index = OTA_INDEX_1;
		#if OTA_DEBUG_INFO
			DBG_8195A("OTA1 address space will be upgraded\n");
		#endif
	}
	#if OTA_DEBUG_INFO
		DBG_8195A("ota_target_index = %d\n", ota_target_index);
	#endif	

	/*=============receive file_info[] from server=====================*/
	/*Because Ameba1 has this operation below. For the compatibility, AmebaZ also do this.
	This file_info includes the file_size and checksum information of the total firmware file
	(Firmware Header + OTA1+OTA2...). Even though the file_info is received from server 
	in this step, however, it won't be used for AmebaZ.*/
	/*clear file_info buffer*/
	memset(file_info, 0, sizeof(file_info));
	if(!recv_file_info_from_server((u8 *)file_info, sizeof(file_info), server_socket)) {
		printf("\n\r[%s] receive file_info failed", __FUNCTION__);
		goto update_ota_exit;
	}

	printf("file_info[0] (checksum)= %p\n", file_info[0]);
	printf("file_info[1] (NULL)= %p\n", file_info[1]);
	printf("file_info[2] (file size)= %p\n", file_info[2]);

	/*----------------step2: receive firmware file header---------------------*/
	if(!recv_ota_file_hdr(alloc, &RevHdrLen, &OtaTargetHdr, server_socket)) {
		printf("\n\r[%s] rev firmware header failed", __FUNCTION__);
		goto update_ota_exit;
	}

	pImgId = (char *)(&OtaImgId[ota_target_index]);
	printf("\n\rTempBuf = %s\n",pImgId);

	/* -----step3: parse firmware file header and get the target OTA image header-----*/
	if(!get_ota_tartget_header(alloc, RevHdrLen, &OtaTargetHdr, pImgId)) {
		printf("\n\rget OTA header failed\n");
		goto update_ota_exit;
	}

	ota_update_free(alloc);

	/*get new image addr and check new address validity*/
	if(!get_ota_address(ota_target_index, &NewImg2Addr, &OtaTargetHdr)) {
		printf("\n\rget OTA address failed\n");
		goto update_ota_exit;
	}

	/*get new image length from the firmware header*/
	NewImg2Len = OtaTargetHdr.FileImgHdr.ImgLen;

	/*-------------------step4: erase flash space for new firmware--------------*/
	/*erase flash space new OTA image */
	erase_ota_target_flash(NewImg2Addr, NewImg2Len);
	/*erase flash space for new RDP image*/
	if(OtaTargetHdr.RdpStatus == ENABLE) {
		device_mutex_lock(RT_DEV_LOCK_FLASH);
		flash_erase_sector(&flash, RDP_FLASH_ADDR - SPI_FLASH_BASE);
		device_mutex_unlock(RT_DEV_LOCK_FLASH);
	}


	/*---------step5: download new firmware from server and write it to flash--------*/
	size = download_new_fw_from_server(NewImg2Addr, server_socket, &OtaTargetHdr, signature);
	if((size < 0) || (size != (OtaTargetHdr.FileImgHdr.ImgLen - 8))) {
		printf("\n\rdownload new firmware failed\n");
		goto update_ota_exit;
	}

	printf("\nsize = %x\n", size);
	printf("\nbuffer signature is: = %s",signature);

	 /*-------------step6: verify checksum and update signature-----------------*/
	if(verify_ota_checksum(NewImg2Addr, size, signature, &OtaTargetHdr)){
		if(!change_ota_signature(NewImg2Addr, signature, ota_target_index)) {
			printf("\n%s: change signature failed\n");
			goto update_ota_exit;
		}
		ret = 0;
	} else {
		/*if checksum error, clear the signature zone which has been 
		written in flash in case of boot from the wrong firmware*/
		#if 1
		device_mutex_lock(RT_DEV_LOCK_FLASH);
		flash_erase_sector(&flash, NewImg2Addr - SPI_FLASH_BASE);
		device_mutex_unlock(RT_DEV_LOCK_FLASH);
		#endif
	}

update_ota_exit:
	if(alloc)
		ota_update_free(alloc);
	if(server_socket >= 0)
		close(server_socket);
	if(param)
		ota_update_free(param);
	
	TaskOTA = NULL;	
	printf("\n\r[%s] Update task exit", __FUNCTION__);	
	/*-------------step7: OTA upgrade successfully, restart device------------*/
	if(!ret){
		printf("\n\r[%s] Ready to reboot", __FUNCTION__);	
		ota_platform_reset();
	}
	vTaskDelete(NULL);	
	return;

}

#else

static void ota_update_local_task(void *param)
{
	int server_socket;
	struct sockaddr_in server_addr;
	unsigned char *buf, *alloc;
	union { uint32_t u; unsigned char c[4]; }  file_checksum;
	int read_bytes = 0, write_bytes = 0,  size = 0, i = 0;
	update_cfg_local_t *cfg = (update_cfg_local_t *)param;
	uint32_t address, flash_checksum=0;
	flash_t	flash;
	uint32_t NewImg2BlkSize = 0, NewImg2Len = 0, NewImg2Addr = 0, file_info[3];
	uint32_t OTA1Len = 0;
	int ret = -1 ;
	uint32_t ota2_addr = OTA2_ADDR;
	IMAGE_HEADER *OTA1Hdr = NULL;
	IMAGE_HEADER *FlashImgDataHdr = NULL;
	uint32_t ota_target_index = OTA_INDEX_2;

#if CONFIG_CUSTOM_SIGNATURE
	uint32_t read_custom_sig[8];
#endif

	printf("\n\r[%s] Update task start", __FUNCTION__);
	alloc = ota_update_malloc(BUF_SIZE);
	if(!alloc){
		printf("\n\r[%s] Alloc buffer failed", __FUNCTION__);
		goto update_ota_exit;
	}
	
	/* Connect socket */
	server_socket = socket(AF_INET, SOCK_STREAM, 0);
	if(server_socket < 0){
		printf("\n\r[%s] Create socket failed", __FUNCTION__);
		goto update_ota_exit;
	}
	server_addr.sin_family = AF_INET;
	server_addr.sin_addr.s_addr = cfg->ip_addr;
	server_addr.sin_port = cfg->port;

	if(connect(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) == -1){
		printf("\n\r[%s] socket connect failed", __FUNCTION__);
		goto update_ota_exit;
	}
	
	DBG_INFO_MSG_OFF(_DBG_SPI_FLASH_);

	/* OAT2 address should not in OTA1 image & should 4K alignment */
	OTA1Hdr = (IMAGE_HEADER *)(OTA1_ADDR);
	OTA1Len = OTA1Hdr->image_size;	
	FlashImgDataHdr = (IMAGE_HEADER *)((u32)OTA1Hdr + OTA1Len + IMAGE_HEADER_LEN);
	if ((ota2_addr > ((u32)FlashImgDataHdr + FlashImgDataHdr->image_size + IMAGE_HEADER_LEN)) && ((ota2_addr & 0xfff) == 0)) {
		ota_write_ota2_addr( &flash, ota2_addr);
	} else {
		printf("\n\r[%s] illegal ota addr 0x%x", __FUNCTION__, ota2_addr);
		goto update_ota_exit;
	}

	/* check OTA index we should update */
	if (ota_get_cur_index() == OTA_INDEX_1) {
		NewImg2Addr = OTA2_ADDR;
		ota_target_index = OTA_INDEX_2;
		#if OTA_DEBUG_INFO
			DBG_8195A("OTA2 address space will be upgraded\n");
		#endif
	} else {
		NewImg2Addr = OTA1_ADDR;
		ota_target_index = OTA_INDEX_1;
		#if OTA_DEBUG_INFO
			DBG_8195A("OTA1 address space will be upgraded\n");
		#endif
	}	

	#if OTA_DEBUG_INFO
		DBG_8195A("ota_target_index = %d\n", ota_target_index);
	#endif	

	/*-------------AmebaZ handshakes with server-------------*/

	alloc[0] = (unsigned char)ota_target_index;
	
	/*firstly, AmebaZ should send the OTA index needed for upgrade to server*/
	write_bytes = write(server_socket, alloc, 1);
	if(write_bytes < 0){
		printf("\n\r[%s] write socket failed", __FUNCTION__);
			goto update_ota_exit;
	}

	/* get update image info from server*/
	memset(file_info, 0, sizeof(file_info));	
	printf("\n\r[%s] Read info first", __FUNCTION__);
	read_bytes = read(server_socket, file_info, sizeof(file_info));
	// !DW checksum !DW padding 0 !DW file size !DW
	printf("\n\r[%s] info %d bytes", __FUNCTION__, read_bytes);
	printf("\n\r[%s] tx chechsum 0x%x, file size 0x%x", __FUNCTION__, file_info[0],file_info[2]);
	printf("\n\r[%s] OTA index received from server is %d", __FUNCTION__, file_info[1]);
	if(file_info[2] == 0){
		printf("\n\r[%s] No checksum and file size", __FUNCTION__);
		goto update_ota_exit;
	}
	NewImg2Len = file_info[2];

	printf("\n\r[%s] update addr:%x update len: %x", __FUNCTION__, NewImg2Addr, NewImg2Len);

	/* if OTA1 will be update, image size should not cross OTA2 */
	if(NewImg2Addr == OTA1_ADDR) {
		if(NewImg2Len > (OTA2_ADDR-OTA1_ADDR) ){	// firmware size too large
			printf("\n\r[%s]  image size should not cross OTA2 \n", __FUNCTION__);
			goto update_ota_exit;
		}
	}
	if(NewImg2Addr == 0xFFFFFFFF) {
		printf("\n\r[%s] update address is invalid \n", __FUNCTION__);
		goto update_ota_exit;	
	}
	
	/* Erase upgraded image 2 region */
	printf("\n\r[%s] NewImg2Len %d  ", __FUNCTION__, NewImg2Len);
	NewImg2BlkSize = ((NewImg2Len - 1)/4096) + 1;

	printf("\n\r[%s] NewImg2BlkSize %d  0x%8x", __FUNCTION__, NewImg2BlkSize, NewImg2BlkSize);
	device_mutex_lock(RT_DEV_LOCK_FLASH);
	for( i = 0; i < NewImg2BlkSize; i++)
		flash_erase_sector(&flash, NewImg2Addr -SPI_FLASH_BASE + i * 4096);
	device_mutex_unlock(RT_DEV_LOCK_FLASH);

	/*the upgrade space should be masked, because the encrypt firmware is used 
	for checksum calculation*/
	OTF_Mask(1, (NewImg2Addr - SPI_FLASH_BASE), NewImg2BlkSize, 1);
	
	/* get OTA image and Write New Image to flash */
	address = NewImg2Addr -SPI_FLASH_BASE ;
	printf("\n\r");
	while(1){
		memset(alloc, 0, BUF_SIZE);
		read_bytes = read(server_socket, alloc, BUF_SIZE);
		if(read_bytes == 0) break; // Read end
		if(read_bytes < 0){
			printf("\n\r[%s] Read socket failed", __FUNCTION__);
			goto update_ota_exit;
		}
		
		if(read_bytes<4)
			printf("\n\r[%s] recv small packet", __FUNCTION__);
		
		device_mutex_lock(RT_DEV_LOCK_FLASH);
		if(flash_stream_write(&flash, address + size, read_bytes, alloc) < 0){
			printf("\n\r[%s] Write sector failed", __FUNCTION__);
			device_mutex_unlock(RT_DEV_LOCK_FLASH);
			goto update_ota_exit;
		}
		device_mutex_unlock(RT_DEV_LOCK_FLASH);
		size += read_bytes;
		
		if(size == NewImg2Len)
			break;
	}
	printf("\n\r");

	file_checksum.u = file_info[0];

#if CONFIG_CUSTOM_SIGNATURE
	/*unmask the upgrade space to read the signature in new firmware*/
	OTF_Mask(1, (NewImg2Addr - SPI_FLASH_BASE), NewImg2BlkSize, 0);

	/* check custom signature */
	_memcpy(read_custom_sig, (void const*)(NewImg2Addr + IMAGE_HEADER_LEN), 32);
	printf("\n\r[%s] read_custom_sig %s", __FUNCTION__ , (char*)read_custom_sig);
	Cache_Flush();

	if (strcmp((char*)read_custom_sig, cus_sig_demo)) {
		goto update_ota_exit;
	}
	OTF_Mask(1, (NewImg2Addr - SPI_FLASH_BASE), NewImg2BlkSize, 1);
#endif

	/* read flash data back and calculate checksum */
	for(i=0;i<size;i+=BUF_SIZE){
		int k;
		int rlen = (size-i)>BUF_SIZE?BUF_SIZE:(size-i);
		flash_stream_read(&flash, NewImg2Addr - SPI_FLASH_BASE+i, rlen, alloc);
		for(k=0;k<rlen;k++)
			flash_checksum+=alloc[k];
	}

	printf("\n\rUpdate file size = %d flash checksum 0x%8x attached checksum 0x%8x", size, flash_checksum, file_checksum.u);

	OTF_Mask(1, (NewImg2Addr - SPI_FLASH_BASE), NewImg2BlkSize, 0);

	/* compare checksum with received checksum */
	if (file_checksum.u == flash_checksum) {
		device_mutex_lock(RT_DEV_LOCK_FLASH);
		OTA_Change(ota_target_index);
		device_mutex_unlock(RT_DEV_LOCK_FLASH);
		printf("\n\r[%s] Update OTA success!", __FUNCTION__);

		ret = 0;
	} else {
		/*if checksum error, clear the signature zone which has been 
		written in flash in case of boot from the wrong firmware*/
		device_mutex_lock(RT_DEV_LOCK_FLASH);
		flash_erase_sector(&flash, NewImg2Addr - SPI_FLASH_BASE);
		device_mutex_unlock(RT_DEV_LOCK_FLASH);
	}

update_ota_exit:
	if(alloc)
		ota_update_free(alloc);
	if(server_socket >= 0)
		close(server_socket);
	if(param)
		ota_update_free(param);
	
	TaskOTA = NULL;	
	printf("\n\r[%s] Update task exit", __FUNCTION__);	
	if(!ret){
		printf("\n\r[%s] Ready to reboot", __FUNCTION__);	
		ota_platform_reset();
	}
	vTaskDelete(NULL);	
	return;

}

#endif



//---------------------------------------------------------------------
int update_ota_local(char *ip, int port)
{
	update_cfg_local_t *pUpdateCfg;
	
	if(TaskOTA){
		printf("\n\r[%s] Update task has created.", __FUNCTION__);
		return 0;
	}
	pUpdateCfg = ota_update_malloc(sizeof(update_cfg_local_t));
	if(pUpdateCfg == NULL){
		printf("\n\r[%s] Alloc update cfg failed", __FUNCTION__);
		return -1;
	}
	pUpdateCfg->ip_addr = inet_addr(ip);
	pUpdateCfg->port = ntohs(port);
	#if SINGLE_IMG_OTA_UPGRADE
	if(xTaskCreate(ota_update_single_img_local_task, "OTA_server", STACK_SIZE, pUpdateCfg, TASK_PRIORITY, &TaskOTA) != pdPASS){
	  	ota_update_free(pUpdateCfg);
		printf("\n\r[%s] Create update task failed", __FUNCTION__);
	}
	#else
	if(xTaskCreate(ota_update_local_task, "OTA_server", STACK_SIZE, pUpdateCfg, TASK_PRIORITY, &TaskOTA) != pdPASS){
	  	ota_update_free(pUpdateCfg);
		printf("\n\r[%s] Create update task failed", __FUNCTION__);
	}
	#endif
	return 0;
}
#endif // #if (SERVER_TYPE == SERVER_LOCAL)

//---------------------------------------------------------------------
#if (SERVER_TYPE == SERVER_CLOUD)
//---------------------------------------------------------------------
int update_ota_cloud(char *repository, char *file_path)
{
	update_cfg_cloud_t *pUpdateCfg;
	
	if(TaskOTA){
		printf("\n\r[%s] Update task has created.", __FUNCTION__);
		return 0;
	}
	pUpdateCfg = ota_update_malloc(sizeof(update_cfg_cloud_t));
	if(pUpdateCfg == NULL){
		printf("\n\r[%s] Alloc update cfg failed.", __FUNCTION__);
		goto exit;
	}
	if(strlen(repository) > (REPOSITORY_LEN-1)){
		printf("\n\r[%s] Repository length is too long.", __FUNCTION__);
		goto exit;
	}
	if(strlen(file_path) > (FILE_PATH_LEN-1)){
		printf("\n\r[%s] File path length is too long.", __FUNCTION__);
		goto exit;
	}
	strcpy((char*)pUpdateCfg->repository, repository);
	strcpy((char*)pUpdateCfg->file_path, file_path);
	  	
	if(xTaskCreate(update_ota_cloud_task, "OTA_server", STACK_SIZE, pUpdateCfg, TASK_PRIORITY, &TaskOTA) != pdPASS){
		printf("\n\r[%s] Create update task failed", __FUNCTION__);
		goto exit;
	}

exit:
	ota_update_free(pUpdateCfg);
	return 0;
}
#endif // #if (SERVER_TYPE == SERVER_CLOUD)

//---------------------------------------------------------------------
void cmd_update(int argc, char **argv)
{
//	printf("\n\r[%s] Firmware A", __FUNCTION__);
#if (SERVER_TYPE == SERVER_LOCAL)
	int port;
	if(argc != 3){
		printf("\n\r[%s] Usage: update IP PORT", __FUNCTION__);
		return;
	}
	port = atoi(argv[2]);
	update_ota_local(argv[1], port);
#endif
#if (SERVER_TYPE == SERVER_CLOUD)
	if(argc != 3){
		printf("\n\r[%s] Usage: update REPOSITORY FILE_PATH", __FUNCTION__);
		return;
	}
	update_ota_cloud(argv[1], argv[2]);
#endif
}

/*  chose to boot ota2 image or not */
void cmd_ota_image(bool cmd)
{
	if (cmd == 1)
		OTA_Change(OTA_INDEX_2);
	else
		OTA_Change(OTA_INDEX_1);
}

#ifdef HTTP_OTA_UPDATE
//static flash_t flash_ota_rtl8710b;
uint32_t http_ota_target_index = OTA_INDEX_2;

/******************************************************************************************************************
** Function Name  : update_ota_http_connect_server
** Description    : connect to the OTA server
** Input          : server_socket: the socket used
**					host: host address of the OTA server
**					port: port of the OTA server
** Return         : connect ok:	socket value
**					Failed:		-1
*******************************************************************************************************************/
int update_ota_http_connect_server(int server_socket, char *host, int port){
	struct sockaddr_in server_addr;
	struct hostent *server;
	
	server_socket = socket(AF_INET, SOCK_STREAM, 0);
	if(server_socket < 0){
		printf("\n\r[%s] Create socket failed", __FUNCTION__);
		return -1;
	}
	printf("[%s] Create socket: %d success!\n", __FUNCTION__, server_socket);
	server = gethostbyname(host);
	if(server == NULL){ 
		printf("[ERROR] Get host ip failed\n");
		return -1;
	}

	memset(&server_addr,0,sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(port);
	memcpy(&server_addr.sin_addr.s_addr,server->h_addr,server->h_length);

	if (connect(server_socket,(struct sockaddr *)&server_addr, sizeof(server_addr)) < 0){
		printf("\n\r[%s] Socket connect failed", __FUNCTION__);
		return -1;
	 }

	return server_socket;
}

/******************************************************************************************************************
** Function Name  : parse_http_response
** Description    : Parse the http response to get some useful parameters
** Input          : response	: The http response got from server
**					response_len: The length of http response
**					result		: The struct that store the usful infor about the http response
** Return         : Parse OK:	1 -> Only got the status code
**								3 -> Got the status code and content_length, but didn't get the full header							
**								4 -> Got all the information needed
**					Failed:		-1
*******************************************************************************************************************/
int parse_http_response(uint8_t *response, uint32_t response_len, http_response_result_t *result) {
	uint32_t i, p, q, m;
	uint32_t header_end = 0;

	//Get status code
	if(0 == result->parse_status){//didn't get the http response
		uint8_t status[4] = {0};
		i = p = q = m = 0;
		for (; i < response_len; ++i) {
			if (' ' == response[i]) {
				++m;
				if (1 == m) {//after HTTP/1.1
					p = i;
				} 
				else if (2 == m) {//after status code
					q = i;
					break;
				}
			}
		}
		if (!p || !q || q-p != 4) {//Didn't get the status code
			return -1;
		}
		memcpy(status, response+p+1, 3);//get the status code
		result->status_code = atoi((char const *)status);
		if(result->status_code == 200)
			result->parse_status = 1;
		else{
			printf("\n\r[%s] The http response status code is %d", __FUNCTION__, result->status_code);
			return -1;
		}
	}

	//if didn't receive the full http header
	if(3 == result->parse_status){//didn't get the http response
		p = q = 0;
		for (i = 0; i < response_len; ++i) {
			if (response[i] == '\r' && response[i+1] == '\n' &&
				response[i+2] == '\r' && response[i+3] == '\n') {//the end of header
				header_end = i+4;
				result->parse_status = 4;
				result->header_len = header_end;
				result->body = response + header_end;
				break;
			}
		}
		if (3 == result->parse_status) {//Still didn't receive the full header	
			result->header_bak = ota_update_malloc(HEADER_BAK_LEN + 1);
			memset(result->header_bak, 0, strlen(result->header_bak));
			memcpy(result->header_bak, response + response_len - HEADER_BAK_LEN, HEADER_BAK_LEN);
		}
	}

	//Get Content-Length
	if(1 == result->parse_status){//didn't get the content length
		uint32_t content_length = 0;
		const uint8_t *content_length_buf1 = "CONTENT-LENGTH";
		const uint8_t *content_length_buf2 = "Content-Length";
		const uint32_t content_length_buf_len = strlen(content_length_buf1);
		p = q = 0;
		
		for (i = 0; i < response_len; ++i) {
			if (response[i] == '\r' && response[i+1] == '\n') {
				q = i;//the end of the line
				if (!memcmp(response+p, content_length_buf1, content_length_buf_len) ||
						!memcmp(response+p, content_length_buf2, content_length_buf_len)) {//get the content length
					int j1 = p+content_length_buf_len, j2 = q-1;
					while ( j1 < q && (*(response+j1) == ':' || *(response+j1) == ' ') ) ++j1;
					while ( j2 > j1 && *(response+j2) == ' ') --j2;
					uint8_t len_buf[12] = {0};
					memcpy(len_buf, response+j1, j2-j1+1);
					result->body_len = atoi((char const *)len_buf);
					result->parse_status = 2;
				}
				p = i+2;
			}
			if (response[i] == '\r' && response[i+1] == '\n' &&
					response[i+2] == '\r' && response[i+3] == '\n') {//Get the end of header
				header_end = i+4;//p is the start of the body
				if(result->parse_status == 2){//get the full header and the content length
					result->parse_status = 4;
					result->header_len = header_end;
					result->body = response + header_end;
				}
				else {//there are no content length in header	
					printf("\n\r[%s] No Content-Length in header", __FUNCTION__);
					return -1;
				}
				break;
			}	
		}
		
		if (1 == result->parse_status) {//didn't get the content length and the full header
			result->header_bak = ota_update_malloc(HEADER_BAK_LEN + 1);
			memset(result->header_bak, 0, strlen(result->header_bak));
			memcpy(result->header_bak, response + response_len - HEADER_BAK_LEN, HEADER_BAK_LEN);
		}
		else if (2 == result->parse_status) {//didn't get the full header but get the content length
			result->parse_status = 3;
			result->header_bak = ota_update_malloc(HEADER_BAK_LEN + 1);
			memset(result->header_bak, 0, strlen(result->header_bak));
			memcpy(result->header_bak, response + response_len - HEADER_BAK_LEN, HEADER_BAK_LEN);
		}
	}

	return result->parse_status;
}

/**
    get the address for arranging new firmware and check new address validity
  */
uint32_t update_ota_prepare_addr(void)
{
	
	uint32_t NewImg2Addr = 0; 
	IMAGE_HEADER *FlashImgDataHdr = NULL;
	IMAGE_HEADER *OTA1Hdr = NULL;
	uint32_t OTA1Len = 0;
	uint32_t ota2_addr = OTA2_ADDR;

	/*For future expansion*/
	#if 0
	ota2_addr = HAL_READ32(SPI_FLASH_BASE, OFFSET_DATA);
	#else
	ota2_addr = OTA2_ADDR;
	#endif
	
	if((ota2_addr%4096) != 0) {
		printf("\n[%s] ota addr in sys data space not 4k aligned 0x%x\n", __FUNCTION__, ota2_addr);
		return 0xFFFFFFFF;
	}
	if(ota2_addr <= OTA2_ADDR) {
		ota2_addr = OTA2_ADDR;
	}

	DBG_INFO_MSG_OFF(_DBG_SPI_FLASH_);
	/* check OTA index we should update */
	if (ota_get_cur_index() == OTA_INDEX_1) {
		http_ota_target_index = OTA_INDEX_2;
		printf("OTA2 address space will be upgraded\n");
	} else {
		http_ota_target_index = OTA_INDEX_1;
		printf("OTA1 address space will be upgraded\n");
	}	

	if(http_ota_target_index == OTA_INDEX_2) {
		/* OAT2 address should not in OTA1 image & should 4K alignment */
		OTA1Hdr = (IMAGE_HEADER *)(OTA1_ADDR);
		OTA1Len = OTA1Hdr->image_size;	
		FlashImgDataHdr = (IMAGE_HEADER *)((u32)OTA1Hdr + OTA1Len + IMAGE_HEADER_LEN);
		if ((ota2_addr > ((u32)FlashImgDataHdr + FlashImgDataHdr->image_size + IMAGE_HEADER_LEN)) && ((ota2_addr & 0xfff) == 0)) {
			ota_write_ota2_addr(ota2_addr);
		} else {
			printf("\n[%s] illegal ota addr 0x%x\n", __FUNCTION__, ota2_addr);
			return 0xFFFFFFFF;
		}
		NewImg2Addr = ota2_addr;
	} else {
		NewImg2Addr = OTA1_ADDR;
	}
	
	return NewImg2Addr;
}

static int restore_ota_file_hdr(int writelen, int len, uint8_t *buf, int socketid)
{
	int TempLen = 0, read_bytes = 0;
	if (writelen < len) {
		TempLen = len - writelen;
		while(TempLen) {
			read_bytes = recv(socketid, buf, TempLen, 0);
			if(read_bytes < 0){
				printf("[%s] read socket failed\n", __FUNCTION__);
				return -1;
			}
			if(read_bytes == 0) {
				writelen = len;
				break;
			}
			TempLen -= read_bytes;
			buf += read_bytes;
		}
		writelen = len;
	}
	writelen -= len;
	return writelen;
}

/******************************************************
        functionname    :   http_read_socket
        description     :   read data form socket
        socket          :   http server socket
        recevie_buf :   data buf.
        buf_len          :   read data length 
        return          :   >0 the data length read form 
                            socket 
                            <0 error,and need to close 
                            the socket.
******************************************************/
int http_read_socket( int socket, uint8_t *recevie_buf, int buf_len )
{
	int bytes_rcvd = -1; 
	if( socket < 0 ) {
		printf("[%s], socket is invalid\n", __FUNCTION__);
		return bytes_rcvd;
	}
	memset(recevie_buf, 0, buf_len);  

	bytes_rcvd = recv(socket, recevie_buf, buf_len, 0 );
	if(bytes_rcvd <= 0) {
		printf("[%s], Close HTTP Socket[%d].\n", socket, __FUNCTION__);
		return -2;
	}
	return bytes_rcvd;
}

int http_update_ota(char *host, int port, char *resource)
{
	int server_socket = -1;
	unsigned char *buf, *alloc=NULL, *request=NULL;
	int read_bytes = 0, i = 0;
	uint32_t address;
	uint32_t NewImg2Len = 0, NewImg2Addr = 0;
	int ret = -1;
	http_response_result_t rsp_result = {0};

	int writelen = 0;
	int templen = 0;
	int RevHdrLen = 0;
	u8 *TempBuf = NULL;
	uint32_t NewImg2BlkSize = 0;
	flash_t	flash;
	int ota_total_len = 0;
	static int RemainBytes = 0;
	u32 TempCnt = 0;
	static u32 SigCnt = 0;
	static unsigned long tick1, tick2;
	
	update_ota_target_hdr http_ota_target_hdr;
	u8 http_signature[9] = {0};
	int http_size = 0;
	
	alloc = (unsigned char *)ota_update_malloc(BUF_SIZE);
	if(!alloc){
		printf("[%s] Alloc buffer failed\n", __FUNCTION__);
		goto update_ota_exit;
	}

	// Connect server
	server_socket = update_ota_http_connect_server(server_socket, host, port);
	if(server_socket == -1){
		goto update_ota_exit;
	}

	NewImg2Addr = update_ota_prepare_addr();
	if(NewImg2Addr == -1){
		printf("[%s] illegal ota addr 0x%x\n", __FUNCTION__, NewImg2Addr);
		goto update_ota_exit;
	}
	
	/* Write New Image 2 sector*/
	if(NewImg2Addr != 0xFFFFFFFF){
		uint32_t idx = 0;
		int data_len = 0;
		printf("\n\r");
		
		/*send http request*/
		request = (unsigned char *) ota_update_malloc(strlen("GET /") + strlen(resource) + strlen(" HTTP/1.1\r\nHost: ") 
			+ strlen(host) + strlen("\r\n\r\n") + 1);
		sprintf(request, "GET /%s HTTP/1.1\r\nHost: %s\r\n\r\n", resource, host);

		ret = write(server_socket, request, strlen(request));
		if(ret < 0){
			printf("[%s] Send HTTP request failed\n", __FUNCTION__);
			goto update_ota_exit;
		}

		while (3 >= rsp_result.parse_status){//still read header
			if(0 == rsp_result.parse_status){//didn't get the http response
				memset(alloc, 0, BUF_SIZE);
				read_bytes = read(server_socket, alloc, BUF_SIZE);
				if(read_bytes <= 0){
					printf("[%s] Read socket failed\n", __FUNCTION__);
					goto update_ota_exit;
				}
	
				idx = read_bytes;
				memset(&rsp_result, 0, sizeof(rsp_result));
				if(parse_http_response(alloc, idx, &rsp_result) == -1){
					goto update_ota_exit;
				}
			} else if ((1 == rsp_result.parse_status) || (3 == rsp_result.parse_status)){//just get the status code
				memset(alloc, 0, BUF_SIZE);
				memcpy(alloc, rsp_result.header_bak, HEADER_BAK_LEN);
				ota_update_free(rsp_result.header_bak);
				rsp_result.header_bak = NULL;
				read_bytes = read(server_socket, alloc + HEADER_BAK_LEN, (BUF_SIZE - HEADER_BAK_LEN));
				if(read_bytes <= 0){
					printf("[%s] Read socket failed\n", __FUNCTION__);
					goto update_ota_exit;
				}
	
				idx = read_bytes + HEADER_BAK_LEN;

				if (parse_http_response(alloc, read_bytes + HEADER_BAK_LEN, &rsp_result) == -1){
					goto update_ota_exit;
				}
			}
		}
		
		if (0 == rsp_result.body_len) {
			printf("[%s] New firmware size = 0 !\n", __FUNCTION__);
			goto update_ota_exit;
		} else {
			printf("[%s] Download new firmware begin, total size : %d\n", __FUNCTION__, rsp_result.body_len);
		}
		
		buf = alloc + idx;
		writelen = idx - rsp_result.header_len;
		writelen = restore_ota_file_hdr(writelen, 16, buf,  server_socket);

		if (writelen < 0) {
			printf("[%s] read socket failed\n", __FUNCTION__);
		   	goto update_ota_exit;
		}

		buf = alloc + rsp_result.header_len;
		memcpy((u8*)(&http_ota_target_hdr.FileHdr), buf, sizeof(http_ota_target_hdr.FileHdr));
		memcpy((u8*)(&http_ota_target_hdr.FileImgHdr), buf+8, 8);

		/*read the remaining Header info*/
		buf = alloc + rsp_result.header_len + 16 + writelen;
		templen =  (http_ota_target_hdr.FileHdr.HdrNum * http_ota_target_hdr.FileImgHdr.ImgHdrLen) - 8;
		writelen = restore_ota_file_hdr(writelen, templen, buf,  server_socket);
		if (writelen < 0) {
			printf("[%s] read socket failed\n", __FUNCTION__);
		 	goto update_ota_exit;
		}
		/* ----- parse firmware file header and get the target OTA image header-----*/
		buf =  alloc + rsp_result.header_len;
		RevHdrLen = (http_ota_target_hdr.FileHdr.HdrNum * http_ota_target_hdr.FileImgHdr.ImgHdrLen) + 8;
		TempBuf = (u8 *)(&OtaImgId[http_ota_target_index]);
		printf("TempBuf = %s\n", TempBuf);
		if (!get_ota_tartget_header(buf, RevHdrLen, &http_ota_target_hdr, TempBuf)) {
			printf("Get OTA header failed\n");
			goto update_ota_exit;
		}

		/*get new image length from the firmware header*/
		NewImg2Len = http_ota_target_hdr.FileImgHdr.ImgLen;
		NewImg2BlkSize = ((NewImg2Len - 1)/4096) + 1;
		
		/* if OTA1 will be update, image size should not cross OTA2 */
		if(http_ota_target_index== OTA_INDEX_1) {
			if(http_ota_target_hdr.FileImgHdr.ImgLen > (OTA2_ADDR - OTA1_ADDR)) {
				printf("\n\r[%s] illegal new image length 0x%x", __FUNCTION__, http_ota_target_hdr.FileImgHdr.ImgLen);
				goto update_ota_exit;
			}
		}

		/* Erase upgraded image 2 region */
		printf("[%s] NewImg2BlkSize %d\n", __FUNCTION__, NewImg2BlkSize);
		device_mutex_lock(RT_DEV_LOCK_FLASH);
		for( i = 0; i < NewImg2BlkSize; i++)
			flash_erase_sector(&flash, NewImg2Addr -SPI_FLASH_BASE + i * 4096);
		device_mutex_unlock(RT_DEV_LOCK_FLASH);

		/*the upgrade space should be masked, because the encrypt firmware is used 
		for checksum calculation*/
		OTF_Mask(1, (NewImg2Addr - SPI_FLASH_BASE), NewImg2BlkSize, 1);
		/* get OTA image and Write New Image to flash, skip the signature, 
		not write signature first for power down protection*/
		address = NewImg2Addr -SPI_FLASH_BASE + 8;
		templen = (http_ota_target_hdr.FileHdr.HdrNum * http_ota_target_hdr.FileImgHdr.ImgHdrLen) + sizeof(http_ota_target_hdr.FileHdr);
		RemainBytes = http_ota_target_hdr.FileImgHdr.ImgLen - 8;
		if (writelen > 0) {
			writelen = idx - rsp_result.header_len;
		} else {
			writelen = templen;
		}

		buf = alloc + rsp_result.header_len;
  		ota_total_len += writelen;
		/*download the new firmware from server*/
		do {
			if (ota_total_len > http_ota_target_hdr.FileImgHdr.Offset) {
				tick1 = xTaskGetTickCount();
				/*reach the the desired image, the first packet process, restore signature*/
				TempCnt = ota_total_len - http_ota_target_hdr.FileImgHdr.Offset;
				if (TempCnt < 8) {
					SigCnt = TempCnt;
				} else {
					SigCnt = 8;
				}
				memcpy(http_signature, buf + writelen - TempCnt, SigCnt);
				if((SigCnt < 8) || (TempCnt -8 == 0)) {
					while (SigCnt < 8) {
						writelen = http_read_socket(server_socket, alloc, 8 -SigCnt);
					        if(writelen < 0 ) {
					          	printf("[%s], socket recv ota file fail!recived: %d\n", __func__, ota_total_len);
							goto update_ota_exit;
					        }
						memcpy(http_signature + SigCnt, alloc, writelen);
						SigCnt += writelen;
					};
					ota_total_len += (8 - TempCnt);
				} else {
					device_mutex_lock(RT_DEV_LOCK_FLASH);
					if(flash_stream_write(&flash, address + http_size, TempCnt -8, (buf + writelen-TempCnt + 8) ) < 0){
						printf("[%s] Write sector failed\n", __FUNCTION__);
						device_mutex_unlock(RT_DEV_LOCK_FLASH);
						goto update_ota_exit;
					}
					device_mutex_unlock(RT_DEV_LOCK_FLASH);
					http_size += TempCnt -8;
					RemainBytes -= http_size; 
				}
				printf("signature: %s, RemainBytes: %d, flash address: %d, http_size: %d\n", http_signature, RemainBytes, address, http_size);
				/*normal packet process*/
				while (RemainBytes) {
					writelen = http_read_socket(server_socket, alloc, BUF_SIZE);
				        if(writelen < 0 ) {
						printf("[%s], socket recv ota file fail!recived: %d\n", __func__, ota_total_len);
						goto update_ota_exit;
				        }
					ota_total_len += writelen;
					RemainBytes -= writelen;
					if (RemainBytes <= 0) {
						writelen = writelen - (-RemainBytes);
						RemainBytes = 0;
					}
					device_mutex_lock(RT_DEV_LOCK_FLASH);
					if (flash_stream_write(&flash, address + http_size, writelen, alloc) < 0){
						printf("[%s] Write sector failed\n", __FUNCTION__);
						device_mutex_unlock(RT_DEV_LOCK_FLASH);
						goto update_ota_exit;
					}
					device_mutex_unlock(RT_DEV_LOCK_FLASH);
					http_size += writelen;
					tick2 = xTaskGetTickCount();
					if (tick2 - tick1 > 1000) {
						printf("Download OTA file: %d Bytes, RemainBytes = %d Bytes\n", http_size, RemainBytes);
						tick1 = tick2;
					}
				}
			}

			if (RemainBytes) {
				writelen = http_read_socket(server_socket, alloc, BUF_SIZE);
					if(writelen < 0 ) {
						printf("[%s], socket recv ota file fail!recived:0x%x\n", __func__, ota_total_len);
						goto update_ota_exit;
					}
				ota_total_len += writelen;
				buf = alloc;
			}
		} while( RemainBytes );

		printf("[%s] Download new firmware %d bytes completed\n", __FUNCTION__, idx);
		printf("signature: %s, size = %d, OtaTargetHdr.FileImgHdr.ImgLen = %d\n", http_signature, http_size, http_ota_target_hdr.FileImgHdr.ImgLen);
		 /*------------- verify checksum and update signature-----------------*/
		if(verify_ota_checksum(NewImg2Addr, http_size, http_signature, &http_ota_target_hdr)){
			if(!change_ota_signature(NewImg2Addr, http_signature, http_ota_target_index)) {
				ret = -1;
				printf("\n[%s], change signature failed\n", __FUNCTION__);
			}
			printf("\n\r[%s] Update OTA success!", __FUNCTION__);
			ret = 0;
		} else {
		/*if checksum error, clear the signature zone which has been written in flash in case of boot from the wrong firmware*/
			device_mutex_lock(RT_DEV_LOCK_FLASH);
			flash_erase_sector(&flash, NewImg2Addr - SPI_FLASH_BASE);
			device_mutex_unlock(RT_DEV_LOCK_FLASH);
			printf("\n\r[%s] The checksume is wrong!\n\r", __FUNCTION__);
			ret = -1;
			goto update_ota_exit;
		}
	}
update_ota_exit:
	if(alloc)
		ota_update_free(alloc);
	if(request)
		ota_update_free(request);
	if(server_socket >= 0)
		close(server_socket);
	return ret;
}
#endif

//---------------------------------------------------------------------


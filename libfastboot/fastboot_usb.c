/*
 * Copyright (c) 2014, Intel Corporation
 * All rights reserved.
 *
 * Authors: Sylvain Chouleur <sylvain.chouleur@intel.com>
 *          Jeremy Compostella <jeremy.compostella@intel.com>
 *          Jocelyn Falempe <jocelyn.falempe@intel.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *    * Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    * Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer
 *      in the documentation and/or other materials provided with the
 *      distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <lib.h>
#include <ui.h>

#include "fastboot_ui.h"
#include "protocol.h"
#include "uefi_utils.h"
#include "fastboot_usb.h"
#include "protocol/UsbDeviceModeProtocol.h"
#include "smbios.h"

#define CONFIG_COUNT            1
#define INTERFACE_COUNT         1
#define ENDPOINT_COUNT          2
#define CFG_MAX_POWER           0x00	/* Max power consumption of the USB device from the bus for this config */
#define FB_IF_SUBCLASS          0x42	/* Fastboot subclass */
#define FB_IF_PROTOCOL          0x03	/* Fastboot protocol */
#define IN_ENDPOINT_NUM         1
#define OUT_ENDPOINT_NUM        2
#define FB_BULK_EP_PKT_SIZE     USB_BULK_EP_PKT_SIZE_HS	/* default to using high speed */
#define VENDOR_ID               0x8087	/* Intel Inc. */
#define PRODUCT_ID		0x09EF
#define BCD_DEVICE		0x0100

static data_callback_t		rx_callback  = NULL;
static data_callback_t		tx_callback  = NULL;
static start_callback_t		start_callback = NULL;
static USB_DEVICE_OBJ		gDevObj;
static USB_DEVICE_CONFIG_OBJ	device_configs[CONFIG_COUNT];
static USB_DEVICE_INTERFACE_OBJ gInterfaceObjs[INTERFACE_COUNT];
static USB_DEVICE_ENDPOINT_OBJ	gEndpointObjs[ENDPOINT_COUNT];

EFI_GUID gEfiUsbDeviceModeProtocolGuid = EFI_USB_DEVICE_MODE_PROTOCOL_GUID;
static EFI_USB_DEVICE_MODE_PROTOCOL *usb_device;

/* String descriptor table indexes */
typedef enum {
	STR_TBL_LANG,
	STR_TBL_MANUFACTURER,
	STR_TBL_PRODUCT,
	STR_TBL_SERIAL,
	STR_TBL_CONFIG,
	STR_TBL_INTERFACE,
	STR_TBL_COUNT,
} strTblIndex;

/* String descriptor Table */
#define LANG_EN_US		((UINT16)0x0409)
#define STR_MANUFACTURER	L"Intel Corporation"
#define STR_PRODUCT		L"Intel Product"
#define STR_SERIAL		L"INT123456"
#define STR_CONFIGURATION	L"USB-Update"
#define STR_INTERFACE		L"Fastboot"

static USB_STRING_DESCRIPTOR string_table[] = {
	{ 2 + sizeof(LANG_EN_US)	, USB_DESC_TYPE_STRING, {LANG_EN_US} },
	{ 2 + sizeof(STR_MANUFACTURER)	, USB_DESC_TYPE_STRING, STR_MANUFACTURER },
	{ 2 + sizeof(STR_PRODUCT)	, USB_DESC_TYPE_STRING, STR_PRODUCT },
	{ 2 + sizeof(STR_SERIAL)	, USB_DESC_TYPE_STRING, STR_SERIAL },
	{ 2 + sizeof(STR_CONFIGURATION)	, USB_DESC_TYPE_STRING, STR_CONFIGURATION },
	{ 2 + sizeof(STR_INTERFACE)	, USB_DESC_TYPE_STRING, STR_INTERFACE },
};

/* Complete Configuration structure for Fastboot */
struct fb_config_descriptor {
	EFI_USB_CONFIG_DESCRIPTOR    config;
	EFI_USB_INTERFACE_DESCRIPTOR interface;
	EFI_USB_ENDPOINT_DESCRIPTOR  ep_in;
	EFI_USB_ENDPOINT_DESCRIPTOR  ep_out;
} __attribute__((packed));

static struct fb_config_descriptor config_descriptor = {
	.config = {
		sizeof(EFI_USB_CONFIG_DESCRIPTOR),
		USB_DESC_TYPE_CONFIG,
		sizeof(struct fb_config_descriptor),
		INTERFACE_COUNT,
		1,
		STR_TBL_CONFIG,
		USB_BM_ATTR_RESERVED | USB_BM_ATTR_SELF_POWERED,
		CFG_MAX_POWER
	},
	.interface = {
		sizeof(EFI_USB_INTERFACE_DESCRIPTOR),
		USB_DESC_TYPE_INTERFACE,
		0x0,
		0x0,
		ENDPOINT_COUNT,
		USB_DEVICE_VENDOR_CLASS,
		FB_IF_SUBCLASS,
		FB_IF_PROTOCOL,
		STR_TBL_INTERFACE
	},
	.ep_in = {
		sizeof(EFI_USB_ENDPOINT_DESCRIPTOR),
		USB_DESC_TYPE_ENDPOINT,
		IN_ENDPOINT_NUM | USB_ENDPOINT_DIR_IN,
		USB_ENDPOINT_BULK,
		FB_BULK_EP_PKT_SIZE,
		0x00 /* Not specified for bulk endpoint */
	},
	.ep_out = {
		sizeof(EFI_USB_ENDPOINT_DESCRIPTOR),
		USB_DESC_TYPE_ENDPOINT,
		OUT_ENDPOINT_NUM | USB_ENDPOINT_DIR_OUT,
		USB_ENDPOINT_BULK,
		FB_BULK_EP_PKT_SIZE,
		0x00 /* Not specified for bulk endpoint */
	}
};

static USB_DEVICE_DESCRIPTOR device_descriptor = {
	sizeof(USB_DEVICE_DESCRIPTOR),
	USB_DESC_TYPE_DEVICE,
	USB_BCD_VERSION_HS, /* Default to High Speed */
	0x00, /* specified in interface descriptor */
	0x00, /* specified in interface descriptor */
	0x00, /* specified in interface descriptor */
	USB_EP0_MAX_PKT_SIZE_HS, /* Default to high speed */
	VENDOR_ID,
	PRODUCT_ID,
	BCD_DEVICE,
	STR_TBL_MANUFACTURER,
	STR_TBL_PRODUCT,
	STR_TBL_SERIAL,
	CONFIG_COUNT
};

int usb_write(void *pBuf, uint32_t size)
{
	EFI_STATUS ret;
	USB_DEVICE_IO_REQ ioReq;

	ioReq.EndpointInfo.EndpointDesc = &config_descriptor.ep_in;
	ioReq.EndpointInfo.EndpointCompDesc = NULL;
	ioReq.IoInfo.Buffer = pBuf;
	ioReq.IoInfo.Length = size;

	/* queue the Tx request */
	ret = uefi_call_wrapper(usb_device->EpTxData, 2, usb_device, &ioReq);
	if (EFI_ERROR(ret))
		efi_perror(ret, L"failed to queue Tx request");
	return EFI_ERROR(ret);
}

int usb_read(void *buf, unsigned len)
{
	EFI_STATUS ret;
	USB_DEVICE_IO_REQ ioReq;

	/* WA: usb device stack doesn't accept rx buffer not multiple of MaxPacketSize */
	unsigned max_pkt_size = config_descriptor.ep_out.MaxPacketSize;

	len = ALIGN(len, max_pkt_size);

	ioReq.EndpointInfo.EndpointDesc = &config_descriptor.ep_out;
	ioReq.EndpointInfo.EndpointCompDesc = NULL;
	ioReq.IoInfo.Buffer = buf;
	ioReq.IoInfo.Length = len;

	/* queue the  receive request */
	ret = uefi_call_wrapper(usb_device->EpRxData, 2, usb_device, &ioReq);
	if (EFI_ERROR(ret))
		efi_perror(ret, L"failed to queue Rx request");

	return EFI_ERROR(ret);
}

static EFIAPI EFI_STATUS setup_handler(__attribute__((__unused__)) EFI_USB_DEVICE_REQUEST *CtrlRequest,
				       __attribute__((__unused__)) USB_DEVICE_IO_INFO *IoInfo)
{

	/* Fastboot doesn't handle any Class/Vendor specific setup requests */

	return EFI_SUCCESS;
}

static EFIAPI EFI_STATUS config_handler(UINT8 cfgVal)
{
	EFI_STATUS status = EFI_SUCCESS;

	if (cfgVal == config_descriptor.config.ConfigurationValue) {
		/* we've been configured, get ready to receive Commands */
		if (start_callback)
			start_callback();
	} else {
		error(L"invalid configuration value: 0x%x", cfgVal);
		status = EFI_INVALID_PARAMETER;
	}

	return status;
}

EFIAPI EFI_STATUS data_handler(EFI_USB_DEVICE_XFER_INFO *XferInfo)
{
	if (!XferInfo->Buffer || XferInfo->Length == 0) {
		error(L"Received an unexpected NULL or zero length buffer");
		return EFI_INVALID_PARAMETER;
	}

	/* if we are receiving a command or data, call the processing routine */
	if (XferInfo->EndpointDir == USB_ENDPOINT_DIR_OUT) {
		if (rx_callback)
			rx_callback(XferInfo->Buffer, XferInfo->Length);
	} else
		if (tx_callback)
			tx_callback(XferInfo->Buffer, XferInfo->Length);
	return EFI_SUCCESS;
}

static void fbSetStringTableLine(UINTN line, char *string)
{
	UINTN size;
	CHAR16 *str;

	str = stra_to_str((CHAR8 *)string);
	if (!str) {
		error(L"Failed to convert '%a' to CHAR16 string", string);
		return;
	}

	size = (StrLen(str) + 1) * sizeof(CHAR16);

	if (size > sizeof(string_table[line].LangID)) {
		error(L"String number from SMBIOS table is too long.");
		goto exit;
	}

	memcpy(string_table[line].LangID, str, size);
	string_table[line].Length = size;

exit:
	FreePool(str);
}

static void fbSetManufacturer(void)
{
	char *manufacturer;

	manufacturer = SMBIOS_GET_STRING(2, Manufacturer);
	if (manufacturer == SMBIOS_UNDEFINED) {
		error(L"SMBIOS Manufacturer value unavailable");
		return;
	}

	fbSetStringTableLine(1, manufacturer);
}

static void fbSetProduct(void)
{
	char *product;

	product = SMBIOS_GET_STRING(2, ProductName);
	if (product == SMBIOS_UNDEFINED) {
		error(L"SMBIOS ProductName value unavailable");
		return;
	}

	fbSetStringTableLine(2, product);
}

static void fbSetSerialNumber(void)
{
	char *serial;

	serial = get_serial_number();
	if (!serial) {
		error(L"SMBIOS SerialNumber value unavailable");
		return;
	}

	fbSetStringTableLine(3, serial);
}

static void fbInitDriverObjs(void)
{
	fbSetManufacturer();
	fbSetProduct();
	fbSetSerialNumber();

	/* Device driver objects */
	gDevObj.DeviceDesc                 = &device_descriptor;
	gDevObj.ConfigObjs                 = device_configs;
	gDevObj.StringTable                = string_table;
	gDevObj.StrTblEntries              = STR_TBL_COUNT;
	gDevObj.ConfigCallback             = config_handler;
	gDevObj.SetupCallback              = setup_handler;
	gDevObj.DataCallback               = data_handler;

	/* Config driver objects */
	device_configs[0].ConfigDesc          = &config_descriptor.config;
	device_configs[0].ConfigAll           = &config_descriptor;
	device_configs[0].InterfaceObjs       = &gInterfaceObjs[0];

	/* Interface driver objects */
	gInterfaceObjs[0].InterfaceDesc    = &config_descriptor.interface;
	gInterfaceObjs[0].EndpointObjs     = &gEndpointObjs[0];

	/* Endpoint Data In/Out objects */
	gEndpointObjs[0].EndpointDesc      = &config_descriptor.ep_in;
	gEndpointObjs[0].EndpointCompDesc  = NULL;

	gEndpointObjs[1].EndpointDesc      = &config_descriptor.ep_out;
	gEndpointObjs[1].EndpointCompDesc  = NULL;
}

static EFI_STATUS fastboot_usb_init(void)
{
	EFI_STATUS ret;

	ret = LibLocateProtocol(&gEfiUsbDeviceModeProtocolGuid, (void **)&usb_device);
	if (EFI_ERROR(ret) || !usb_device) {
		error(L"Failed to locate usb device protocol");
		return EFI_ERROR(ret) ? ret : EFI_UNSUPPORTED;
	}
	ret = uefi_call_wrapper(usb_device->InitXdci, 1, usb_device);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Init XDCI failed");
		return ret;
	}

	fbInitDriverObjs();

	/* Bind this Fastboot layer to the USB device driver layer */
	ret = uefi_call_wrapper(usb_device->Bind, 2, usb_device, &gDevObj);
	if (EFI_ERROR(ret)) {
		debug(L"Failed to initialize USB Device driver layer: %r", ret);
		return ret;
	}

	return EFI_SUCCESS;
}

static void *fastboot_bootimage;
static void *fastboot_efiimage;
static UINTN fastboot_imagesize;
static enum boot_target fastboot_target;

EFI_STATUS fastboot_usb_stop(void *bootimage, void *efiimage, UINTN imagesize,
			     enum boot_target target)
{
	EFI_STATUS ret;
	VOID *imgbuffer = NULL;

	fastboot_imagesize = imagesize;
	fastboot_target = target;

	if (imagesize && (bootimage || efiimage)) {
		imgbuffer = AllocatePool(imagesize);
		if (!imgbuffer) {
			error(L"Failed to allocate image buffer");
			return EFI_OUT_OF_RESOURCES;
		}
		memcpy(imgbuffer, bootimage ? bootimage : efiimage, imagesize);
	}

	fastboot_bootimage = bootimage ? imgbuffer : NULL;
	fastboot_efiimage = efiimage ? imgbuffer : NULL;

	ret = uefi_call_wrapper(usb_device->Stop, 1, usb_device);
	if (EFI_ERROR(ret))
		efi_perror(ret, L"Failed to Stop USB", ret);

	return ret;
}

EFI_STATUS fastboot_usb_start(start_callback_t start_cb,
			      data_callback_t rx_cb,
			      data_callback_t tx_cb,
			      void **bootimage,
			      void **efiimage,
			      UINTN *imagesize,
			      enum boot_target *target)
{
	EFI_STATUS ret;

	start_callback = start_cb;
	rx_callback = rx_cb;
	tx_callback = tx_cb;


	/* In case user still holding it from answering a UX prompt
	 * or magic key */
	ui_wait_for_key_release();

	ret = fastboot_usb_init();
	if (EFI_ERROR(ret))
		goto error;

	ret = uefi_call_wrapper(usb_device->Connect, 1, usb_device);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to connect");
		goto error;
	}

	fastboot_bootimage = NULL;
	fastboot_efiimage = NULL;
	fastboot_target = UNKNOWN_TARGET;
	*target = UNKNOWN_TARGET;

	for (;;) {
		*target = fastboot_ui_event_handler();
		if (*target != UNKNOWN_TARGET)
			break;

		ret = uefi_call_wrapper(usb_device->Run, 2, usb_device, 600);
		if (ret == EFI_TIMEOUT)
			continue;

		if (EFI_ERROR(ret)) {
			efi_perror(ret, L"Error occurred during run");
			goto error;
		}

		if (fastboot_target != UNKNOWN_TARGET) {
			*target = fastboot_target;
			break;
		}

		if (fastboot_bootimage || fastboot_efiimage)
			break;
	}

	ret = uefi_call_wrapper(usb_device->DisConnect, 1, usb_device);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to disconnect USB");
		goto error;
	}

	ret = uefi_call_wrapper(usb_device->UnBind, 1, usb_device);
	if (EFI_ERROR(ret)) {
		efi_perror(ret, L"Failed to unbind USB");
		goto error;
	}

	*bootimage = fastboot_bootimage;
	*efiimage = fastboot_efiimage;
	*imagesize = fastboot_imagesize;

	return EFI_SUCCESS;

error:
	return ret;
}

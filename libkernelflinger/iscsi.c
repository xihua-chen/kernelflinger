/*
 * Copyright (c) 2020, Intel Corporation
 * All rights reserved.
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
 * This file defines bootlogic data structures, try to keep it without
 * any external definitions in order to ease export of it.
 */

#include <lib.h>
#include "storage.h"

#include "protocol/NvmExpressHci.h"
#include "protocol/DevicePath.h"
#include "protocol/NvmExpressPassthru.h"


#define ATTR_UNUSED __attribute__((unused))


#define MSG_SCSI_DP               0x02

#include "pci.h"
void  show_device_path(EFI_DEVICE_PATH *p)
{
	if (!p)
		return;

	PCI_DEVICE_PATH *pci;
	if (p == NULL)
		return;

	pci = get_pci_device_path(p);
	if (!pci) {
		Print(L"Boot device is not PCI, unsupported");
		//return EFI_UNSUPPORTED;
	}
	else
	{
		CHAR16 *dps;
		dps = DevicePathToStr((EFI_DEVICE_PATH *)pci);
		Print(L"Dev: '%s'", dps);
		FreePool(dps);
	}

	while (!IsDevicePathEndType(p)) {
		Print(L"   type=%d", DevicePathType(p));
		Print(L", subtype=%d", DevicePathSubType(p));
		Print(L", len=%d", DevicePathNodeLength(p));
		if (p->Type == ACPI_DEVICE_PATH) {	// 2
			if (p->SubType == ACPI_DP) {	// 1
				ACPI_HID_DEVICE_PATH *dp = (ACPI_HID_DEVICE_PATH *)p;
				Print(L"ACPI_DEVICE_PATH/ACPI_DP, HID=%X, UID=%X", dp->HID, dp->UID);
			}
		}

		if (p->Type == HARDWARE_DEVICE_PATH) {	// 1
			if (p->SubType == HW_PCI_DP) {	// 1
				PCI_DEVICE_PATH *dp = (PCI_DEVICE_PATH *)p;
				Print(L"HARDWARE_DEVICE_PATH/HW_PCI_DP, Function=%X, Device=%X", dp->Function, dp->Device);
			}
			if (p->SubType == HW_VENDOR_DP) {	// 1
				//PCI_DEVICE_PATH *dp = (PCI_DEVICE_PATH *)p;
				Print(L"HARDWARE_DEVICE_PATH/HW_VENDOR_DP");
			}

		}

		if (p->Type == MESSAGING_DEVICE_PATH) { // 3
			if(p->SubType == MSG_SCSI_DP) // 2
			{
				SCSI_DEVICE_PATH *dp = (SCSI_DEVICE_PATH *)p;
				Print(L"MESSAGING_DEVICE_PATH/MSG_SCSI_DP, Pun=%X, Lun=%X", dp->Pun, dp->Lun);
			}
		}

		if (p->Type == MEDIA_DEVICE_PATH) {	// 4
			if (p->SubType == MEDIA_HARDDRIVE_DP) {	// 1
				HARDDRIVE_DEVICE_PATH *dp = (HARDDRIVE_DEVICE_PATH *)p;
				Print(L", MEDIA_DEVICE_PATH/MEDIA_HARDDRIVE_DP");
				Print(L", PartitionNumber=%d", dp->PartitionNumber);
				Print(L", PartitionStart=%X", dp->PartitionStart);
				Print(L", PartitionSize=%X", dp->PartitionSize);
			}
		}

		Print(L"\n");
		p = NextDevicePathNode(p);
	}
	return;
}

static void *get_iscsi_device_path(EFI_DEVICE_PATH *p)
{
	for (; !IsDevicePathEndType(p); p = NextDevicePathNode(p)) {
		if (DevicePathType(p) == MESSAGING_DEVICE_PATH
		   && DevicePathSubType(p) == MSG_SCSI_DP)
			return (void *)p;
	}

	return NULL;
}

static EFI_STATUS iscsi_erase_blocks(
	EFI_HANDLE handle ATTR_UNUSED,
	ATTR_UNUSED EFI_BLOCK_IO *bio ATTR_UNUSED,
	EFI_LBA start ATTR_UNUSED,
	EFI_LBA end ATTR_UNUSED
)
{
	return EFI_UNSUPPORTED;
}

static EFI_STATUS iscsi_check_logical_unit(ATTR_UNUSED EFI_DEVICE_PATH *p, ATTR_UNUSED logical_unit_t log_unit)
{
	return log_unit == LOGICAL_UNIT_USER ? EFI_SUCCESS : EFI_UNSUPPORTED;
}

static BOOLEAN is_iscsi(EFI_DEVICE_PATH *p)
{
	Print(L"is_iscsi called\n");
	show_device_path(p);
	return get_iscsi_device_path(p) != NULL;
}

struct storage STORAGE(STORAGE_ISCSI) = {
	.erase_blocks = iscsi_erase_blocks,
	.check_logical_unit = iscsi_check_logical_unit,
	.probe = is_iscsi,
	.name = L"ISCSI"
};



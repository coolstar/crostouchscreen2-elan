#define DESCRIPTOR_DEF
#include "elan.h"

static ULONG ElanDebugLevel = 100;
static ULONG ElanDebugCatagories = DBG_INIT || DBG_PNP || DBG_IOCTL;

NTSTATUS
DriverEntry(
	__in PDRIVER_OBJECT  DriverObject,
	__in PUNICODE_STRING RegistryPath
)
{
	NTSTATUS               status = STATUS_SUCCESS;
	WDF_DRIVER_CONFIG      config;
	WDF_OBJECT_ATTRIBUTES  attributes;

	ElanPrint(DEBUG_LEVEL_INFO, DBG_INIT,
		"Driver Entry\n");

	WDF_DRIVER_CONFIG_INIT(&config, ElanEvtDeviceAdd);

	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

	//
	// Create a framework driver object to represent our driver.
	//

	status = WdfDriverCreate(DriverObject,
		RegistryPath,
		&attributes,
		&config,
		WDF_NO_HANDLE
	);

	if (!NT_SUCCESS(status))
	{
		ElanPrint(DEBUG_LEVEL_ERROR, DBG_INIT,
			"WdfDriverCreate failed with status 0x%x\n", status);
	}

	return status;
}

static NTSTATUS elants_i2c_send(PELAN_CONTEXT pDevice, uint8_t *data, size_t size) {
	return SpbWriteDataSynchronously(&pDevice->I2CContext, data, (ULONG)size);
}

static NTSTATUS elants_i2c_read(PELAN_CONTEXT pDevice, uint8_t *data, size_t size) {
	return SpbReadDataSynchronously(&pDevice->I2CContext, data, (ULONG)size);
}

static NTSTATUS elants_i2c_execute_command(PELAN_CONTEXT pDevice, uint8_t* cmd, size_t cmd_size,
	uint8_t* resp, size_t resp_size, const char* cmd_name) {
	uint8_t expected_response;

	switch (cmd[0]) {
	case CMD_HEADER_READ:
		expected_response = CMD_HEADER_RESP;
		break;

	case CMD_HEADER_6B_READ:
		expected_response = CMD_HEADER_6B_RESP;
		break;

	case CMD_HEADER_ROM_READ:
		expected_response = CMD_HEADER_ROM_RESP;
		break;

	default:
		ElanPrint(DEBUG_LEVEL_ERROR, DBG_INIT, "(%s): invalid command: %s\n",
			cmd_name);
		return STATUS_INVALID_PARAMETER;
	}

	NTSTATUS status = SpbXferDataSynchronously(&pDevice->I2CContext, cmd, cmd_size, resp, resp_size);
	if (status != STATUS_SUCCESS) {
		return status;
	}

	if (resp[FW_HDR_TYPE] != expected_response) {
		ElanPrint(DEBUG_LEVEL_ERROR, DBG_INIT, "unexpected response: %s (0x%x)\n", cmd_name, resp[FW_HDR_TYPE]);
		return STATUS_DEVICE_DATA_ERROR;
	}
	return status;
}

NTSTATUS BOOTTOUCHSCREEN(
	_In_  PELAN_CONTEXT  devContext
)
{
	NTSTATUS status = STATUS_SUCCESS;

	LARGE_INTEGER delay;
	if (!devContext->TouchScreenBooted) {
#define MAX_RETRIES 3
		for (int retries = 0; retries < MAX_RETRIES; retries++) {
			ElanPrint(DEBUG_LEVEL_ERROR, DBG_INIT, "Initializing... (attempt %d)\n", retries);
			uint8_t soft_rst_cmd[] = { 0x77, 0x77, 0x77, 0x77 };
			status = elants_i2c_send(devContext, soft_rst_cmd, sizeof(soft_rst_cmd));

			delay.QuadPart = -30 * 10;
			KeDelayExecutionThread(KernelMode, FALSE, &delay);

			if (!NT_SUCCESS(status)) {
				ElanPrint(DEBUG_LEVEL_ERROR, DBG_INIT, "Unable to send soft reset\n");
				if (retries < MAX_RETRIES - 1) {
					continue;
				}
				else {
					break;
				}
			}

			for (int bootretries = 0; bootretries < MAX_RETRIES; bootretries++){
				ElanPrint(DEBUG_LEVEL_ERROR, DBG_INIT, "Booting... (attempt %d)\n", bootretries);
				uint8_t boot_cmd[] = { 0x4D, 0x61, 0x69, 0x6E };
				status = elants_i2c_send(devContext, boot_cmd, sizeof(boot_cmd));

				delay.QuadPart = -1 * BOOT_TIME_DELAY_MS * 10;
				KeDelayExecutionThread(KernelMode, FALSE, &delay);

				if (!NT_SUCCESS(status)) {
					ElanPrint(DEBUG_LEVEL_ERROR, DBG_INIT, "Unable to send boot cmd\n");
					if (bootretries < MAX_RETRIES - 1) {
						continue;
					}
					else {
						break;
					}
				}

				//Get Hello Packet
				uint8_t hello_packet[] = { 0x55, 0x55, 0x55, 0x55 };

				uint8_t buf[HEADER_SIZE];
				status = elants_i2c_read(devContext, buf, sizeof(buf));
				if (status != STATUS_SUCCESS) {
					ElanPrint(DEBUG_LEVEL_ERROR, DBG_INIT, "Unable to read hello packet!\n");
					if (bootretries < MAX_RETRIES - 1) {
						continue;
					}
					else {
						break;
					}
				}

				if (memcmp(buf, hello_packet, sizeof(hello_packet))) {
					ElanPrint(DEBUG_LEVEL_ERROR, DBG_INIT, "Failed to get hello packet! Got: 0x%x 0x%x 0x%x 0x%x\n", buf[0], buf[1], buf[2], buf[3]);
					if (bootretries < MAX_RETRIES - 1) {
						continue;
					}
					else {
						DbgPrint("Warning: Allow malformed hello packet\n");
						break;
					}
				}
			}
		}
		if (!NT_SUCCESS(status)) {
			return status;
		}

		//Begin EKTH3500 query
		uint8_t resp[17];
		uint16_t phy_x, phy_y, rows, cols, osr;
		uint8_t get_resolution_cmd[] = {
			CMD_HEADER_6B_READ, 0x00, 0x00, 0x00, 0x00, 0x00
		};
		uint8_t get_osr_cmd[] = {
			CMD_HEADER_READ, E_INFO_OSR, 0x00, 0x01
		};
		uint8_t get_physical_scan_cmd[] = {
			CMD_HEADER_READ, E_INFO_PHY_SCAN, 0x00, 0x01
		};
		uint8_t get_physical_drive_cmd[] = {
			CMD_HEADER_READ, E_INFO_PHY_DRIVER, 0x00, 0x01
		};
		status = elants_i2c_execute_command(devContext, get_resolution_cmd, sizeof(get_resolution_cmd), resp, sizeof(resp), "get resolution");
		if (status != STATUS_SUCCESS) {
			ElanPrint(DEBUG_LEVEL_ERROR, DBG_INIT, "Unable to get resolution!\n");
			return status;
		}
		rows = resp[2] + resp[6] + resp[10];
		cols = resp[3] + resp[7] + resp[11];

		status = elants_i2c_execute_command(devContext, get_osr_cmd, sizeof(get_osr_cmd), resp, sizeof(resp), "get osr");
		if (status != STATUS_SUCCESS) {
			ElanPrint(DEBUG_LEVEL_ERROR, DBG_INIT, "Unable to get osr!\n");
			return status;
		}

		osr = resp[3];

		status = elants_i2c_execute_command(devContext, get_physical_scan_cmd, sizeof(get_physical_scan_cmd), resp, sizeof(resp), "get physical scan");
		if (status != STATUS_SUCCESS) {
			ElanPrint(DEBUG_LEVEL_ERROR, DBG_INIT, "Unable to get physical scan!\n");
			return status;
		}

		phy_x = (resp[2] << 8) | resp[3];

		status = elants_i2c_execute_command(devContext, get_physical_drive_cmd, sizeof(get_physical_drive_cmd), resp, sizeof(resp), "get physical drive");
		if (status != STATUS_SUCCESS) {
			ElanPrint(DEBUG_LEVEL_ERROR, DBG_INIT, "Unable to get physical drive!\n");
			return status;
		}

		phy_y = (resp[2] << 8) | resp[3];

		devContext->max_x = ELAN_TS_RESOLUTION(rows, osr);
		devContext->max_y = ELAN_TS_RESOLUTION(cols, osr);

		devContext->max_x_hid[0] = devContext->max_x;
		devContext->max_x_hid[1] = devContext->max_x >> 8;

		devContext->max_y_hid[0] = devContext->max_y;
		devContext->max_y_hid[1] = devContext->max_y >> 8;

		ElanPrint(DEBUG_LEVEL_INFO, DBG_INIT, "max x: %d, max y: %d, phy x: %d, phy y: %d\n", devContext->max_x, devContext->max_y, phy_x, phy_y);

		uint8_t soft_rst_cmd[] = { 0x77, 0x77, 0x77, 0x77 };
		status = elants_i2c_send(devContext, soft_rst_cmd, sizeof(soft_rst_cmd));
		if (!NT_SUCCESS(status)) {
			ElanPrint(DEBUG_LEVEL_ERROR, DBG_INIT, "Unable to send soft reset\n");
			return status;
		}

		devContext->TouchScreenBooted = true;
	}
	return status;
}

NTSTATUS
OnPrepareHardware(
	_In_  WDFDEVICE     FxDevice,
	_In_  WDFCMRESLIST  FxResourcesRaw,
	_In_  WDFCMRESLIST  FxResourcesTranslated
)
/*++

Routine Description:

This routine caches the SPB resource connection ID.

Arguments:

FxDevice - a handle to the framework device object
FxResourcesRaw - list of translated hardware resources that
the PnP manager has assigned to the device
FxResourcesTranslated - list of raw hardware resources that
the PnP manager has assigned to the device

Return Value:

Status

--*/
{
	PELAN_CONTEXT pDevice = GetDeviceContext(FxDevice);
	BOOLEAN fSpbResourceFound = FALSE;
	NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;

	UNREFERENCED_PARAMETER(FxResourcesRaw);

	//
	// Parse the peripheral's resources.
	//

	ULONG resourceCount = WdfCmResourceListGetCount(FxResourcesTranslated);

	for (ULONG i = 0; i < resourceCount; i++)
	{
		PCM_PARTIAL_RESOURCE_DESCRIPTOR pDescriptor;
		UCHAR Class;
		UCHAR Type;

		pDescriptor = WdfCmResourceListGetDescriptor(
			FxResourcesTranslated, i);

		switch (pDescriptor->Type)
		{
		case CmResourceTypeConnection:
			//
			// Look for I2C or SPI resource and save connection ID.
			//
			Class = pDescriptor->u.Connection.Class;
			Type = pDescriptor->u.Connection.Type;
			if (Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
				Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C)
			{
				if (fSpbResourceFound == FALSE)
				{
					status = STATUS_SUCCESS;
					pDevice->I2CContext.I2cResHubId.LowPart = pDescriptor->u.Connection.IdLowPart;
					pDevice->I2CContext.I2cResHubId.HighPart = pDescriptor->u.Connection.IdHighPart;
					fSpbResourceFound = TRUE;
				}
				else
				{
				}
			}
			break;
		default:
			//
			// Ignoring all other resource types.
			//
			break;
		}
	}

	//
	// An SPB resource is required.
	//

	if (fSpbResourceFound == FALSE)
	{
		status = STATUS_NOT_FOUND;
	}

	status = SpbTargetInitialize(FxDevice, &pDevice->I2CContext);

	if (!NT_SUCCESS(status))
	{
		return status;
	}

	status = BOOTTOUCHSCREEN(pDevice);

	if (!NT_SUCCESS(status))
	{
		return status;
	}

	return status;
}

NTSTATUS
OnReleaseHardware(
	_In_  WDFDEVICE     FxDevice,
	_In_  WDFCMRESLIST  FxResourcesTranslated
)
/*++

Routine Description:

Arguments:

FxDevice - a handle to the framework device object
FxResourcesTranslated - list of raw hardware resources that
the PnP manager has assigned to the device

Return Value:

Status

--*/
{
	PELAN_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status = STATUS_SUCCESS;

	UNREFERENCED_PARAMETER(FxResourcesTranslated);

	SpbTargetDeinitialize(FxDevice, &pDevice->I2CContext);

	return status;
}

NTSTATUS
OnD0Entry(
	_In_  WDFDEVICE               FxDevice,
	_In_  WDF_POWER_DEVICE_STATE  FxPreviousState
)
/*++

Routine Description:

This routine allocates objects needed by the driver.

Arguments:

FxDevice - a handle to the framework device object
FxPreviousState - previous power state

Return Value:

Status

--*/
{
	UNREFERENCED_PARAMETER(FxPreviousState);

	PELAN_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status = STATUS_SUCCESS;

	if (!pDevice->TouchScreenBooted) {
		status = BOOTTOUCHSCREEN(pDevice);
		if (status != STATUS_SUCCESS) {
			return status;
		}
	}

	for (int i = 0; i < 20; i++) {
		pDevice->Flags[i] = 0;
	}

	pDevice->RegsSet = false;
	pDevice->ConnectInterrupt = true;

	return status;
}

NTSTATUS
OnD0Exit(
	_In_  WDFDEVICE               FxDevice,
	_In_  WDF_POWER_DEVICE_STATE  FxPreviousState
)
/*++

Routine Description:

This routine destroys objects needed by the driver.

Arguments:

FxDevice - a handle to the framework device object
FxPreviousState - previous power state

Return Value:

Status

--*/
{
	UNREFERENCED_PARAMETER(FxPreviousState);

	PELAN_CONTEXT pDevice = GetDeviceContext(FxDevice);

	pDevice->ConnectInterrupt = false;
	pDevice->TouchScreenBooted = false;

	return STATUS_SUCCESS;
}

void ElanProcessInput(PELAN_CONTEXT pDevice) {
	struct _ELAN_MULTITOUCH_REPORT report;
	report.ReportID = REPORTID_MTOUCH;

	int count = 0, i = 0;
	while (count < 10 && i < 20) {
		if (pDevice->Flags[i] != 0) {
			report.Touch[count].ContactID = i;
			report.Touch[count].Height = pDevice->AREA[i];
			report.Touch[count].Width = pDevice->AREA[i];

			report.Touch[count].XValue = pDevice->XValue[i];
			report.Touch[count].YValue = pDevice->YValue[i];

			uint8_t flags = pDevice->Flags[i];
			if (flags & MXT_T9_DETECT) {
				report.Touch[count].Status = MULTI_CONFIDENCE_BIT | MULTI_TIPSWITCH_BIT;
			}
			else if (flags & MXT_T9_PRESS) {
				report.Touch[count].Status = MULTI_CONFIDENCE_BIT | MULTI_TIPSWITCH_BIT;
			}
			else if (flags & MXT_T9_RELEASE) {
				report.Touch[count].Status = MULTI_CONFIDENCE_BIT;
				pDevice->Flags[i] = 0;
			}
			else
				report.Touch[count].Status = 0;

			count++;
		}
		i++;
	}

	report.ActualCount = count;

	if (count > 0) {
		size_t bytesWritten;
		ElanProcessVendorReport(pDevice, &report, sizeof(report), &bytesWritten);
	}
}

static void elants_i2c_mt_event(PELAN_CONTEXT pDevice, uint8_t *buf) {
	unsigned int n_fingers;
	uint16_t finger_state;

	n_fingers = buf[FW_POS_STATE + 1] & 0x0f;
	finger_state = ((buf[FW_POS_STATE + 1] & 0x30) << 4) |
		buf[FW_POS_STATE];

	for (int i = 0; i < MAX_CONTACT_NUM; i++) {
		if (finger_state & 1) {
			unsigned int x, y, p, w;

			uint8_t *pos;

			pos = &buf[FW_POS_XY + i * 3];
			x = (((uint16_t)pos[0] & 0xf0) << 4) | pos[1];
			y = (((uint16_t)pos[0] & 0x0f) << 8) | pos[2];
			p = buf[FW_POS_PRESSURE + i];
			w = buf[FW_POS_WIDTH + i];

			ElanPrint(DEBUG_LEVEL_INFO, DBG_IOCTL, "i=%d x=%d y=%d p=%d w=%d\n",
				i, x, y, p, w);

			pDevice->Flags[i] = MXT_T9_DETECT;
			pDevice->XValue[i] = x;
			pDevice->YValue[i] = y;
			pDevice->AREA[i] = w;

			n_fingers--;
		}
		else if (pDevice->Flags[i] == MXT_T9_DETECT) {
			pDevice->Flags[i] = MXT_T9_RELEASE;
		}
		finger_state >>= 1;
	}

	ElanProcessInput(pDevice);
}

static uint8_t elants_i2c_calculate_checksum(uint8_t *buf)
{
	uint8_t checksum = 0;
	uint8_t i;

	for (i = 0; i < FW_POS_CHECKSUM; i++)
		checksum += buf[i];

	return checksum;
}

static void elants_i2c_event(PELAN_CONTEXT pDevice, uint8_t *buf) {
	uint8_t checksum = elants_i2c_calculate_checksum(buf);

	if (buf[FW_POS_CHECKSUM] != checksum) {
		ElanPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL, "invalid checksum for packet 0x02x: %02x vs. %02x\n", buf[FW_POS_HEADER], checksum, buf[FW_POS_CHECKSUM]);
	}
	else if (buf[FW_POS_HEADER] != HEADER_REPORT_10_FINGER) {
		ElanPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL, "unknown packet type: %02x\n", buf[FW_POS_HEADER]);
	}
	else
		elants_i2c_mt_event(pDevice, buf);
}

BOOLEAN OnInterruptIsr(
	WDFINTERRUPT Interrupt,
	ULONG MessageID) {
	UNREFERENCED_PARAMETER(MessageID);

	WDFDEVICE Device = WdfInterruptGetDevice(Interrupt);
	PELAN_CONTEXT pDevice = GetDeviceContext(Device);

	if (!pDevice->ConnectInterrupt)
		return false;

	if (!pDevice->TouchScreenBooted)
		return false;

	uint8_t buf[MAX_PACKET_SIZE];
	NTSTATUS status = elants_i2c_read(pDevice, buf, sizeof(buf));
	if (!NT_SUCCESS(status)) {
		return false;
	}
	
	switch (buf[FW_HDR_TYPE]) {
	case QUEUE_HEADER_SINGLE:
		elants_i2c_event(pDevice, &buf[HEADER_SIZE]);
		break;
	case QUEUE_HEADER_NORMAL:
		int report_count = buf[FW_HDR_COUNT];
		if (report_count == 0 || report_count > 3) {
			ElanPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL, "bad report count: %d\n", report_count);
			break;
		}

		int report_len = buf[FW_HDR_LENGTH] / report_count;
		if (report_len != PACKET_SIZE) {
			ElanPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL, "mismatching report length: %d\n", report_len);
			break;
		}

		for (int i = 0; i < report_count; i++) {
			uint8_t *newbuf = buf + HEADER_SIZE + i * PACKET_SIZE;
			elants_i2c_event(pDevice, newbuf);
		}

		break;
	}

	return true;
}

NTSTATUS
ElanEvtDeviceAdd(
	IN WDFDRIVER       Driver,
	IN PWDFDEVICE_INIT DeviceInit
)
{
	NTSTATUS                      status = STATUS_SUCCESS;
	WDF_IO_QUEUE_CONFIG           queueConfig;
	WDF_OBJECT_ATTRIBUTES         attributes;
	WDFDEVICE                     device;
	WDF_INTERRUPT_CONFIG interruptConfig;
	WDFQUEUE                      queue;
	UCHAR                         minorFunction;
	PELAN_CONTEXT               devContext;

	UNREFERENCED_PARAMETER(Driver);

	PAGED_CODE();

	ElanPrint(DEBUG_LEVEL_INFO, DBG_PNP,
		"ElanEvtDeviceAdd called\n");

	//
	// Tell framework this is a filter driver. Filter drivers by default are  
	// not power policy owners. This works well for this driver because
	// HIDclass driver is the power policy owner for HID minidrivers.
	//

	WdfFdoInitSetFilter(DeviceInit);

	{
		WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
		WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);

		pnpCallbacks.EvtDevicePrepareHardware = OnPrepareHardware;
		pnpCallbacks.EvtDeviceReleaseHardware = OnReleaseHardware;
		pnpCallbacks.EvtDeviceD0Entry = OnD0Entry;
		pnpCallbacks.EvtDeviceD0Exit = OnD0Exit;

		WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);
	}

	//
	// Setup the device context
	//

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, ELAN_CONTEXT);

	//
	// Create a framework device object.This call will in turn create
	// a WDM device object, attach to the lower stack, and set the
	// appropriate flags and attributes.
	//

	status = WdfDeviceCreate(&DeviceInit, &attributes, &device);

	if (!NT_SUCCESS(status))
	{
		ElanPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfDeviceCreate failed with status code 0x%x\n", status);

		return status;
	}

	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);

	queueConfig.EvtIoInternalDeviceControl = ElanEvtInternalDeviceControl;

	status = WdfIoQueueCreate(device,
		&queueConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&queue
	);

	if (!NT_SUCCESS(status))
	{
		ElanPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfIoQueueCreate failed 0x%x\n", status);

		return status;
	}

	//
	// Create manual I/O queue to take care of hid report read requests
	//

	devContext = GetDeviceContext(device);

	devContext->TouchScreenBooted = false;

	devContext->FxDevice = device;

	WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);

	queueConfig.PowerManaged = WdfFalse;

	status = WdfIoQueueCreate(device,
		&queueConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&devContext->ReportQueue
	);

	if (!NT_SUCCESS(status))
	{
		ElanPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfIoQueueCreate failed 0x%x\n", status);

		return status;
	}

	//
	// Create an interrupt object for hardware notifications
	//
	WDF_INTERRUPT_CONFIG_INIT(
		&interruptConfig,
		OnInterruptIsr,
		NULL);
	interruptConfig.PassiveHandling = TRUE;

	status = WdfInterruptCreate(
		device,
		&interruptConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&devContext->Interrupt);

	if (!NT_SUCCESS(status))
	{
		ElanPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"Error creating WDF interrupt object - %!STATUS!",
			status);

		return status;
	}

	//
	// Initialize DeviceMode
	//

	devContext->DeviceMode = DEVICE_MODE_MOUSE;

	return status;
}

VOID
ElanEvtInternalDeviceControl(
	IN WDFQUEUE     Queue,
	IN WDFREQUEST   Request,
	IN size_t       OutputBufferLength,
	IN size_t       InputBufferLength,
	IN ULONG        IoControlCode
)
{
	NTSTATUS            status = STATUS_SUCCESS;
	WDFDEVICE           device;
	PELAN_CONTEXT     devContext;
	BOOLEAN             completeRequest = TRUE;

	UNREFERENCED_PARAMETER(OutputBufferLength);
	UNREFERENCED_PARAMETER(InputBufferLength);

	device = WdfIoQueueGetDevice(Queue);
	devContext = GetDeviceContext(device);

	ElanPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
		"%s, Queue:0x%p, Request:0x%p\n",
		DbgHidInternalIoctlString(IoControlCode),
		Queue,
		Request
	);

	//
	// Please note that HIDCLASS provides the buffer in the Irp->UserBuffer
	// field irrespective of the ioctl buffer type. However, framework is very
	// strict about type checking. You cannot get Irp->UserBuffer by using
	// WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
	// internal ioctl. So depending on the ioctl code, we will either
	// use retreive function or escape to WDM to get the UserBuffer.
	//

	switch (IoControlCode)
	{

	case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
		//
		// Retrieves the device's HID descriptor.
		//
		status = ElanGetHidDescriptor(device, Request);
		break;

	case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
		//
		//Retrieves a device's attributes in a HID_DEVICE_ATTRIBUTES structure.
		//
		status = ElanGetDeviceAttributes(Request);
		break;

	case IOCTL_HID_GET_REPORT_DESCRIPTOR:
		//
		//Obtains the report descriptor for the HID device.
		//
		status = ElanGetReportDescriptor(device, Request);
		break;

	case IOCTL_HID_GET_STRING:
		//
		// Requests that the HID minidriver retrieve a human-readable string
		// for either the manufacturer ID, the product ID, or the serial number
		// from the string descriptor of the device. The minidriver must send
		// a Get String Descriptor request to the device, in order to retrieve
		// the string descriptor, then it must extract the string at the
		// appropriate index from the string descriptor and return it in the
		// output buffer indicated by the IRP. Before sending the Get String
		// Descriptor request, the minidriver must retrieve the appropriate
		// index for the manufacturer ID, the product ID or the serial number
		// from the device extension of a top level collection associated with
		// the device.
		//
		status = ElanGetString(Request);
		break;

	case IOCTL_HID_WRITE_REPORT:
	case IOCTL_HID_SET_OUTPUT_REPORT:
		//
		//Transmits a class driver-supplied report to the device.
		//
		status = ElanWriteReport(devContext, Request);
		break;

	case IOCTL_HID_READ_REPORT:
	case IOCTL_HID_GET_INPUT_REPORT:
		//
		// Returns a report from the device into a class driver-supplied buffer.
		// 
		status = ElanReadReport(devContext, Request, &completeRequest);
		break;

	case IOCTL_HID_SET_FEATURE:
		//
		// This sends a HID class feature report to a top-level collection of
		// a HID class device.
		//
		status = ElanSetFeature(devContext, Request, &completeRequest);
		break;

	case IOCTL_HID_GET_FEATURE:
		//
		// returns a feature report associated with a top-level collection
		//
		status = ElanGetFeature(devContext, Request, &completeRequest);
		break;

	case IOCTL_HID_ACTIVATE_DEVICE:
		//
		// Makes the device ready for I/O operations.
		//
	case IOCTL_HID_DEACTIVATE_DEVICE:
		//
		// Causes the device to cease operations and terminate all outstanding
		// I/O requests.
		//
	default:
		status = STATUS_NOT_SUPPORTED;
		break;
	}

	if (completeRequest)
	{
		WdfRequestComplete(Request, status);

		ElanPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"%s completed, Queue:0x%p, Request:0x%p\n",
			DbgHidInternalIoctlString(IoControlCode),
			Queue,
			Request
		);
	}
	else
	{
		ElanPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"%s deferred, Queue:0x%p, Request:0x%p\n",
			DbgHidInternalIoctlString(IoControlCode),
			Queue,
			Request
		);
	}

	return;
}

NTSTATUS
ElanGetHidDescriptor(
	IN WDFDEVICE Device,
	IN WDFREQUEST Request
)
{
	NTSTATUS            status = STATUS_SUCCESS;
	size_t              bytesToCopy = 0;
	WDFMEMORY           memory;

	UNREFERENCED_PARAMETER(Device);

	ElanPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"ElanGetHidDescriptor Entry\n");

	//
	// This IOCTL is METHOD_NEITHER so WdfRequestRetrieveOutputMemory
	// will correctly retrieve buffer from Irp->UserBuffer. 
	// Remember that HIDCLASS provides the buffer in the Irp->UserBuffer
	// field irrespective of the ioctl buffer type. However, framework is very
	// strict about type checking. You cannot get Irp->UserBuffer by using
	// WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
	// internal ioctl.
	//
	status = WdfRequestRetrieveOutputMemory(Request, &memory);

	if (!NT_SUCCESS(status))
	{
		ElanPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfRequestRetrieveOutputMemory failed 0x%x\n", status);

		return status;
	}

	//
	// Use hardcoded "HID Descriptor" 
	//
	bytesToCopy = DefaultHidDescriptor.bLength;

	if (bytesToCopy == 0)
	{
		status = STATUS_INVALID_DEVICE_STATE;

		ElanPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"DefaultHidDescriptor is zero, 0x%x\n", status);

		return status;
	}

	status = WdfMemoryCopyFromBuffer(memory,
		0, // Offset
		(PVOID)&DefaultHidDescriptor,
		bytesToCopy);

	if (!NT_SUCCESS(status))
	{
		ElanPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfMemoryCopyFromBuffer failed 0x%x\n", status);

		return status;
	}

	//
	// Report how many bytes were copied
	//
	WdfRequestSetInformation(Request, bytesToCopy);

	ElanPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"ElanGetHidDescriptor Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
ElanGetReportDescriptor(
	IN WDFDEVICE Device,
	IN WDFREQUEST Request
)
{
	NTSTATUS            status = STATUS_SUCCESS;
	ULONG_PTR           bytesToCopy;
	WDFMEMORY           memory;

	PELAN_CONTEXT devContext = GetDeviceContext(Device);

	UNREFERENCED_PARAMETER(Device);

	ElanPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"ElanGetReportDescriptor Entry\n");

#define MT_TOUCH_COLLECTION												\
			MT_TOUCH_COLLECTION0 \
			0x26, devContext->max_x_hid[0], devContext->max_x_hid[1],                   /*       LOGICAL_MAXIMUM (WIDTH)    */ \
			MT_TOUCH_COLLECTION1 \
			0x26, devContext->max_y_hid[0], devContext->max_y_hid[1],                   /*       LOGICAL_MAXIMUM (HEIGHT)    */ \
			MT_TOUCH_COLLECTION2 \

	HID_REPORT_DESCRIPTOR ReportDescriptor[] = {
		//
		// Multitouch report starts here
		//
		0x05, 0x0d,                         // USAGE_PAGE (Digitizers)
		0x09, 0x04,                         // USAGE (Touch Screen)
		0xa1, 0x01,                         // COLLECTION (Application)
		0x85, REPORTID_MTOUCH,              //   REPORT_ID (Touch)
		0x09, 0x22,                         //   USAGE (Finger)
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		USAGE_PAGE
		0xc0,                               // END_COLLECTION
	};

	//
	// This IOCTL is METHOD_NEITHER so WdfRequestRetrieveOutputMemory
	// will correctly retrieve buffer from Irp->UserBuffer. 
	// Remember that HIDCLASS provides the buffer in the Irp->UserBuffer
	// field irrespective of the ioctl buffer type. However, framework is very
	// strict about type checking. You cannot get Irp->UserBuffer by using
	// WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
	// internal ioctl.
	//
	status = WdfRequestRetrieveOutputMemory(Request, &memory);
	if (!NT_SUCCESS(status))
	{
		ElanPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfRequestRetrieveOutputMemory failed 0x%x\n", status);

		return status;
	}

	//
	// Use hardcoded Report descriptor
	//
	bytesToCopy = DefaultHidDescriptor.DescriptorList[0].wReportLength;

	if (bytesToCopy == 0)
	{
		status = STATUS_INVALID_DEVICE_STATE;

		ElanPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"DefaultHidDescriptor's reportLength is zero, 0x%x\n", status);

		return status;
	}

	status = WdfMemoryCopyFromBuffer(memory,
		0,
		(PVOID)ReportDescriptor,
		bytesToCopy);
	if (!NT_SUCCESS(status))
	{
		ElanPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfMemoryCopyFromBuffer failed 0x%x\n", status);

		return status;
	}

	//
	// Report how many bytes were copied
	//
	WdfRequestSetInformation(Request, bytesToCopy);

	ElanPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"ElanGetReportDescriptor Exit = 0x%x\n", status);

	return status;
}


NTSTATUS
ElanGetDeviceAttributes(
	IN WDFREQUEST Request
)
{
	NTSTATUS                 status = STATUS_SUCCESS;
	PHID_DEVICE_ATTRIBUTES   deviceAttributes = NULL;

	ElanPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"ElanGetDeviceAttributes Entry\n");

	//
	// This IOCTL is METHOD_NEITHER so WdfRequestRetrieveOutputMemory
	// will correctly retrieve buffer from Irp->UserBuffer. 
	// Remember that HIDCLASS provides the buffer in the Irp->UserBuffer
	// field irrespective of the ioctl buffer type. However, framework is very
	// strict about type checking. You cannot get Irp->UserBuffer by using
	// WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
	// internal ioctl.
	//
	status = WdfRequestRetrieveOutputBuffer(Request,
		sizeof(HID_DEVICE_ATTRIBUTES),
		(PVOID *)&deviceAttributes,
		NULL);
	if (!NT_SUCCESS(status))
	{
		ElanPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfRequestRetrieveOutputBuffer failed 0x%x\n", status);

		return status;
	}

	//
	// Set USB device descriptor
	//

	deviceAttributes->Size = sizeof(HID_DEVICE_ATTRIBUTES);
	deviceAttributes->VendorID = ELAN_VID;
	deviceAttributes->ProductID = ELAN_PID;
	deviceAttributes->VersionNumber = ELAN_VERSION;

	//
	// Report how many bytes were copied
	//
	WdfRequestSetInformation(Request, sizeof(HID_DEVICE_ATTRIBUTES));

	ElanPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"ElanGetDeviceAttributes Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
ElanGetString(
	IN WDFREQUEST Request
)
{

	NTSTATUS status = STATUS_SUCCESS;
	PWSTR pwstrID;
	size_t lenID;
	WDF_REQUEST_PARAMETERS params;
	void *pStringBuffer = NULL;

	ElanPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"ElanGetString Entry\n");

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	switch ((ULONG_PTR)params.Parameters.DeviceIoControl.Type3InputBuffer & 0xFFFF)
	{
	case HID_STRING_ID_IMANUFACTURER:
		pwstrID = L"Elan.\0";
		break;

	case HID_STRING_ID_IPRODUCT:
		pwstrID = L"MaxTouch Touch Screen\0";
		break;

	case HID_STRING_ID_ISERIALNUMBER:
		pwstrID = L"123123123\0";
		break;

	default:
		pwstrID = NULL;
		break;
	}

	lenID = pwstrID ? wcslen(pwstrID) * sizeof(WCHAR) + sizeof(UNICODE_NULL) : 0;

	if (pwstrID == NULL)
	{

		ElanPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"ElanGetString Invalid request type\n");

		status = STATUS_INVALID_PARAMETER;

		return status;
	}

	status = WdfRequestRetrieveOutputBuffer(Request,
		lenID,
		&pStringBuffer,
		&lenID);

	if (!NT_SUCCESS(status))
	{

		ElanPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"ElanGetString WdfRequestRetrieveOutputBuffer failed Status 0x%x\n", status);

		return status;
	}

	RtlCopyMemory(pStringBuffer, pwstrID, lenID);

	WdfRequestSetInformation(Request, lenID);

	ElanPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"ElanGetString Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
ElanWriteReport(
	IN PELAN_CONTEXT DevContext,
	IN WDFREQUEST Request
)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDF_REQUEST_PARAMETERS params;
	PHID_XFER_PACKET transferPacket = NULL;

	ElanPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"ElanWriteReport Entry\n");

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	if (params.Parameters.DeviceIoControl.InputBufferLength < sizeof(HID_XFER_PACKET))
	{
		ElanPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"ElanWriteReport Xfer packet too small\n");

		status = STATUS_BUFFER_TOO_SMALL;
	}
	else
	{

		transferPacket = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;

		if (transferPacket == NULL)
		{
			ElanPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"ElanWriteReport No xfer packet\n");

			status = STATUS_INVALID_DEVICE_REQUEST;
		}
		else
		{
			//
			// switch on the report id
			//

			switch (transferPacket->reportId)
			{
			default:

				ElanPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
					"ElanWriteReport Unhandled report type %d\n", transferPacket->reportId);

				status = STATUS_INVALID_PARAMETER;

				break;
			}
		}
	}

	ElanPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"ElanWriteReport Exit = 0x%x\n", status);

	return status;

}

NTSTATUS
ElanProcessVendorReport(
	IN PELAN_CONTEXT DevContext,
	IN PVOID ReportBuffer,
	IN ULONG ReportBufferLen,
	OUT size_t* BytesWritten
)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDFREQUEST reqRead;
	PVOID pReadReport = NULL;
	size_t bytesReturned = 0;

	ElanPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"ElanProcessVendorReport Entry\n");

	status = WdfIoQueueRetrieveNextRequest(DevContext->ReportQueue,
		&reqRead);

	if (NT_SUCCESS(status))
	{
		status = WdfRequestRetrieveOutputBuffer(reqRead,
			ReportBufferLen,
			&pReadReport,
			&bytesReturned);

		if (NT_SUCCESS(status))
		{
			//
			// Copy ReportBuffer into read request
			//

			if (bytesReturned > ReportBufferLen)
			{
				bytesReturned = ReportBufferLen;
			}

			RtlCopyMemory(pReadReport,
				ReportBuffer,
				bytesReturned);

			//
			// Complete read with the number of bytes returned as info
			//

			WdfRequestCompleteWithInformation(reqRead,
				status,
				bytesReturned);

			ElanPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
				"ElanProcessVendorReport %d bytes returned\n", bytesReturned);

			//
			// Return the number of bytes written for the write request completion
			//

			*BytesWritten = bytesReturned;

			ElanPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
				"%s completed, Queue:0x%p, Request:0x%p\n",
				DbgHidInternalIoctlString(IOCTL_HID_READ_REPORT),
				DevContext->ReportQueue,
				reqRead);
		}
		else
		{
			ElanPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"WdfRequestRetrieveOutputBuffer failed Status 0x%x\n", status);
		}
	}
	else
	{
		ElanPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfIoQueueRetrieveNextRequest failed Status 0x%x\n", status);
	}

	ElanPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"ElanProcessVendorReport Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
ElanReadReport(
	IN PELAN_CONTEXT DevContext,
	IN WDFREQUEST Request,
	OUT BOOLEAN* CompleteRequest
)
{
	NTSTATUS status = STATUS_SUCCESS;

	ElanPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"ElanReadReport Entry\n");

	//
	// Forward this read request to our manual queue
	// (in other words, we are going to defer this request
	// until we have a corresponding write request to
	// match it with)
	//

	status = WdfRequestForwardToIoQueue(Request, DevContext->ReportQueue);

	if (!NT_SUCCESS(status))
	{
		ElanPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfRequestForwardToIoQueue failed Status 0x%x\n", status);
	}
	else
	{
		*CompleteRequest = FALSE;
	}

	ElanPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"ElanReadReport Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
ElanSetFeature(
	IN PELAN_CONTEXT DevContext,
	IN WDFREQUEST Request,
	OUT BOOLEAN* CompleteRequest
)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDF_REQUEST_PARAMETERS params;
	PHID_XFER_PACKET transferPacket = NULL;
	ElanFeatureReport* pReport = NULL;

	ElanPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"ElanSetFeature Entry\n");

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	if (params.Parameters.DeviceIoControl.InputBufferLength < sizeof(HID_XFER_PACKET))
	{
		ElanPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"ElanSetFeature Xfer packet too small\n");

		status = STATUS_BUFFER_TOO_SMALL;
	}
	else
	{

		transferPacket = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;

		if (transferPacket == NULL)
		{
			ElanPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"ElanWriteReport No xfer packet\n");

			status = STATUS_INVALID_DEVICE_REQUEST;
		}
		else
		{
			//
			// switch on the report id
			//

			switch (transferPacket->reportId)
			{
			case REPORTID_FEATURE:

				if (transferPacket->reportBufferLen == sizeof(ElanFeatureReport))
				{
					pReport = (ElanFeatureReport*)transferPacket->reportBuffer;

					DevContext->DeviceMode = pReport->DeviceMode;

					ElanPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
						"ElanSetFeature DeviceMode = 0x%x\n", DevContext->DeviceMode);
				}
				else
				{
					status = STATUS_INVALID_PARAMETER;

					ElanPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
						"ElanSetFeature Error transferPacket->reportBufferLen (%d) is different from sizeof(ElanFeatureReport) (%d)\n",
						transferPacket->reportBufferLen,
						sizeof(ElanFeatureReport));
				}

				break;

			default:

				ElanPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
					"ElanSetFeature Unhandled report type %d\n", transferPacket->reportId);

				status = STATUS_INVALID_PARAMETER;

				break;
			}
		}
	}

	ElanPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"ElanSetFeature Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
ElanGetFeature(
	IN PELAN_CONTEXT DevContext,
	IN WDFREQUEST Request,
	OUT BOOLEAN* CompleteRequest
)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDF_REQUEST_PARAMETERS params;
	PHID_XFER_PACKET transferPacket = NULL;

	ElanPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"ElanGetFeature Entry\n");

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	if (params.Parameters.DeviceIoControl.OutputBufferLength < sizeof(HID_XFER_PACKET))
	{
		ElanPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"ElanGetFeature Xfer packet too small\n");

		status = STATUS_BUFFER_TOO_SMALL;
	}
	else
	{

		transferPacket = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;

		if (transferPacket == NULL)
		{
			ElanPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"ElanGetFeature No xfer packet\n");

			status = STATUS_INVALID_DEVICE_REQUEST;
		}
		else
		{
			//
			// switch on the report id
			//

			switch (transferPacket->reportId)
			{
			case REPORTID_MTOUCH:
			{

				ElanMaxCountReport* pReport = NULL;

				if (transferPacket->reportBufferLen == sizeof(ElanMaxCountReport))
				{
					pReport = (ElanMaxCountReport*)transferPacket->reportBuffer;

					pReport->MaximumCount = MULTI_MAX_COUNT;

					ElanPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
						"ElanGetFeature MaximumCount = 0x%x\n", MULTI_MAX_COUNT);
				}
				else
				{
					status = STATUS_INVALID_PARAMETER;

					ElanPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
						"ElanGetFeature Error transferPacket->reportBufferLen (%d) is different from sizeof(ElanMaxCountReport) (%d)\n",
						transferPacket->reportBufferLen,
						sizeof(ElanMaxCountReport));
				}

				break;
			}

			case REPORTID_FEATURE:
			{

				ElanFeatureReport* pReport = NULL;

				if (transferPacket->reportBufferLen == sizeof(ElanFeatureReport))
				{
					pReport = (ElanFeatureReport*)transferPacket->reportBuffer;

					pReport->DeviceMode = DevContext->DeviceMode;

					pReport->DeviceIdentifier = 0;

					ElanPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
						"ElanGetFeature DeviceMode = 0x%x\n", DevContext->DeviceMode);
				}
				else
				{
					status = STATUS_INVALID_PARAMETER;

					ElanPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
						"ElanGetFeature Error transferPacket->reportBufferLen (%d) is different from sizeof(ElanFeatureReport) (%d)\n",
						transferPacket->reportBufferLen,
						sizeof(ElanFeatureReport));
				}

				break;
			}

			default:

				ElanPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
					"ElanGetFeature Unhandled report type %d\n", transferPacket->reportId);

				status = STATUS_INVALID_PARAMETER;

				break;
			}
		}
	}

	ElanPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"ElanGetFeature Exit = 0x%x\n", status);

	return status;
}

PCHAR
DbgHidInternalIoctlString(
	IN ULONG IoControlCode
)
{
	switch (IoControlCode)
	{
	case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
		return "IOCTL_HID_GET_DEVICE_DESCRIPTOR";
	case IOCTL_HID_GET_REPORT_DESCRIPTOR:
		return "IOCTL_HID_GET_REPORT_DESCRIPTOR";
	case IOCTL_HID_READ_REPORT:
		return "IOCTL_HID_READ_REPORT";
	case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
		return "IOCTL_HID_GET_DEVICE_ATTRIBUTES";
	case IOCTL_HID_WRITE_REPORT:
		return "IOCTL_HID_WRITE_REPORT";
	case IOCTL_HID_SET_FEATURE:
		return "IOCTL_HID_SET_FEATURE";
	case IOCTL_HID_GET_FEATURE:
		return "IOCTL_HID_GET_FEATURE";
	case IOCTL_HID_GET_STRING:
		return "IOCTL_HID_GET_STRING";
	case IOCTL_HID_ACTIVATE_DEVICE:
		return "IOCTL_HID_ACTIVATE_DEVICE";
	case IOCTL_HID_DEACTIVATE_DEVICE:
		return "IOCTL_HID_DEACTIVATE_DEVICE";
	case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:
		return "IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST";
	case IOCTL_HID_SET_OUTPUT_REPORT:
		return "IOCTL_HID_SET_OUTPUT_REPORT";
	case IOCTL_HID_GET_INPUT_REPORT:
		return "IOCTL_HID_GET_INPUT_REPORT";
	default:
		return "Unknown IOCTL";
	}
}
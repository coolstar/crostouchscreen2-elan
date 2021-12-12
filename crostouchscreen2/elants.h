#include "stdint.h"

#define ELAN_TS_RESOLUTION(n, m)   (((n) - 1) * (m))

/* FW header data */
#define HEADER_SIZE		4
#define FW_HDR_TYPE		0
#define FW_HDR_COUNT		1
#define FW_HDR_LENGTH		2

/* Buffer mode Queue Header information */
#define QUEUE_HEADER_SINGLE	0x62
#define QUEUE_HEADER_NORMAL	0X63
#define QUEUE_HEADER_WAIT	0x64
#define QUEUE_HEADER_NORMAL2	0x66

/* Command header definition */
#define CMD_HEADER_WRITE	0x54
#define CMD_HEADER_READ		0x53
#define CMD_HEADER_6B_READ	0x5B
#define CMD_HEADER_ROM_READ	0x96
#define CMD_HEADER_RESP		0x52
#define CMD_HEADER_6B_RESP	0x9B
#define CMD_HEADER_ROM_RESP	0x95
#define CMD_HEADER_HELLO	0x55
#define CMD_HEADER_REK		0x66

/* FW position data */
#define PACKET_SIZE_OLD		40
#define PACKET_SIZE		55
#define MAX_CONTACT_NUM		10
#define FW_POS_HEADER		0
#define FW_POS_STATE		1
#define FW_POS_TOTAL		2
#define FW_POS_XY		3
#define FW_POS_TOOL_TYPE	33
#define FW_POS_CHECKSUM		34
#define FW_POS_WIDTH		35
#define FW_POS_PRESSURE		45

#define HEADER_REPORT_10_FINGER	0x62

/* Header (4 bytes) plus 3 fill 10-finger packets */
#define MAX_PACKET_SIZE		169

#define BOOT_TIME_DELAY_MS	50

/* FW read command, 0x53 0x?? 0x0, 0x01 */
#define E_ELAN_INFO_FW_VER	0x00
#define E_ELAN_INFO_BC_VER	0x10
#define E_ELAN_INFO_X_RES	0x60
#define E_ELAN_INFO_Y_RES	0x63
#define E_ELAN_INFO_REK		0xD0
#define E_ELAN_INFO_TEST_VER	0xE0
#define E_ELAN_INFO_FW_ID	0xF0
#define E_INFO_OSR		0xD6
#define E_INFO_PHY_SCAN		0xD7
#define E_INFO_PHY_DRIVER	0xD8

/* FW write command, 0x54 0x?? 0x0, 0x01 */
#define E_POWER_STATE_SLEEP	0x50
#define E_POWER_STATE_RESUME	0x58

#define MAX_RETRIES		3
#define MAX_FW_UPDATE_RETRIES	30

#define ELAN_FW_PAGESIZE	132

/* calibration timeout definition */
#define ELAN_CALI_TIMEOUT_MSEC	12000

#define ELAN_POWERON_DELAY_USEC	500
#define ELAN_RESET_DELAY_MSEC	20

enum elants_chip_id {
	EKTH3500,
	EKTF3624, //not supported
};

enum elants_state {
	ELAN_STATE_NORMAL,
	ELAN_WAIT_QUEUE_HEADER,
	ELAN_WAIT_RECALIBRATION,
};

enum elants_iap_mode {
	ELAN_IAP_OPERATIONAL,
	ELAN_IAP_RECOVERY,
};

#define MXT_T9_RELEASE		(1 << 5)
#define MXT_T9_PRESS		(1 << 6)
#define MXT_T9_DETECT		(1 << 7)
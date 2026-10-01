#pragma once
#include <efi.h>

#undef EFI_ERROR
#define EFI_ERROR(a) (((INTN)(a)) < 0)
#define EFI_HTTP_ERROR EFIERR(35)
#define EFI_CONNECTION_FIN EFIERR(104)
#define EFI_CONNECTION_RESET EFIERR(105)
#define EFI_CONNECTION_REFUSED EFIERR(106)

#define TCP4_SB_GUID   { 0x00720665, 0x67eb, 0x4a99, { 0xba, 0xf7, 0xd3, 0xc3, 0x3a, 0x1c, 0x7c, 0xc9 } }
#define TCP4_GUID      { 0x65530bc7, 0xa359, 0x410f, { 0xb0, 0x10, 0x5a, 0xad, 0xc7, 0xec, 0x2b, 0x62 } }
#define IP4CFG2_GUID   { 0x5b446ed1, 0xe30b, 0x4faa, { 0x87, 0x1a, 0x36, 0x54, 0xec, 0xa3, 0x60, 0x80 } }
#define DNS4_SB_GUID   { 0xb625b186, 0xe063, 0x44f7, { 0x89, 0x05, 0x6a, 0x74, 0xdc, 0x6f, 0x52, 0xb4 } }
#define DNS4_GUID      { 0xae3d28cc, 0xe05b, 0x4fa1, { 0xa0, 0x11, 0x7e, 0xb5, 0x5a, 0x3f, 0x14, 0x01 } }

typedef struct {
	CHAR16 Name[32];
	UINT8 IfType;
	UINT32 HwAddressSize;
	EFI_MAC_ADDRESS HwAddress;
	EFI_IPv4_ADDRESS StationAddress, SubnetMask;
	UINT32 RouteTableSize;
	EFI_IP4_ROUTE_TABLE *RouteTable;
} EFI_IP4_CONFIG2_INTERFACE_INFO;

typedef struct {
	VOID *SetData, *GetData;
} EFI_IP4_CONFIG2_PROTOCOL;

typedef struct {
	UINTN DnsServerListCount;
	EFI_IPv4_ADDRESS *DnsServerList;
	BOOLEAN UseDefaultSetting, EnableDnsCache;
	UINT8 Protocol;
	EFI_IPv4_ADDRESS StationIp, SubnetMask;
	UINT16 LocalPort;
	UINT32 RetryCount, RetryInterval;
} EFI_DNS4_CONFIG_DATA;

typedef struct {
	UINT32 IpCount;
	EFI_IPv4_ADDRESS *IpList;
} DNS_HOST_TO_ADDR_DATA;

typedef struct {
	EFI_EVENT Event;
	EFI_STATUS Status;
	UINT32 RetryCount, RetryInterval;
	DNS_HOST_TO_ADDR_DATA *H2AData;
} EFI_DNS4_COMPLETION_TOKEN;

typedef struct {
	VOID *GetModeData, *Configure, *HostNameToIp, *IpToHostName, *GeneralLookUp, *UpdateDnsCache, *Poll, *Cancel;
} EFI_DNS4_PROTOCOL;

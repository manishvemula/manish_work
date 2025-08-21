/**
  DO NOT EDIT
  FILE auto-generated
  Module name:
    AutoGen.h
  Abstract:       Auto-generated AutoGen.h for building module or library.
**/

#ifndef _AUTOGENH_2366c20f_e15a_11e3_8bf1_e4115b28bc50
#define _AUTOGENH_2366c20f_e15a_11e3_8bf1_e4115b28bc50

#ifdef __cplusplus
extern "C" {
#endif

#include <Uefi.h>
#include <Library/PcdLib.h>

extern GUID  gEfiCallerIdGuid;
extern GUID  gEdkiiDscPlatformGuid;
extern CHAR8 *gEfiCallerBaseName;

#define EFI_CALLER_ID_GUID \
  {0x2366c20f, 0xe15a, 0x11e3, {0x8b, 0xf1, 0xe4, 0x11, 0x5b, 0x28, 0xbc, 0x50}}
#define EDKII_DSC_PLATFORM_GUID \
  {0x3FD34E9B, 0xE90C, 0x44e1, {0xB5, 0x10, 0x1F, 0x63, 0x2A, 0x50, 0x9F, 0x10}}
#define STACK_COOKIE_VALUE 0x984232D8B1037F28ULL

// Guids
extern EFI_GUID gEfiTlsCaCertificateGuid;
extern EFI_GUID gEdkiiHttpTlsCipherListGuid;
extern EFI_GUID gEfiCertX509Guid;
extern EFI_GUID gEfiNetworkPkgTokenSpaceGuid;
extern EFI_GUID gEfiMdePkgTokenSpaceGuid;
extern EFI_GUID gEfiVTUTF8Guid;
extern EFI_GUID gEfiVT100Guid;
extern EFI_GUID gEfiVT100PlusGuid;
extern EFI_GUID gEfiPcAnsiGuid;
extern EFI_GUID gEfiUartDevicePathGuid;
extern EFI_GUID gEfiSasDevicePathGuid;
extern EFI_GUID gEfiVirtualDiskGuid;
extern EFI_GUID gEfiVirtualCdGuid;
extern EFI_GUID gEfiPersistentVirtualDiskGuid;
extern EFI_GUID gEfiPersistentVirtualCdGuid;
extern EFI_GUID gEfiEventReadyToBootGuid;
extern EFI_GUID gEfiEventAfterReadyToBootGuid;
extern EFI_GUID gEfiEventLegacyBootGuid;
extern EFI_GUID gEfiGlobalVariableGuid;
extern EFI_GUID gEfiAcpi20TableGuid;
extern EFI_GUID gEfiAcpi10TableGuid;
extern EFI_GUID gEfiSmbiosTableGuid;
extern EFI_GUID gEfiSmbios3TableGuid;
extern EFI_GUID gEfiAdapterInfoMediaStateGuid;
extern EFI_GUID gEfiRngAlgorithmRaw;
extern EFI_GUID gEfiRngAlgorithmSp80090Ctr256Guid;
extern EFI_GUID gEfiRngAlgorithmSp80090Hmac256Guid;
extern EFI_GUID gEfiRngAlgorithmSp80090Hash256Guid;
extern EFI_GUID gEfiRngAlgorithmArmRndr;

// Protocols
extern EFI_GUID gEfiHttpServiceBindingProtocolGuid;
extern EFI_GUID gEfiHttpProtocolGuid;
extern EFI_GUID gEfiHttpUtilitiesProtocolGuid;
extern EFI_GUID gEfiTcp4ServiceBindingProtocolGuid;
extern EFI_GUID gEfiTcp4ProtocolGuid;
extern EFI_GUID gEfiTcp6ServiceBindingProtocolGuid;
extern EFI_GUID gEfiTcp6ProtocolGuid;
extern EFI_GUID gEfiDns4ServiceBindingProtocolGuid;
extern EFI_GUID gEfiDns4ProtocolGuid;
extern EFI_GUID gEfiDns6ServiceBindingProtocolGuid;
extern EFI_GUID gEfiDns6ProtocolGuid;
extern EFI_GUID gEfiIp4Config2ProtocolGuid;
extern EFI_GUID gEfiIp6ConfigProtocolGuid;
extern EFI_GUID gEfiTlsServiceBindingProtocolGuid;
extern EFI_GUID gEfiTlsProtocolGuid;
extern EFI_GUID gEfiTlsConfigurationProtocolGuid;
extern EFI_GUID gEdkiiHttpCallbackProtocolGuid;
extern EFI_GUID gEfiDevicePathProtocolGuid;
extern EFI_GUID gEfiDebugPortProtocolGuid;
extern EFI_GUID gEfiDriverBindingProtocolGuid;
extern EFI_GUID gEfiSimpleTextOutProtocolGuid;
extern EFI_GUID gEfiGraphicsOutputProtocolGuid;
extern EFI_GUID gEfiHiiFontProtocolGuid;
extern EFI_GUID gEfiSimpleFileSystemProtocolGuid;
extern EFI_GUID gEfiUgaDrawProtocolGuid;
extern EFI_GUID gEfiComponentNameProtocolGuid;
extern EFI_GUID gEfiComponentName2ProtocolGuid;
extern EFI_GUID gEfiDriverConfigurationProtocolGuid;
extern EFI_GUID gEfiDriverConfiguration2ProtocolGuid;
extern EFI_GUID gEfiDriverDiagnosticsProtocolGuid;
extern EFI_GUID gEfiDriverDiagnostics2ProtocolGuid;
extern EFI_GUID gEfiSimpleNetworkProtocolGuid;
extern EFI_GUID gEfiManagedNetworkProtocolGuid;
extern EFI_GUID gEfiManagedNetworkServiceBindingProtocolGuid;
extern EFI_GUID gEfiAdapterInformationProtocolGuid;
extern EFI_GUID gEfiRngProtocolGuid;
extern EFI_GUID gEfiLoadedImageProtocolGuid;
extern EFI_GUID gEfiDpcProtocolGuid;

// Definition of SkuId Array
extern UINT64 _gPcd_SkuId_Array[];

// Definition of PCDs used in this module

#define _PCD_TOKEN_PcdAllowHttpConnections  0U
#define _PCD_SIZE_PcdAllowHttpConnections 1
#define _PCD_GET_MODE_SIZE_PcdAllowHttpConnections  _PCD_SIZE_PcdAllowHttpConnections 
#define _PCD_VALUE_PcdAllowHttpConnections  0U
extern const  BOOLEAN  _gPcd_FixedAtBuild_PcdAllowHttpConnections;
#define _PCD_GET_MODE_BOOL_PcdAllowHttpConnections  _gPcd_FixedAtBuild_PcdAllowHttpConnections
//#define _PCD_SET_MODE_BOOL_PcdAllowHttpConnections  ASSERT(FALSE)  // It is not allowed to set value for a FIXED_AT_BUILD PCD

#define _PCD_TOKEN_PcdHttpIoTimeout  0U
#define _PCD_GET_MODE_32_PcdHttpIoTimeout  LibPcdGet32(_PCD_TOKEN_PcdHttpIoTimeout)
#define _PCD_GET_MODE_SIZE_PcdHttpIoTimeout  LibPcdGetSize(_PCD_TOKEN_PcdHttpIoTimeout)
#define _PCD_SET_MODE_32_PcdHttpIoTimeout(Value)  LibPcdSet32(_PCD_TOKEN_PcdHttpIoTimeout, (Value))
#define _PCD_SET_MODE_32_S_PcdHttpIoTimeout(Value)  LibPcdSet32S(_PCD_TOKEN_PcdHttpIoTimeout, (Value))

#define _PCD_TOKEN_PcdHttpDnsRetryInterval  0U
#define _PCD_SIZE_PcdHttpDnsRetryInterval 4
#define _PCD_GET_MODE_SIZE_PcdHttpDnsRetryInterval  _PCD_SIZE_PcdHttpDnsRetryInterval 
#define _PCD_VALUE_PcdHttpDnsRetryInterval  0U
extern const  UINT32  _gPcd_FixedAtBuild_PcdHttpDnsRetryInterval;
#define _PCD_GET_MODE_32_PcdHttpDnsRetryInterval  _gPcd_FixedAtBuild_PcdHttpDnsRetryInterval
//#define _PCD_SET_MODE_32_PcdHttpDnsRetryInterval  ASSERT(FALSE)  // It is not allowed to set value for a FIXED_AT_BUILD PCD

#define _PCD_TOKEN_PcdHttpDnsRetryCount  0U
#define _PCD_SIZE_PcdHttpDnsRetryCount 4
#define _PCD_GET_MODE_SIZE_PcdHttpDnsRetryCount  _PCD_SIZE_PcdHttpDnsRetryCount 
#define _PCD_VALUE_PcdHttpDnsRetryCount  0U
extern const  UINT32  _gPcd_FixedAtBuild_PcdHttpDnsRetryCount;
#define _PCD_GET_MODE_32_PcdHttpDnsRetryCount  _gPcd_FixedAtBuild_PcdHttpDnsRetryCount
//#define _PCD_SET_MODE_32_PcdHttpDnsRetryCount  ASSERT(FALSE)  // It is not allowed to set value for a FIXED_AT_BUILD PCD

#define _PCD_TOKEN_PcdHttpTransferBufferSize  0U
#define _PCD_SIZE_PcdHttpTransferBufferSize 4
#define _PCD_GET_MODE_SIZE_PcdHttpTransferBufferSize  _PCD_SIZE_PcdHttpTransferBufferSize 
#define _PCD_VALUE_PcdHttpTransferBufferSize  0x200000U
extern const  UINT32  _gPcd_FixedAtBuild_PcdHttpTransferBufferSize;
#define _PCD_GET_MODE_32_PcdHttpTransferBufferSize  _gPcd_FixedAtBuild_PcdHttpTransferBufferSize
//#define _PCD_SET_MODE_32_PcdHttpTransferBufferSize  ASSERT(FALSE)  // It is not allowed to set value for a FIXED_AT_BUILD PCD

// Definition of PCDs used in libraries is in AutoGen.c


EFI_STATUS
EFIAPI
HttpDxeDriverEntryPoint (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  );



EFI_STATUS
EFIAPI
NetLibDefaultUnload (
  IN EFI_HANDLE        ImageHandle
  );



#ifdef __cplusplus
}
#endif

#endif

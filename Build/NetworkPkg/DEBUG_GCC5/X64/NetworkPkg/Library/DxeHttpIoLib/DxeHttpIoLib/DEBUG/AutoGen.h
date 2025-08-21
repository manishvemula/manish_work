/**
  DO NOT EDIT
  FILE auto-generated
  Module name:
    AutoGen.h
  Abstract:       Auto-generated AutoGen.h for building module or library.
**/

#ifndef _AUTOGENH_50B198F8_7986_4F51_A857_CFE4643D59F3
#define _AUTOGENH_50B198F8_7986_4F51_A857_CFE4643D59F3

#ifdef __cplusplus
extern "C" {
#endif

#include <PiDxe.h>
#include <Library/PcdLib.h>

extern GUID  gEfiCallerIdGuid;
extern GUID  gEdkiiDscPlatformGuid;
extern CHAR8 *gEfiCallerBaseName;

#define STACK_COOKIE_VALUE 0xFBECCE6A3A19AD87ULL

// Guids
extern EFI_GUID gEfiNetworkPkgTokenSpaceGuid;

// Protocols
extern EFI_GUID gEfiHttpProtocolGuid;

// Definition of SkuId Array
extern UINT64 _gPcd_SkuId_Array[];

// PCD definitions
#define _PCD_TOKEN_PcdMaxHttpChunkTransfer  0U
extern const UINT32 _gPcd_FixedAtBuild_PcdMaxHttpChunkTransfer;
#define _PCD_GET_MODE_32_PcdMaxHttpChunkTransfer  _gPcd_FixedAtBuild_PcdMaxHttpChunkTransfer
//#define _PCD_SET_MODE_32_PcdMaxHttpChunkTransfer  ASSERT(FALSE)  // It is not allowed to set value for a FIXED_AT_BUILD PCD
#define _PCD_VALUE_PcdMaxHttpChunkTransfer 0x0C00000
#define _PCD_SIZE_PcdMaxHttpChunkTransfer 4
#define _PCD_GET_MODE_SIZE_PcdMaxHttpChunkTransfer _PCD_SIZE_PcdMaxHttpChunkTransfer
#define _PCD_TOKEN_PcdHttpIoTimeout  0U
#define _PCD_GET_MODE_32_PcdHttpIoTimeout  LibPcdGet32(_PCD_TOKEN_PcdHttpIoTimeout)
#define _PCD_GET_MODE_SIZE_PcdHttpIoTimeout  LibPcdGetSize(_PCD_TOKEN_PcdHttpIoTimeout)
#define _PCD_SET_MODE_32_PcdHttpIoTimeout(Value)  LibPcdSet32(_PCD_TOKEN_PcdHttpIoTimeout, (Value))
#define _PCD_SET_MODE_32_S_PcdHttpIoTimeout(Value)  LibPcdSet32S(_PCD_TOKEN_PcdHttpIoTimeout, (Value))


#ifdef __cplusplus
}
#endif

#endif

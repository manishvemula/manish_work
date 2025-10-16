// IpmiLanApp.c
// UEFI IPMI-like client + responder (UDP4)
// Responds to incoming 2-byte requests [netfn, cmd] and returns [netfn+1, cmd, completion, payload...]
// Also sends small 2-byte requests to host BMC (so you can type commands in UEFI to talk to socket too)
// It:
// - initializes the network (SNP -> UDP4)
// - sends a small "initial" BIOS_INFO request so the socket BMC learns the guest address
// - listens for UDP packets and console input concurrently
// - when receiving a request from the socket, decodes and replies with UEFI info (real where possible)
// - when you type commands in the UEFI console it sends requests to the socket BMC
//

#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>  // AllocateZeroPool, FreePool
#include <Library/BaseMemoryLib.h>        // ZeroMem, CopyMem
#include <Protocol/Udp4.h>
#include <Protocol/ServiceBinding.h>
#include <Protocol/SimpleNetwork.h>
#include <Library/PrintLib.h>
#include <Library/BaseLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Guid/GlobalVariable.h>
#include <Protocol/LoadedImage.h>

#define DEFAULT_BMC_HOST_IP   {{127,0,0,1}}
#define DEFAULT_BMC_HOST_PORT 50891

#define IPMI_NETFN_APP              0x06
#define IPMI_NETFN_RESP             (IPMI_NETFN_APP + 1)

#define IPMI_CMD_GET_CHASSIS_STATUS 0x00
#define IPMI_CMD_GET_DEVICE_ID      0x01
#define IPMI_CMD_BIOS_INFO          0x02
#define IPMI_CMD_NETBOOT_INFO       0x03
#define IPMI_CMD_SECUREBOOT_INFO    0x04
#define IPMI_CMD_RESET              0x05
#define IPMI_CMD_SEL_VIEW           0x06
#define IPMI_CMD_SEL_CLEAR          0x07
#define IPMI_CMD_POWER_ON           0x08
#define IPMI_CMD_POWER_OFF          0x09
#define IPMI_CMD_POWER_CYCLE        0x0A
#define IPMI_CMD_FRU                0x0B

#define IPMI_CMD_BMC_FEATURE_SET   0x0C
#define IPMI_CMD_BMC_FEATURE_GET   0x0D
#define BMC_FEATURE_KCS             0x01
#define BMC_FEATURE_HOST_IF         0x02
#define BMC_FEATURE_NETWORK         0x03


#define IPMI_CMD_BMC_NET_SET       0x0E
#define IPMI_CMD_BMC_NET_GET       0x0F

#define FEATURE_VAR_NAME  L"BmcFeatures"
EFI_GUID FEATURE_VAR_GUID = { 0x12345678,0x1234,0x1234,{0x12,0x34,0x12,0x34,0x12,0x34,0x12,0x34} };

typedef struct {
    BOOLEAN KcsEnabled;
    BOOLEAN HostIfEnabled;
    BOOLEAN NetworkEnabled;
} BMC_FEATURE_STATE;

STATIC EFI_STATUS SaveBmcFeatures(BMC_FEATURE_STATE *State) {
    return gRT->SetVariable(
        FEATURE_VAR_NAME,
        &FEATURE_VAR_GUID,
        EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_NON_VOLATILE,
        sizeof(BMC_FEATURE_STATE),
        State
    );
}

STATIC BMC_FEATURE_STATE LoadBmcFeatures(VOID) {
    BMC_FEATURE_STATE s = { TRUE, TRUE, TRUE };
    UINTN sz = sizeof(s);
    EFI_STATUS Status;
    Status = gRT->GetVariable(FEATURE_VAR_NAME, &FEATURE_VAR_GUID, NULL, &sz, &s);
    if (EFI_ERROR(Status)) {
        // If variable doesn't exist, create it once
        SaveBmcFeatures(&s);
    }
    return s;
}


/* forward declaration so calls earlier in file don't cause implicit decl errors */
STATIC EFI_STATUS SendIpmiRequestWithData(EFI_UDP4_PROTOCOL *Udp, UINT8 Cmd, UINT8 *Data, UINTN DataLen);

STATIC EFI_STATUS GetVariableData(CONST CHAR16 *VarName, EFI_GUID *VarGuid, UINT8 **Data, UINTN *DataSize); // forward declare



STATIC
CHAR8* MyAsciiStrDup(CONST CHAR8 *Str) {
    if (!Str) return NULL;
    UINTN Len = AsciiStrLen(Str);
    CHAR8 *Out = AllocateZeroPool(Len + 1);
    if (Out) CopyMem(Out, Str, Len);
    return Out;
}

///
/// ---- Helper to read ASCII line from UEFI console ----
///
STATIC
EFI_STATUS
ReadAsciiLine(CHAR8 *Buffer, UINTN MaxSize)
{
    UINTN i = 0;
    EFI_INPUT_KEY Key;
    EFI_STATUS Status;

    while (i < MaxSize - 1) {
        UINTN EventIndex;
        gBS->WaitForEvent(1, &gST->ConIn->WaitForKey, &EventIndex);
        Status = gST->ConIn->ReadKeyStroke(gST->ConIn, &Key);
        if (EFI_ERROR(Status)) continue;

        if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN || Key.UnicodeChar == L'\n') {
            break;
        }

        if (Key.UnicodeChar == CHAR_BACKSPACE && i > 0) {
            i--;
            Print(L"\b \b");
            continue;
        }

        if (Key.UnicodeChar >= 32 && Key.UnicodeChar < 127) {
            Buffer[i++] = (CHAR8)Key.UnicodeChar;
            Print(L"%c", Key.UnicodeChar);
        }
    }
    Buffer[i] = '\0';
    Print(L"\n");
    return EFI_SUCCESS;
}

// Helper: ensure NIC is UP and initialized via SimpleNetworkProtocol
STATIC EFI_STATUS EnsureNicReady(VOID) {
    EFI_STATUS Status;
    EFI_SIMPLE_NETWORK_PROTOCOL *Snp;
    EFI_HANDLE *Handles = NULL;
    UINTN HandleCount = 0;
    Status = gBS->LocateHandleBuffer(ByProtocol, &gEfiSimpleNetworkProtocolGuid, NULL, &HandleCount, &Handles);
    if (EFI_ERROR(Status)) { Print(L"[ERROR] Locate SNP: %r\n", Status); return Status; }
    Status = gBS->HandleProtocol(Handles[0], &gEfiSimpleNetworkProtocolGuid, (VOID**)&Snp);
    if (EFI_ERROR(Status)) { Print(L"[ERROR] HandleProtocol SNP: %r\n", Status); if (Handles) FreePool(Handles); return Status; }
    if (Snp->Mode->State == EfiSimpleNetworkStopped) { Status = Snp->Start(Snp); if (EFI_ERROR(Status)) { FreePool(Handles); return Status; } }
    if (Snp->Mode->State != EfiSimpleNetworkInitialized) { Status = Snp->Initialize(Snp, 0, 0); if (EFI_ERROR(Status)) { FreePool(Handles); return Status; } }
    Print(L"[OK] NIC ready (SNP initialized)\n");
    if (Handles) FreePool(Handles);
    return EFI_SUCCESS;
}

// Utility: read UEFI variable (returns allocated buffer)
STATIC EFI_STATUS GetVariableData(CONST CHAR16 *VarName, EFI_GUID *VarGuid, UINT8 **Data, UINTN *DataSize) {
    EFI_STATUS Status; UINTN Size = 0; *Data = NULL; *DataSize = 0;
    Status = gRT->GetVariable((CHAR16*)VarName, VarGuid, NULL, &Size, NULL);
    if (Status == EFI_BUFFER_TOO_SMALL) {
        *Data = AllocateZeroPool(Size);
        if (!*Data) return EFI_OUT_OF_RESOURCES;
        Status = gRT->GetVariable((CHAR16*)VarName, VarGuid, NULL, &Size, *Data);
        if (EFI_ERROR(Status)) { FreePool(*Data); *Data = NULL; *DataSize = 0; return Status; }
        *DataSize = Size;
        return EFI_SUCCESS;
    }
    return Status;
}

// Convert CHAR16 -> allocated CHAR8
STATIC CHAR8* UnicodeToAsciiAlloc(CONST CHAR16 *Str) {
    if (!Str) return NULL;
    UINTN Len = StrLen((CHAR16*)Str);
    CHAR8 *Out = AllocateZeroPool(Len + 1);
    if (!Out) return NULL;
    for (UINTN i=0;i<Len;i++) Out[i] = (CHAR8)Str[i];
    return Out;
}

// Build BIOS info from gST -> return newly allocated CHAR8* (caller must FreePool)
STATIC CHAR8* BuildBiosInfoPayload(VOID) {
    CHAR16 Buf16[256]; ZeroMem(Buf16, sizeof(Buf16));
    UnicodeSPrint(Buf16, sizeof(Buf16), L"Vendor=%s;Revision=%u",
                  (gST->FirmwareVendor?gST->FirmwareVendor:L"(unknown)"), gST->FirmwareRevision);
    return UnicodeToAsciiAlloc(Buf16);
}

// Build SecureBoot info by reading UEFI variables
STATIC CHAR8* BuildSecureBootPayload(VOID) {
    UINT8 *data = NULL; UINTN size = 0;
    CHAR8 tmp[256]; ZeroMem(tmp, sizeof(tmp));
    EFI_STATUS st = GetVariableData(L"SecureBoot", &gEfiGlobalVariableGuid, &data, &size);
    if (!EFI_ERROR(st) && size == 1) AsciiSPrint(tmp, sizeof(tmp), "SecureBoot=%u", data[0]);
    else AsciiSPrint(tmp, sizeof(tmp), "SecureBoot=Unknown");
    if (data) { FreePool(data); data = NULL; }
    CHAR8 *out = AllocateZeroPool(AsciiStrLen(tmp) + 1);
    if (out) AsciiStrCpyS(out, AsciiStrSize(tmp), tmp);
    return out;
}

// Build Netboot info (simple): check presence of network boot items in BootOrder
STATIC CHAR8* BuildNetbootPayload(VOID) {
    CHAR8 tmp[256]; ZeroMem(tmp, sizeof(tmp));
    AsciiSPrint(tmp, sizeof(tmp), "NetBoot=Unknown");
    // We try BootOrder variable to detect network boot descriptions
    UINT8 *bootOrder=NULL; UINTN bootSize=0;
    EFI_STATUS st = GetVariableData(L"BootOrder", &gEfiGlobalVariableGuid, &bootOrder, &bootSize);
    if (!EFI_ERROR(st) && bootSize >= sizeof(UINT16)) {
        UINT16 *order = (UINT16*)bootOrder; UINTN count = bootSize / sizeof(UINT16);
        BOOLEAN foundNet = FALSE;
        for (UINTN i=0;i<count;i++) {
            CHAR16 varName[16]; UnicodeSPrint(varName, sizeof(varName), L"Boot%04X", order[i]);
            UINT8 *bootData=NULL; UINTN bsize=0;
            if (!EFI_ERROR(GetVariableData(varName, &gEfiGlobalVariableGuid, &bootData, &bsize)) && bsize > 6) {
                CHAR16 *desc = (CHAR16*)(bootData + 6);
                if (desc) {
                    CHAR8 *asc = UnicodeToAsciiAlloc(desc);
                    if (asc) {
                        if (AsciiStrStr(asc,"Net") || AsciiStrStr(asc,"PXE") || AsciiStrStr(asc,"Network")) foundNet = TRUE;
                        FreePool(asc);
                    }
                }
                FreePool(bootData);
            }
        }
        if (foundNet) AsciiSPrint(tmp, sizeof(tmp), "NetBoot=Enabled:PXE=Yes");
        else AsciiSPrint(tmp, sizeof(tmp), "NetBoot=Disabled");
    } else {
        AsciiSPrint(tmp, sizeof(tmp), "NetBoot=Unknown");
    }
    if (bootOrder) FreePool(bootOrder);
    CHAR8 *out = AllocateZeroPool(AsciiStrLen(tmp) + 1);
    if (out) AsciiStrCpyS(out, AsciiStrSize(tmp), tmp);
    return out;
}

// SEL persistence in UEFI variable "FakeSel" (so socket side can request sel_view / sel_clear)
STATIC CHAR16 *SEL_VAR_NAME = L"FakeSel";
STATIC EFI_GUID SEL_VAR_GUID = EFI_GLOBAL_VARIABLE; // reuse global guid for simplicity

STATIC CHAR8* GetSelPayload(VOID) {
    UINT8 *data=NULL; UINTN size=0;
    EFI_STATUS st = GetVariableData(SEL_VAR_NAME, &SEL_VAR_GUID, &data, &size);
    if (EFI_ERROR(st) || size==0) {
        // no SEL stored -> empty message
        const CHAR8 *msg = "SEL: (empty)";
        CHAR8 *out = AllocateZeroPool(AsciiStrLen(msg)+1);
        if (out) AsciiStrCpyS(out, AsciiStrSize(msg), msg);
        return out;
    }
    CHAR8 *out = AllocateZeroPool(size+1);
    if (out) CopyMem(out, data, size);
    if (data) FreePool(data);
    return out;
}

STATIC EFI_STATUS ClearSel(VOID) {
    // Set the variable to zero-length -> cleared
    EFI_STATUS st = gRT->SetVariable(SEL_VAR_NAME, &SEL_VAR_GUID, EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS, 0, NULL);
    if (EFI_ERROR(st)) {
        // If setting variable fails, return error but still return simulated success string elsewhere
        return st;
    }
    return EFI_SUCCESS;
}

STATIC VOID __attribute__((used))
AddSelEntry(CONST CHAR8 *msg) {
    UINT8 *data = NULL; 
    UINTN size = 0;

    GetVariableData(SEL_VAR_NAME, &SEL_VAR_GUID, &data, &size);

    UINTN newlen = size + AsciiStrLen(msg) + 2;
    CHAR8 *buf = AllocateZeroPool(newlen);
    if (!buf) return;

    if (data) {
        CopyMem(buf, data, size);
        FreePool(data);
    }

    if (size > 0) {
        AsciiStrCatS(buf, newlen, "\n");
    }
    AsciiStrCatS(buf, newlen, msg);

    gRT->SetVariable(
        SEL_VAR_NAME,
        &SEL_VAR_GUID,
        EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS,
        AsciiStrSize(buf) - 1,
        buf
    );

    FreePool(buf);
}


STATIC CHAR8* GetFruPayload(VOID) {
    const CHAR8 *board = "UEFI-FakeBoard";
    const CHAR8 *serial = "SN12345";
    CHAR8 *out = AllocateZeroPool(64);
    if (out)
        AsciiSPrint(out, 64, "FRU: Board=%a; SN=%a", board, serial);
    return out;
}


STATIC EFI_STATUS DoPowerOff(VOID) {
    Print(L"[INFO] Powering off system...\n");
    gRT->ResetSystem(EfiResetShutdown, EFI_SUCCESS, 0, NULL);
    return EFI_SUCCESS; // never returns
}

STATIC EFI_STATUS DoPowerCycle(VOID) {
    Print(L"[INFO] Power cycling system...\n");
    gRT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
    return EFI_SUCCESS; // never returns
}

STATIC EFI_STATUS __attribute__((used))
DoReset(VOID) {
    Print(L"[INFO] Resetting system...\n");
    gRT->ResetSystem(EfiResetWarm, EFI_SUCCESS, 0, NULL);
    return EFI_SUCCESS; // never returns
}

// Send IPMI request with extra data payload
STATIC EFI_STATUS SendIpmiRequestWithData(EFI_UDP4_PROTOCOL *Udp, UINT8 Cmd, UINT8 *Data, UINTN DataLen) {
    EFI_STATUS Status;
    UINT8 buf[256];
    UINTN totalLen = 2;

    buf[0] = IPMI_NETFN_APP;
    buf[1] = Cmd;

    if (Data && DataLen > 0) {
        CopyMem(&buf[2], Data, DataLen);
        totalLen += DataLen;
    }

    EFI_IPv4_ADDRESS DestIp = DEFAULT_BMC_HOST_IP;
    EFI_UDP4_SESSION_DATA Sess;
    ZeroMem(&Sess,sizeof(Sess));
    Sess.SourcePort = 0;
    Sess.DestinationPort = DEFAULT_BMC_HOST_PORT;
    CopyMem(&Sess.DestinationAddress,&DestIp,sizeof(EFI_IPv4_ADDRESS));

    EFI_UDP4_TRANSMIT_DATA *TxData = AllocateZeroPool(sizeof(EFI_UDP4_TRANSMIT_DATA)+sizeof(EFI_UDP4_FRAGMENT_DATA));
    if (!TxData) { Print(L"[ERROR] Out of memory\n"); return EFI_OUT_OF_RESOURCES; }

    TxData->UdpSessionData = &Sess;
    TxData->DataLength = (UINT32)totalLen;
    TxData->FragmentCount = 1;
    TxData->FragmentTable[0].FragmentLength = (UINT32)totalLen;
    TxData->FragmentTable[0].FragmentBuffer = buf;

    EFI_UDP4_COMPLETION_TOKEN TxToken;
    ZeroMem(&TxToken, sizeof(TxToken));
    EFI_EVENT TxEvent = NULL;
    Status = gBS->CreateEvent(0, TPL_CALLBACK, NULL, NULL, &TxEvent);
    if (!EFI_ERROR(Status)) TxToken.Event = TxEvent;
    TxToken.Packet.TxData = TxData;

    Status = Udp->Transmit(Udp, &TxToken);
    if (EFI_ERROR(Status)) {
        Print(L"[ERROR] Udp->Transmit: %r\n", Status);
        if (TxEvent) gBS->CloseEvent(TxEvent);
        FreePool(TxData);
        return Status;
    }
    while (TxEvent && gBS->CheckEvent(TxEvent)==EFI_NOT_READY) { Udp->Poll(Udp); gBS->Stall(1000); }
    Print(L"[OK] Transmitted %u bytes\n", (UINT32)totalLen);
    if (TxEvent) gBS->CloseEvent(TxEvent);
    FreePool(TxData);
    return EFI_SUCCESS;
}


// Send small 2-byte request to BMC host (127.0.0.1:50891)
STATIC EFI_STATUS SendIpmiRequest(EFI_UDP4_PROTOCOL *Udp, UINT8 cmd) {
    EFI_STATUS Status;
    UINT8 req[2];
    req[0] = IPMI_NETFN_APP; req[1] = cmd;

    EFI_IPv4_ADDRESS DestIp = DEFAULT_BMC_HOST_IP;
    EFI_UDP4_SESSION_DATA Sess;
    ZeroMem(&Sess,sizeof(Sess));
    Sess.SourcePort = 0;
    Sess.DestinationPort = DEFAULT_BMC_HOST_PORT;
    CopyMem(&Sess.DestinationAddress,&DestIp,sizeof(EFI_IPv4_ADDRESS));

    EFI_UDP4_TRANSMIT_DATA *TxData = AllocateZeroPool(sizeof(EFI_UDP4_TRANSMIT_DATA)+sizeof(EFI_UDP4_FRAGMENT_DATA));
    if (TxData == NULL) { Print(L"[ERROR] Out of memory to build request\n"); return EFI_OUT_OF_RESOURCES; }
    TxData->UdpSessionData = &Sess;
    TxData->DataLength = 2;
    TxData->FragmentCount = 1;
    TxData->FragmentTable[0].FragmentLength = 2;
    TxData->FragmentTable[0].FragmentBuffer = req;

    EFI_UDP4_COMPLETION_TOKEN TxToken;
    ZeroMem(&TxToken, sizeof(TxToken));
    EFI_EVENT TxEvent = NULL;
    Status = gBS->CreateEvent(0, TPL_CALLBACK, NULL, NULL, &TxEvent);
    if (!EFI_ERROR(Status)) TxToken.Event = TxEvent;
    TxToken.Packet.TxData = TxData;

    Status = Udp->Transmit(Udp, &TxToken);
    if (EFI_ERROR(Status)) {
        Print(L"[ERROR] Udp->Transmit request: %r\n", Status);
        if (TxEvent) gBS->CloseEvent(TxEvent);
        FreePool(TxData);
        return Status;
    }
    // Wait for transmit completion
    while (TxEvent && gBS->CheckEvent(TxEvent)==EFI_NOT_READY) { Udp->Poll(Udp); gBS->Stall(1000); }
    Print(L"[OK] Transmitted %u bytes\n", 2);
    if (TxEvent) gBS->CloseEvent(TxEvent);
    FreePool(TxData);
    return EFI_SUCCESS;
}

// Parse and print a response (netfn == IPMI_NETFN_RESP)
STATIC VOID PrintIpmiResponse(UINT8 *buf, UINT32 totalLen) {
    if (totalLen < 3) {
        Print(L"Reply: (too short)\n");
        return;
    }
    UINT8 cmd = buf[1];
    UINT8 cc = buf[2];
    Print(L"Completion code = 0x%02x\n", cc);

    if (cmd == IPMI_CMD_GET_CHASSIS_STATUS && totalLen >= 4) {
        UINT8 ch = buf[3];
        if (ch == 0x01) Print(L"Reply: Chassis Power = ON\n");
        else if (ch == 0x00) Print(L"Reply: Chassis Power = OFF\n");
        else Print(L"Reply: Chassis Power = UNKNOWN (0x%02x)\n", ch);
    } else if (cmd == IPMI_CMD_BMC_FEATURE_GET || cmd == IPMI_CMD_BMC_FEATURE_SET || cmd == IPMI_CMD_BMC_NET_GET || cmd == IPMI_CMD_BMC_NET_SET) {
        if (totalLen >= 4) {
            Print(L"Reply: %s\n", (buf[3] ? L"ENABLED" : L"DISABLED"));
        } else {
            Print(L"Reply: (invalid feature reply)\n");
        }
    } else {
        // treat payload as ASCII starting at buf[3]
        if (totalLen > 3) {
            CHAR16 *ustr = AllocateZeroPool((totalLen - 3 + 1) * sizeof(CHAR16));
            if (ustr) {
                for (UINT32 i = 0; i + 3 < totalLen; ++i) ustr[i] = (CHAR16)buf[3 + i];
                Print(L"Reply: %s\n", ustr);
                FreePool(ustr);
            } else {
                Print(L"Reply: payload (no memory)\n");
            }
        } else {
            Print(L"Reply: (no payload)\n");
        }
    }
}

// Main entrypoint
EFI_STATUS EFIAPI UefiMain (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS Status;
    EFI_HANDLE Child = NULL;
    EFI_SERVICE_BINDING_PROTOCOL *UdpSb = NULL;
    EFI_UDP4_PROTOCOL *Udp = NULL;

    Print(L"\n=== IpmiLanApp (interactive) ===\n");

    // 1) NIC ready
    Status = EnsureNicReady();
    if (EFI_ERROR(Status)) goto cleanup;

    // 2) UDP4 service binding + child
    Status = gBS->LocateProtocol(&gEfiUdp4ServiceBindingProtocolGuid, NULL, (VOID **)&UdpSb);
    if (EFI_ERROR(Status)) { Print(L"[ERROR] Locate UDP4 ServiceBinding: %r\n", Status); goto cleanup; }
    Status = UdpSb->CreateChild(UdpSb, &Child);
    if (EFI_ERROR(Status)) { Print(L"[ERROR] CreateChild: %r\n", Status); goto cleanup; }
    Status = gBS->HandleProtocol(Child, &gEfiUdp4ProtocolGuid, (VOID **)&Udp);
    if (EFI_ERROR(Status)) { Print(L"[ERROR] HandleProtocol UDP4: %r\n", Status); goto cleanup; }

    // 3) configure UDP (listen)
    EFI_UDP4_CONFIG_DATA Cfg; ZeroMem(&Cfg,sizeof(Cfg));
    Cfg.AcceptAnyPort = TRUE; Cfg.UseDefaultAddress = TRUE; Cfg.TimeToLive = 64;
    Cfg.AcceptBroadcast = TRUE; Cfg.AcceptPromiscuous = TRUE; Cfg.AllowDuplicatePort = TRUE;
    Status = Udp->Configure(Udp, &Cfg);
    if (EFI_ERROR(Status)) { Print(L"[ERROR] Udp4->Configure: %r\n", Status); goto cleanup; }
    Print(L"[OK] UDP4 configured\n");

    // 4) Send small initial request so server learns our address (harmless)
    (void) SendIpmiRequest(Udp, IPMI_CMD_BIOS_INFO);

    // 5) Prepare receive event
    EFI_UDP4_COMPLETION_TOKEN RxToken; ZeroMem(&RxToken,sizeof(RxToken));
    EFI_EVENT RxEvent;
    Status = gBS->CreateEvent(0, TPL_NOTIFY, NULL, NULL, &RxEvent);
    if (EFI_ERROR(Status)) { Print(L"[ERROR] CreateEvent(RxEvent): %r\n", Status); goto cleanup; }
    RxToken.Event = RxEvent;
    Status = Udp->Receive(Udp, &RxToken);
    if (EFI_ERROR(Status)) { Print(L"[WARN] Udp->Receive initial failed: %r\n", Status); }

    // Event array: RxEvent and console key
    EFI_EVENT Events[2]; Events[0] = RxEvent; Events[1] = gST->ConIn->WaitForKey;

    BMC_FEATURE_STATE cur = LoadBmcFeatures();
    Print(L"[INFO] Loaded: KCS=%d HostIF=%d Network=%d\n",cur.KcsEnabled, cur.HostIfEnabled, cur.NetworkEnabled);


    Print(L"\n[IPMI MENU] Available commands (type and press Enter):\n");
    Print(L"  deviceid  chassis  fru  sel_view  sel_clear  power_on  power_off  power_cycle reset\n");
    Print(L"  bios  netboot  secureboot  exit\n\n");

    CHAR8 line[128]; ZeroMem(line,sizeof(line));

    for (;;) {
        UINTN Index;
        Status = gBS->WaitForEvent(2, Events, &Index);
        if (EFI_ERROR(Status)) break;

        if (Index == 0) {
            // Received a UDP packet from BMC
            if (RxToken.Status == EFI_SUCCESS && RxToken.Packet.RxData) {
                EFI_UDP4_RECEIVE_DATA *RxData = RxToken.Packet.RxData;
                if (RxData->FragmentCount && RxData->FragmentTable[0].FragmentBuffer) {
                    UINT8 *buf = (UINT8 *)RxData->FragmentTable[0].FragmentBuffer;
                    UINT32 totalLen = RxData->DataLength;
                    if (totalLen >= 2) {
                        UINT8 netfn = buf[0];
                        UINT8 cmd = buf[1];

                        if (netfn == IPMI_NETFN_APP) {
                            // Incoming REQUEST from socket -> build appropriate response and send it to source
                            Print(L"Decoded Request: netfn=0x%02x cmd=0x%02x\n", netfn, cmd);

                            // Build response header
                            UINT8 respBuf[1024]; UINTN rp = 0;
                            respBuf[rp++] = (UINT8)(netfn + 1); // netfn + 1
                            respBuf[rp++] = cmd;
                            respBuf[rp++] = 0x00; // completion OK

                            // Fill payload based on requested cmd
                            if (cmd == IPMI_CMD_GET_CHASSIS_STATUS) {
                                // Try to infer chassis state — for simplicity, we return OFF (0x00).
                                // If you want to query virtual machine state or platform sensor, add logic here.
                                respBuf[rp++] = 0x00;
                            } else if (cmd == IPMI_CMD_GET_DEVICE_ID) {
                                // Return a 10-byte device-id-like sequence
                                respBuf[rp++] = 0x20; respBuf[rp++] = 0x81; respBuf[rp++] = 0x01;
                                respBuf[rp++] = 0x00; respBuf[rp++] = 0x02; respBuf[rp++] = 0x00;
                                respBuf[rp++] = 0x34; respBuf[rp++] = 0x12; respBuf[rp++] = 0x00; respBuf[rp++] = 0x01;
                            } else if (cmd == IPMI_CMD_BIOS_INFO) {
                                CHAR8 *payload = BuildBiosInfoPayload();
                                if (payload) {
                                    UINTN l = AsciiStrLen(payload); if (l > sizeof(respBuf)-rp-1) l = sizeof(respBuf)-rp-1;
                                    CopyMem(&respBuf[rp], payload, l); rp += l; FreePool(payload);
                                } else {
                                    CHAR8 *s = "Vendor=(unknown)"; UINTN l = AsciiStrLen(s); CopyMem(&respBuf[rp], s, l); rp+=l;
                                }
                            } else if (cmd == IPMI_CMD_NETBOOT_INFO) {
                                CHAR8 *payload = BuildNetbootPayload();
                                if (payload) {
                                    UINTN l = AsciiStrLen(payload); if (l > sizeof(respBuf)-rp-1) l = sizeof(respBuf)-rp-1;
                                    CopyMem(&respBuf[rp], payload, l); rp += l; FreePool(payload);
                                } else { CHAR8 *s="NetBoot=Unknown"; UINTN l=AsciiStrLen(s); CopyMem(&respBuf[rp], s,l); rp+=l;}
                            } else if (cmd == IPMI_CMD_SECUREBOOT_INFO) {
                                CHAR8 *payload = BuildSecureBootPayload();
                                if (payload) { UINTN l = AsciiStrLen(payload); if (l>sizeof(respBuf)-rp-1) l=sizeof(respBuf)-rp-1;
                                                CopyMem(&respBuf[rp], payload, l); rp += l; FreePool(payload);
                                } else { CHAR8 *s="SecureBoot=Unknown"; UINTN l=AsciiStrLen(s); CopyMem(&respBuf[rp], s,l); rp+=l;}
                            } else if (cmd == IPMI_CMD_SEL_VIEW) {
                                CHAR8 *s = GetSelPayload();
                                if(!s){ s = MyAsciiStrDup("SEL: (empty)");}
                                UINTN l = AsciiStrLen(s);
                                CopyMem(&respBuf[rp], s, l); rp += l;
                                if (s) FreePool(s);
                            } else if (cmd == IPMI_CMD_SEL_CLEAR) {
                                ClearSel();
                                CHAR8 *s = "SEL: Cleared";
                                UINTN l = AsciiStrLen(s);
                                CopyMem(&respBuf[rp], s, l); rp += l;
                            } else if (cmd == IPMI_CMD_FRU) {
                                CHAR8 *s = GetFruPayload();
                                if (!s) s = MyAsciiStrDup("FRU: Unknown");
                                UINTN l = AsciiStrLen(s);
                                CopyMem(&respBuf[rp], s, l); rp += l;
                                if (s) FreePool(s);
                            } else if (cmd == IPMI_CMD_POWER_ON) {
                                CHAR8 *s = "Power: Already ON";
                                UINTN l = AsciiStrLen(s);
                                CopyMem(&respBuf[rp], s, l); rp += l;
                            } else if (cmd == IPMI_CMD_POWER_OFF) {
                                DoPowerOff();
                            } else if (cmd == IPMI_CMD_POWER_CYCLE) {
                                DoPowerCycle();
                            } else if (cmd == IPMI_CMD_RESET) {
                                CHAR8 *s = "Reset: Will reset now"; UINTN l=AsciiStrLen(s); CopyMem(&respBuf[rp], s, l); rp+=l;
                            } 
                            else if (cmd == IPMI_CMD_BMC_NET_SET) {
                                // Rx data format: [feature value], i.e. buf[2] == value (1/0)
                                if (totalLen >= 3) {
                                    UINT8 val = buf[2];
                                    BMC_FEATURE_STATE st = LoadBmcFeatures();
                                    st.NetworkEnabled = (val ? TRUE : FALSE);
                                    SaveBmcFeatures(&st);
                                }
                                respBuf[rp++] = (LoadBmcFeatures().NetworkEnabled ? 1 : 0);
                            } else if (cmd == IPMI_CMD_BMC_NET_GET) {
                                respBuf[rp++] = (LoadBmcFeatures().NetworkEnabled ? 1 : 0);
} else if (cmd == IPMI_CMD_BMC_FEATURE_SET) {
    if (totalLen >= 4) {
        UINT8 fid = buf[2];
        UINT8 val = buf[3];
        BMC_FEATURE_STATE st = LoadBmcFeatures();

        if (fid == BMC_FEATURE_KCS)
            st.KcsEnabled = (val ? TRUE : FALSE);
        else if (fid == BMC_FEATURE_HOST_IF)
            st.HostIfEnabled = (val ? TRUE : FALSE);
        else if (fid == BMC_FEATURE_NETWORK)
            st.NetworkEnabled = (val ? TRUE : FALSE);

        EFI_STATUS S = SaveBmcFeatures(&st);
        if (EFI_ERROR(S))
            Print(L"[WARN] SaveBmcFeatures: %r\n", S);

        respBuf[rp++] = val;
    } else {
        respBuf[rp++] = 0;
    }
}
else if (cmd == IPMI_CMD_BMC_FEATURE_GET) {
    if (totalLen >= 3) {
        UINT8 fid = buf[2];
        BMC_FEATURE_STATE st = LoadBmcFeatures();
        UINT8 val = 0;

        if (fid == BMC_FEATURE_KCS)
            val = st.KcsEnabled;
        else if (fid == BMC_FEATURE_HOST_IF)
            val = st.HostIfEnabled;
        else if (fid == BMC_FEATURE_NETWORK)
            val = st.NetworkEnabled;

        respBuf[rp++] = val;
    } else {
        respBuf[rp++] = 0;
    }
}

else {
                                CHAR8 *s = "Unsupported"; UINTN l = AsciiStrLen(s); CopyMem(&respBuf[rp], s, l); rp += l;
                            }

                            // Send response to the source (BMC socket)
                            EFI_UDP4_SESSION_DATA Sess2; ZeroMem(&Sess2,sizeof(Sess2));
                            Sess2.SourcePort = 0;
                            Sess2.DestinationPort = (UINT16)RxData->UdpSession.SourcePort;
                            CopyMem(&Sess2.DestinationAddress, &RxData->UdpSession.SourceAddress, sizeof(EFI_IPv4_ADDRESS));
                            EFI_UDP4_TRANSMIT_DATA *TxData2 = AllocateZeroPool(sizeof(EFI_UDP4_TRANSMIT_DATA)+sizeof(EFI_UDP4_FRAGMENT_DATA));
                            if (TxData2) {
                                TxData2->UdpSessionData = &Sess2;
                                TxData2->DataLength = (UINT32)rp;
                                TxData2->FragmentCount = 1;
                                TxData2->FragmentTable[0].FragmentLength = (UINT32)rp;
                                TxData2->FragmentTable[0].FragmentBuffer = respBuf;
                                EFI_UDP4_COMPLETION_TOKEN TxToken2; ZeroMem(&TxToken2, sizeof(TxToken2));
                                EFI_EVENT TxEvent2 = NULL;
                                Status = gBS->CreateEvent(0, TPL_CALLBACK, NULL, NULL, &TxEvent2);
                                if (!EFI_ERROR(Status)) TxToken2.Event = TxEvent2;
                                TxToken2.Packet.TxData = TxData2;
                                Status = Udp->Transmit(Udp, &TxToken2);
                                if (!EFI_ERROR(Status)) {
                                    while (gBS->CheckEvent(TxEvent2)==EFI_NOT_READY) { Udp->Poll(Udp); gBS->Stall(1000); }
                                    Print(L"Transmitted %u bytes\n\n", (UINT32)rp);
                                } else {
                                    Print(L"[ERROR] Udp->Transmit response: %r\n", Status);
                                }
                                if (TxEvent2) gBS->CloseEvent(TxEvent2);
                                FreePool(TxData2);
                            } else {
                                Print(L"[ERROR] Out of memory to construct transmit buffer\n");
                            }

                            // If RESET command issued by BMC console, actually reset the system
                            if (cmd == IPMI_CMD_RESET) {
                                Print(L"[INFO] Received RESET request from BMC console. Resetting now...\n");
                                gRT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);
                                // does not return
                            }
                        } else if (netfn == IPMI_NETFN_RESP) {
                            // Response to a request UEFI previously sent (for example if UEFI did SendIpmiRequest earlier)
                            PrintIpmiResponse(buf, totalLen);
                            Print(L"[OK] Received %u bytes\n\n", totalLen);
                        }
                    }
                }
                // re-issue receive
                ZeroMem(&RxToken,sizeof(RxToken)); RxToken.Event = RxEvent;
                Status = Udp->Receive(Udp, &RxToken);
                if (EFI_ERROR(Status)) { Print(L"[WARN] Re-issue Udp->Receive failed: %r\n", Status); }
            } else {
                // no data or receive error: re-issue receive
                ZeroMem(&RxToken,sizeof(RxToken)); RxToken.Event = RxEvent;
                Status = Udp->Receive(Udp, &RxToken);
            }
        } else if (Index == 1) {
            // Console key pressed, read full ASCII command line
            ZeroMem(line, sizeof(line));
            if (EFI_ERROR(ReadAsciiLine(line, sizeof(line)))) continue;
            if (AsciiStrCmp(line, (CHAR8*)"exit") == 0) { Print(L"Exiting IpmiLanApp...\n"); break; }

            // Map ascii commands typed in UEFI to IPMI commands we send to server
            if (AsciiStrCmp(line, (CHAR8*)"bios")==0) SendIpmiRequest(Udp, IPMI_CMD_BIOS_INFO);
            else if (AsciiStrCmp(line, (CHAR8*)"netboot")==0) SendIpmiRequest(Udp, IPMI_CMD_NETBOOT_INFO);
            else if (AsciiStrCmp(line, (CHAR8*)"secureboot")==0) SendIpmiRequest(Udp, IPMI_CMD_SECUREBOOT_INFO);
            else if (AsciiStrCmp(line, (CHAR8*)"deviceid")==0) SendIpmiRequest(Udp, IPMI_CMD_GET_DEVICE_ID);
            else if (AsciiStrCmp(line, (CHAR8*)"chassis")==0) SendIpmiRequest(Udp, IPMI_CMD_GET_CHASSIS_STATUS);
            else if (AsciiStrCmp(line, (CHAR8*)"fru")==0) SendIpmiRequest(Udp, IPMI_CMD_FRU);
            else if (AsciiStrCmp(line, (CHAR8*)"sel_view")==0) SendIpmiRequest(Udp, IPMI_CMD_SEL_VIEW);
            else if (AsciiStrCmp(line, (CHAR8*)"sel_clear")==0) SendIpmiRequest(Udp, IPMI_CMD_SEL_CLEAR);
            else if (AsciiStrCmp(line, (CHAR8*)"power_on")==0) SendIpmiRequest(Udp, IPMI_CMD_POWER_ON);
            else if (AsciiStrCmp(line, (CHAR8*)"power_off")==0) SendIpmiRequest(Udp, IPMI_CMD_POWER_OFF);
            else if (AsciiStrCmp(line, (CHAR8*)"power_cycle")==0) SendIpmiRequest(Udp, IPMI_CMD_POWER_CYCLE);
            else if (AsciiStrCmp(line, (CHAR8*)"reset")==0) SendIpmiRequest(Udp, IPMI_CMD_RESET);
            else if (AsciiStrCmp(line, (CHAR8*)"bmc_status_network")==0) {
    UINT8 data[1] = { BMC_FEATURE_NETWORK };
    SendIpmiRequestWithData(Udp, IPMI_CMD_BMC_FEATURE_GET, data, 1);
}
else if (AsciiStrCmp(line, (CHAR8*)"bmc_enable_network")==0) {
    UINT8 data[2] = { BMC_FEATURE_NETWORK, 1 };
    SendIpmiRequestWithData(Udp, IPMI_CMD_BMC_FEATURE_SET, data, 2);
}
else if (AsciiStrCmp(line, (CHAR8*)"bmc_disable_network")==0) {
    UINT8 data[2] = { BMC_FEATURE_NETWORK, 0 };
    SendIpmiRequestWithData(Udp, IPMI_CMD_BMC_FEATURE_SET, data, 2);
}
            // 🔹 NEW: BMC feature control commands
            else if (AsciiStrCmp(line, (CHAR8*)"bmc_enable_kcs")==0) {
                UINT8 data[2] = { BMC_FEATURE_KCS, 1 };
                SendIpmiRequestWithData(Udp, IPMI_CMD_BMC_FEATURE_SET, data, 2);
            }
            else if (AsciiStrCmp(line, (CHAR8*)"bmc_disable_kcs")==0) {
                UINT8 data[2] = { BMC_FEATURE_KCS, 0 };
                SendIpmiRequestWithData(Udp, IPMI_CMD_BMC_FEATURE_SET, data, 2);
            }
            else if (AsciiStrCmp(line, (CHAR8*)"bmc_status_kcs")==0) {
                UINT8 data[1] = { BMC_FEATURE_KCS };
                SendIpmiRequestWithData(Udp, IPMI_CMD_BMC_FEATURE_GET, data, 1);
            }
            else if (AsciiStrCmp(line, (CHAR8*)"bmc_enable_hostif")==0) {
                UINT8 data[2] = { BMC_FEATURE_HOST_IF, 1 };
                SendIpmiRequestWithData(Udp, IPMI_CMD_BMC_FEATURE_SET, data, 2);
            }
            else if (AsciiStrCmp(line, (CHAR8*)"bmc_disable_hostif")==0) {
                UINT8 data[2] = { BMC_FEATURE_HOST_IF, 0 };
                SendIpmiRequestWithData(Udp, IPMI_CMD_BMC_FEATURE_SET, data, 2);
            }
            else if (AsciiStrCmp(line, (CHAR8*)"bmc_status_hostif")==0) {
                UINT8 data[1] = { BMC_FEATURE_HOST_IF };
                SendIpmiRequestWithData(Udp, IPMI_CMD_BMC_FEATURE_GET, data, 1);
            }

            else Print(L"Unknown command: %a\n", line);
        }
    }

cleanup:
    if (Child && UdpSb) UdpSb->DestroyChild(UdpSb, Child);
    return EFI_SUCCESS;
}


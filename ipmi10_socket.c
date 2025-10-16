/*
 ipmi10_socket.c
 Fake BMC (UDP) — interactive console.
 - Listens on UDP port 50891
 - When it receives a request (NetFn=0x06) from UEFI it replies with appropriate data (server-side behavior).
 - When you type commands on the console (bios, fru, sel_view, sel_clear, power_on, power_off, power_cycle, reset, netboot, secureboot, deviceid, chassis)
   it sends a request (2 bytes) to the last seen UEFI client, waits for the response, and prints it.
 - While waiting for a reply, it still responds to incoming UEFI requests (so both sides can be active).
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <stdint.h>
#include <sys/select.h>
#include <errno.h>

#define SERVER_PORT 50891
#define BUFFER_SIZE 2048
#define RECV_TIMEOUT_SEC 5

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
#define IPMI_CMD_FRU                 0x0B

#define IPMI_RSP_OK                 0x00
#define IPMI_RSP_INVALID_CMD        0xC1

#define IPMI_CMD_BMC_FEATURE_SET    0x0C
#define IPMI_CMD_BMC_FEATURE_GET    0x0D
#define BMC_FEATURE_KCS              0x01
#define BMC_FEATURE_HOST_IF          0x02

#define IPMI_CMD_BMC_NET_SET      0x0E
#define IPMI_CMD_BMC_NET_GET      0x0F
#define BMC_FEATURE_NETWORK       0x03

typedef struct {
    uint8_t netfn;
    uint8_t cmd;
    uint8_t data[256];
} __attribute__((packed)) IPMIRequest;

typedef struct {
    uint8_t netfn;
    uint8_t cmd;
    uint8_t completion_code;
    uint8_t data[254];
} __attribute__((packed)) IPMIResponse;

//static int bmc_kcs_enabled = 0;
//static int bmc_hostif_enabled = 0;

static int g_kcs_enabled = 1;      // default enabled
static int g_hostif_enabled = 1;   // default enabled

static int g_network_enabled = 1;  // default enabled
 
const char* get_command_name(uint8_t cmd) {
    switch(cmd){
        case IPMI_CMD_GET_CHASSIS_STATUS: return "get_chassis_status";
        case IPMI_CMD_GET_DEVICE_ID:      return "get_device_id";
        case IPMI_CMD_BIOS_INFO:          return "bios_info";
        case IPMI_CMD_NETBOOT_INFO:       return "netboot_info";
        case IPMI_CMD_SECUREBOOT_INFO:    return "secureboot_info";
        case IPMI_CMD_RESET:              return "reset";
        case IPMI_CMD_FRU:                return "fru";
        case IPMI_CMD_SEL_VIEW:           return "sel_view";
        case IPMI_CMD_SEL_CLEAR:          return "sel_clear";
        case IPMI_CMD_POWER_ON:           return "power_on";
        case IPMI_CMD_POWER_OFF:          return "power_off";
        case IPMI_CMD_POWER_CYCLE:        return "power_cycle";
        case IPMI_CMD_BMC_FEATURE_SET:    return "bmc_feature_set";
        case IPMI_CMD_BMC_FEATURE_GET:    return "bmc_feature_get";
        case IPMI_CMD_BMC_NET_SET: return "bmc_net_set";
        case IPMI_CMD_BMC_NET_GET: return "bmc_net_get";
        default:                          return "unknown";
    }
}


/* --- server-side helper functions that can produce responses if UEFI sent a REQUEST to the fake BMC.
       In practice, the socket code will BOTH:
         - reply to UEFI requests (when UEFI sends requests to the server),
         - send requests to UEFI (when operator types a command), and print replies.
*/

static void safe_read_first_line(const char *path, char *out, size_t max) {
    out[0]=0;
    FILE *f = fopen(path,"r");
    if (!f) { snprintf(out, max, "N/A"); return; }
    if (fgets(out, max, f)==NULL) out[0]=0;
    fclose(f);
    out[strcspn(out,"\r\n")] = 0;
}

void build_response_for_request_from_server(const IPMIRequest *req, IPMIResponse *resp) {
    memset(resp,0,sizeof(*resp));
    resp->netfn = req->netfn + 1;
    resp->cmd = req->cmd;
    resp->completion_code = IPMI_RSP_OK;

    switch(req->cmd) {
        case IPMI_CMD_GET_CHASSIS_STATUS: {
            // try to detect VM state for simulation; else OFF
            FILE *fp = popen("virsh domstate ipmi-vm 2>/dev/null","r");
            char state[128] = {0};
            if (fp && fgets(state, sizeof(state), fp)) {
                resp->data[0] = (strstr(state,"running") ? 0x01 : 0x00);
                printf("Reply: Chassis Status = %s (from host)\n", resp->data[0] ? "ON" : "OFF");
                pclose(fp);
            } else {
                resp->data[0] = 0x00;
                printf("Reply: Chassis Status = OFF (default)\n");
            }
            break;
        }
        case IPMI_CMD_GET_DEVICE_ID: {
            resp->data[0] = 0x20; resp->data[1] = 0x81; resp->data[2] = 0x01;
            resp->data[3] = 0x00; resp->data[4] = 0x02; resp->data[5] = 0x00;
            resp->data[6] = 0x34; resp->data[7] = 0x12; resp->data[8] = 0x00; resp->data[9] = 0x01;
            break;
        }
        case IPMI_CMD_BIOS_INFO: {
            char v[64], ver[64], date[64];
            safe_read_first_line("/sys/class/dmi/id/bios_vendor", v, sizeof(v));
            safe_read_first_line("/sys/class/dmi/id/bios_version", ver, sizeof(ver));
            safe_read_first_line("/sys/class/dmi/id/bios_date", date, sizeof(date));
            snprintf((char*)resp->data, sizeof(resp->data), "Vendor=%s;Version=%s;Date=%s", v, ver, date);
            break;
        }
        case IPMI_CMD_NETBOOT_INFO: {
            FILE *f = fopen("/proc/cmdline","r");
            if (f) {
                size_t r = fread(resp->data,1,sizeof(resp->data)-1,f);
                resp->data[r] = 0;
                fclose(f);
            } else {
                strcpy((char*)resp->data,"NetBoot=Unknown");
            }
            break;
        }
        case IPMI_CMD_SECUREBOOT_INFO: {
            if (access("/sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c",F_OK)==0)
                strncpy((char*)resp->data,"SecureBoot=Enabled", sizeof(resp->data)-1);
            else
                strncpy((char*)resp->data,"SecureBoot=Disabled", sizeof(resp->data)-1);
            break;
        }
        case IPMI_CMD_FRU: {
            char product[128], board[128], serial[128];
            safe_read_first_line("/sys/class/dmi/id/product_name", product, sizeof(product));
            safe_read_first_line("/sys/class/dmi/id/board_name", board, sizeof(board));
            safe_read_first_line("/sys/class/dmi/id/board_serial", serial, sizeof(serial));
            snprintf((char*)resp->data, sizeof(resp->data), "FRU: Product=%s;Board=%s;SN=%s",
                     product[0]?product:"unknown", board[0]?board:"unknown", serial[0]?serial:"unknown");
            break;
        }
        case IPMI_CMD_SEL_VIEW: {
            FILE *f = fopen("/var/log/syslog","r");
            if (!f) { strncpy((char*)resp->data,"SEL: Cannot read syslog", sizeof(resp->data)-1); break; }
            size_t n=fread(resp->data,1,sizeof(resp->data)-1,f); resp->data[n]=0; fclose(f);
            break;
        }
        case IPMI_CMD_SEL_CLEAR:
            strncpy((char*)resp->data,"SEL: Cleared (server-side simulated)", sizeof(resp->data)-1);
            break;
        case IPMI_CMD_POWER_ON:
            strncpy((char*)resp->data,"Power: ON (server simulated)", sizeof(resp->data)-1);
            break;
        case IPMI_CMD_POWER_OFF:
            strncpy((char*)resp->data,"Power: OFF (server simulated)", sizeof(resp->data)-1);
            break;
        case IPMI_CMD_POWER_CYCLE:
            strncpy((char*)resp->data,"Power: CYCLE (server simulated)", sizeof(resp->data)-1);
            break;
        case IPMI_CMD_RESET:
            strncpy((char*)resp->data,"Reset: Initiated (server simulated)", sizeof(resp->data)-1);
            break;
case IPMI_CMD_BMC_FEATURE_SET: {
    uint8_t fid = req->data[0];
    uint8_t val = req->data[1];
    if (fid == BMC_FEATURE_KCS) g_kcs_enabled = val;
    else if (fid == BMC_FEATURE_HOST_IF) g_hostif_enabled = val;
    else if (fid == BMC_FEATURE_NETWORK) g_network_enabled = val;  // <-- add this
    resp->data[0] = val;
    break;
}
case IPMI_CMD_BMC_FEATURE_GET: {
    uint8_t fid = req->data[0];
    if (fid == BMC_FEATURE_KCS) resp->data[0] = g_kcs_enabled;
    else if (fid == BMC_FEATURE_HOST_IF) resp->data[0] = g_hostif_enabled;
    else if (fid == BMC_FEATURE_NETWORK) resp->data[0] = g_network_enabled; // <-- add this
    break;
}

        default:
            resp->completion_code = IPMI_RSP_INVALID_CMD;
            strncpy((char*)resp->data,"Unsupported", sizeof(resp->data)-1);
            break;
    }
}

int process_incoming_packet(int sockfd, uint8_t *buf, ssize_t n, struct sockaddr_in *src, socklen_t srclen) {
    if (n < 2) return 0;
    uint8_t netfn = buf[0];
    uint8_t cmd   = buf[1];

    if (netfn == IPMI_NETFN_APP) {
        printf("Decoded Request: netfn=0x%02x cmd=0x%02x (%s)\n", netfn, cmd, get_command_name(cmd));
        IPMIRequest req; IPMIResponse resp;
        memset(&req,0,sizeof(req));
        req.netfn = buf[0]; req.cmd = buf[1];
        if (n > 2) {
            size_t copylen = (size_t)n - 2;
            if (copylen > sizeof(req.data)) copylen = sizeof(req.data);
            memcpy(req.data, buf + 2, copylen);
        }
        build_response_for_request_from_server(&req, &resp);
        ssize_t sent = sendto(sockfd, &resp, sizeof(resp), 0, (struct sockaddr*)src, srclen);
        if (sent < 0) perror("sendto");
        else printf("Transmitted %zd bytes\n\n", sent);
        printf("Type commands here: bios | netboot | secureboot | deviceid | chassis | fru | sel_view | sel_clear | power_on | power_off | power_cycle | reset | bmc_enable_kcs | bmc_disable_kcs | bmc_status_kcs | bmc_enable_hostif | bmc_disable_hostif | bmc_status_hostif | bmc_disable_network | bmc_status_network | bmc_enable_network | exit\n\n");
        return 1;
    } else if (netfn == IPMI_NETFN_RESP) {
        if (n < 3) {
            printf("Reply: (invalid short response)\n");
            return 2;
        }
        uint8_t cc = buf[2];
        if (cc != IPMI_RSP_OK) {
            printf("Reply: completion code 0x%02x\n", cc);
        }
        if (cmd == IPMI_CMD_GET_CHASSIS_STATUS) {
            uint8_t ch = (n > 3) ? buf[3] : 0;
            printf("Reply: Chassis Power = %s\n", ch ? "ON" : "OFF");
        } else if (cmd == IPMI_CMD_GET_DEVICE_ID) {
            ssize_t payload_len = n - 3;
            printf("Reply: Device ID bytes =");
            for (int i=0; i<payload_len; i++) {
                printf(" %02X", buf[3+i]);
            }
            printf("\n");
        } else if (cmd == IPMI_CMD_BMC_FEATURE_GET || cmd == IPMI_CMD_BMC_FEATURE_SET || cmd == IPMI_CMD_BMC_NET_GET || cmd == IPMI_CMD_BMC_NET_SET) {
    if (n >= 4) {
        printf("Reply: %s\n", buf[3] ? "ENABLED" : "DISABLED");
    } else {
        printf("Reply: (invalid feature reply)\n");
    }
}
else if (cmd == IPMI_CMD_BIOS_INFO || cmd == IPMI_CMD_NETBOOT_INFO || cmd == IPMI_CMD_SECUREBOOT_INFO ||
                   cmd == IPMI_CMD_FRU || cmd == IPMI_CMD_SEL_VIEW || cmd == IPMI_CMD_SEL_CLEAR ||
                   cmd == IPMI_CMD_POWER_ON || cmd == IPMI_CMD_POWER_OFF || cmd == IPMI_CMD_POWER_CYCLE ||
                   cmd == IPMI_CMD_RESET) {
            char tmp[1024]; memset(tmp,0,sizeof(tmp));
            ssize_t payload_len = n - 3;
            if (payload_len > 0) {
                if (payload_len > (ssize_t)(sizeof(tmp)-1)) payload_len = sizeof(tmp)-1;
                memcpy(tmp, buf + 3, payload_len);
            }
            printf("Reply: %s\n", tmp);
        } else {
            printf("Reply: Unknown cmd 0x%02x\n", cmd);
        }
        printf("[OK] Received %zd bytes\n\n", n);
        return 2;
    }
    return 0;
}

void send_request_to_lastclient_and_wait(int sockfd, struct sockaddr_in *last_client, socklen_t last_client_len, const char *cmdstr) {
    if (!last_client) { printf("No UEFI client seen yet — cannot send request\n\n"); return; }

    uint8_t reqbuf[8];
    int sendlen = 2;
    reqbuf[0] = IPMI_NETFN_APP;
    if      (strcmp(cmdstr,"bios")==0)       reqbuf[1] = IPMI_CMD_BIOS_INFO;
    else if (strcmp(cmdstr,"netboot")==0)    reqbuf[1] = IPMI_CMD_NETBOOT_INFO;
    else if (strcmp(cmdstr,"secureboot")==0) reqbuf[1] = IPMI_CMD_SECUREBOOT_INFO;
    else if (strcmp(cmdstr,"deviceid")==0)   reqbuf[1] = IPMI_CMD_GET_DEVICE_ID;
    else if (strcmp(cmdstr,"chassis")==0)    reqbuf[1] = IPMI_CMD_GET_CHASSIS_STATUS;
    else if (strcmp(cmdstr,"fru")==0)        reqbuf[1] = IPMI_CMD_FRU;
    else if (strcmp(cmdstr,"sel_view")==0)   reqbuf[1] = IPMI_CMD_SEL_VIEW;
    else if (strcmp(cmdstr,"sel_clear")==0)  reqbuf[1] = IPMI_CMD_SEL_CLEAR;
    else if (strcmp(cmdstr,"power_on")==0)   reqbuf[1] = IPMI_CMD_POWER_ON;
    else if (strcmp(cmdstr,"power_off")==0)  reqbuf[1] = IPMI_CMD_POWER_OFF;
    else if (strcmp(cmdstr,"power_cycle")==0)reqbuf[1] = IPMI_CMD_POWER_CYCLE;
    else if (strcmp(cmdstr,"reset")==0)      reqbuf[1] = IPMI_CMD_RESET;
    else if (strcmp(cmdstr,"bmc_enable_kcs")==0)    { reqbuf[1]=IPMI_CMD_BMC_FEATURE_SET; reqbuf[2]=BMC_FEATURE_KCS; reqbuf[3]=1; sendlen=4; }
    else if (strcmp(cmdstr,"bmc_disable_kcs")==0)   { reqbuf[1]=IPMI_CMD_BMC_FEATURE_SET; reqbuf[2]=BMC_FEATURE_KCS; reqbuf[3]=0; sendlen=4; }
    else if (strcmp(cmdstr,"bmc_status_kcs")==0)    { reqbuf[1]=IPMI_CMD_BMC_FEATURE_GET; reqbuf[2]=BMC_FEATURE_KCS; sendlen=3; }
    else if (strcmp(cmdstr,"bmc_enable_hostif")==0) { reqbuf[1]=IPMI_CMD_BMC_FEATURE_SET; reqbuf[2]=BMC_FEATURE_HOST_IF; reqbuf[3]=1; sendlen=4; }
    else if (strcmp(cmdstr,"bmc_disable_hostif")==0){ reqbuf[1]=IPMI_CMD_BMC_FEATURE_SET; reqbuf[2]=BMC_FEATURE_HOST_IF; reqbuf[3]=0; sendlen=4; }
    else if (strcmp(cmdstr,"bmc_status_hostif")==0) { reqbuf[1]=IPMI_CMD_BMC_FEATURE_GET; reqbuf[2]=BMC_FEATURE_HOST_IF; sendlen=3; }
    /* IMPORTANT: use the same FEATURE_SET/FEATURE_GET message format for NETWORK as for KCS/HostIF.
       Previously you had mixed IPMI_CMD_BMC_NET_* and FEATURE_*; that caused mismatches. */
    else if (strcmp(cmdstr,"bmc_enable_network")==0)  { reqbuf[1]=IPMI_CMD_BMC_FEATURE_SET; reqbuf[2]=BMC_FEATURE_NETWORK; reqbuf[3]=1; sendlen=4; }
    else if (strcmp(cmdstr,"bmc_disable_network")==0) { reqbuf[1]=IPMI_CMD_BMC_FEATURE_SET; reqbuf[2]=BMC_FEATURE_NETWORK; reqbuf[3]=0; sendlen=4; }
    else if (strcmp(cmdstr,"bmc_status_network")==0)  { reqbuf[1]=IPMI_CMD_BMC_FEATURE_GET; reqbuf[2]=BMC_FEATURE_NETWORK; sendlen=3; }
    else { printf("Unknown client command: %s\n\n", cmdstr); return; }

    ssize_t sent = sendto(sockfd, reqbuf, sendlen, 0, (struct sockaddr*)last_client, last_client_len);
    if (sent < 0) { perror("sendto"); return; }
    printf("Transmitted %zd bytes\n\n", sent);

    fd_set rfds; struct timeval tv; tv.tv_sec = RECV_TIMEOUT_SEC; tv.tv_usec = 0;
    uint8_t inbuf[BUFFER_SIZE]; struct sockaddr_in src; socklen_t srclen = sizeof(src);

    while (1) {
        FD_ZERO(&rfds); FD_SET(sockfd, &rfds);
        int rv = select(sockfd + 1, &rfds, NULL, NULL, &tv);
        if (rv <= 0) { printf("[WARN] No reply received in %d seconds\n\n", RECV_TIMEOUT_SEC); return; }
        memset(inbuf, 0, BUFFER_SIZE);
        if (FD_ISSET(sockfd, &rfds)) {
            ssize_t n = recvfrom(sockfd, inbuf, sizeof(inbuf), 0, (struct sockaddr*)&src, &srclen);
            if (n <= 0) { if (n<0) perror("recvfrom"); continue; }
            int r = process_incoming_packet(sockfd, inbuf, n, &src, srclen);
            if (r == 2) return;
        }
    }
}

int main(void){
    int sockfd;
    struct sockaddr_in servaddr, cliaddr; socklen_t cli_len = sizeof(cliaddr);
    uint8_t buf[BUFFER_SIZE];

    struct sockaddr_in last_client; socklen_t last_client_len = 0; int have_last_client = 0;

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) { perror("socket"); exit(EXIT_FAILURE); }

    memset(&servaddr,0,sizeof(servaddr));
    servaddr.sin_family = AF_INET; servaddr.sin_addr.s_addr = INADDR_ANY; servaddr.sin_port = htons(SERVER_PORT);
    if (bind(sockfd, (struct sockaddr*)&servaddr, sizeof(servaddr)) < 0) { perror("bind failed"); close(sockfd); exit(EXIT_FAILURE); }

    printf("Fake BMC started on UDP port %d...\n", SERVER_PORT);
    printf("Console commands: bios | netboot | secureboot | deviceid | chassis | fru | sel_view | sel_clear | power_on | power_off | power_cycle | reset | bmc_enable_kcs | bmc_disable_kcs | bmc_status_kcs | bmc_enable_hostif | bmc_disable_hostif | bmc_status_hostif | bmc_disable_network | bmc_status_network | bmc_enable_network | exit\n\n");

    while (1) {
        fd_set readfds; FD_ZERO(&readfds); FD_SET(sockfd, &readfds); FD_SET(STDIN_FILENO, &readfds);
        int maxfd = (sockfd > STDIN_FILENO) ? sockfd : STDIN_FILENO;
        int activity = select(maxfd + 1, &readfds, NULL, NULL, NULL);
        if (activity < 0) { if (errno == EINTR) continue; perror("select"); break; }

        if (FD_ISSET(sockfd, &readfds)) {
            ssize_t n = recvfrom(sockfd, buf, sizeof(buf), 0, (struct sockaddr*)&cliaddr, &cli_len);
            if (n > 0) {
                last_client = cliaddr; last_client_len = cli_len; have_last_client = 1;
                process_incoming_packet(sockfd, buf, n, &cliaddr, cli_len);
            }
        }

        if (FD_ISSET(STDIN_FILENO, &readfds)) {
            char line[128];
            if (fgets(line, sizeof(line), stdin) == NULL) break;
            line[strcspn(line, "\r\n")] = 0;
            if (strcmp(line,"exit")==0) { printf("Exiting Fake BMC...\n"); break; }
            if (!have_last_client) { printf("No UEFI client seen yet — cannot send request\n\n"); continue; }
            // allowed console commands
            if (strcmp(line,"bios")==0 || strcmp(line,"netboot")==0 || strcmp(line,"secureboot")==0 ||
                strcmp(line,"deviceid")==0 || strcmp(line,"chassis")==0 || strcmp(line,"fru")==0 ||
                strcmp(line,"sel_view")==0 || strcmp(line,"sel_clear")==0 ||
                strcmp(line,"power_on")==0 || strcmp(line,"power_off")==0 || strcmp(line,"power_cycle")==0 ||
                strcmp(line,"reset")==0 || 
                strcmp(line,"bmc_enable_kcs")==0 || strcmp(line,"bmc_disable_kcs")==0 || strcmp(line,"bmc_status_kcs")==0 ||
                strcmp(line,"bmc_enable_hostif")==0 || strcmp(line,"bmc_disable_hostif")==0 || strcmp(line,"bmc_status_hostif")==0 || strcmp(line,"bmc_status_network")==0 || strcmp(line,"bmc_enable_network")==0 || strcmp(line,"bmc_disable_network")==0) {
                send_request_to_lastclient_and_wait(sockfd, &last_client, last_client_len, line);
            } else {
                printf("Unknown command: %s\n", line);
            }
        }
    }

    close(sockfd);
    return 0;
}


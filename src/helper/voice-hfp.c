// PaperPro 拼音：蓝牙免提麦克风录音（HFP 音频网关，平板扮演“手机”）。
//
// 平板的内核有 L2CAP 与 SCO，但没有 RFCOMM，所以免提协议所需的 RFCOMM 在这里
// 用 L2CAP 实现（TS 07.10 的一个子集：基本模式、按信用量的流控）。流程：
//   1. 连接麦克风的 RFCOMM 通道（SDP 记录里的 Handsfree 通道）；
//   2. 回应麦克风发来的 AT 指令，建立服务级连接（SLC）；
//   3. 打开 SCO 语音链路，读取 8 kHz、16 位单声道 PCM（窄带 CVSD，由控制器解码）。
//
// 用法：voice-hfp <地址> <通道> [秒数]
//   给秒数：测试用，连上就录这么久，PCM 原样写到标准输出。
//   不给秒数：常驻，供输入法模块使用（协议见 main 中的说明）。
//   进度都写到标准错误。
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif
#define BTPROTO_L2CAP 0
#define BTPROTO_SCO 2
#define SOL_BLUETOOTH 274
#define SOL_SCO 17
#define SCO_OPTIONS 0x01
#define BT_SECURITY 4
#define BT_SECURITY_MEDIUM 2
#define BT_VOICE 11
#define BT_VOICE_CVSD_16BIT 0x0060

typedef struct { uint8_t b[6]; } __attribute__((packed)) bdaddr_t;
struct sockaddr_l2 {
    sa_family_t l2_family;
    uint16_t l2_psm;
    bdaddr_t l2_bdaddr;
    uint16_t l2_cid;
    uint8_t l2_bdaddr_type;
};
struct sockaddr_sco {
    sa_family_t sco_family;
    bdaddr_t sco_bdaddr;
};
struct bt_security { uint8_t level, key_size; };
struct bt_voice { uint16_t setting; };
struct sco_options { uint16_t mtu; };
#define BTPROTO_HCI 1
#define SOL_L2CAP 6
#define L2CAP_CONNINFO 0x02
struct l2cap_conninfo { uint16_t hci_handle; uint8_t dev_class[3]; };
struct sockaddr_hci { sa_family_t hci_family; unsigned short hci_dev, hci_channel; };

// RFCOMM 帧类型（控制字段，不含 P/F 位）
enum { SABM = 0x2f, UA = 0x63, DM = 0x0f, DISC = 0x43, UIH = 0xef, PF = 0x10 };
// 多路复用控制命令
enum { MCC_PN = 0x20, MCC_MSC = 0x38, MCC_RPN = 0x24, MCC_TEST = 0x08, MCC_NSC = 0x04 };

static int l2 = -1, sco = -1;
static int dlci;                 // 免提通道的 DLCI = 通道号 × 2
static int cfc = 0;              // 是否用信用量流控
static int tx_credits = 0;       // 还能发多少帧
static int rx_given = 0;         // 已给对方、尚未用掉的信用量
static uint8_t crc_table[256];

static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("voice-hfp: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int parse_addr(const char *s, bdaddr_t *a) {
    unsigned v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return -1;
    for (int i = 0; i < 6; i++) a->b[5 - i] = (uint8_t)v[i];
    return 0;
}

// ---- RFCOMM 帧 --------------------------------------------------------------------
static void crc_init(void) {
    for (int i = 0; i < 256; i++) {
        uint8_t c = (uint8_t)i;
        for (int b = 0; b < 8; b++) c = (c & 1) ? (uint8_t)((c >> 1) ^ 0xe0) : (uint8_t)(c >> 1);
        crc_table[i] = c;
    }
}
static uint8_t fcs(const uint8_t *p, int n) {
    uint8_t f = 0xff;
    while (n--) f = crc_table[f ^ *p++];
    return (uint8_t)(0xff - f);
}

// 本机是发起方：命令与数据帧的 C/R 位为 1。credits >= 0 时带上信用量（UIH 的 P/F 位）。
static int send_frame(int d, int ctrl, const uint8_t *data, int len, int credits) {
    uint8_t b[512];
    int n = 0;
    b[n++] = (uint8_t)((d << 2) | 0x02 | 0x01);
    int c = ctrl;
    if (ctrl == SABM || ctrl == DISC) c |= PF;
    if (ctrl == UIH && credits >= 0) c |= PF;
    b[n++] = (uint8_t)c;
    if (len < 128) {
        b[n++] = (uint8_t)((len << 1) | 1);
    } else {
        b[n++] = (uint8_t)((len & 0x7f) << 1);
        b[n++] = (uint8_t)(len >> 7);
    }
    int hdr = n;
    if (ctrl == UIH && credits >= 0) b[n++] = (uint8_t)credits;
    if (len) memcpy(b + n, data, len);
    n += len;
    b[n++] = ctrl == UIH ? fcs(b, 2) : fcs(b, hdr);
    if (write(l2, b, n) != n) {
        say("write: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static int send_mcc(int type, int command, const uint8_t *v, int len) {
    uint8_t b[64];
    b[0] = (uint8_t)((type << 2) | (command ? 0x02 : 0) | 0x01);
    b[1] = (uint8_t)((len << 1) | 1);
    memcpy(b + 2, v, len);
    return send_frame(0, UIH, b, len + 2, -1);
}

// 发给免提通道的数据（AT 回应）。用信用量流控时顺便补足给对方的信用量。
static int send_data(const char *s) {
    if (cfc && tx_credits <= 0) say("no credits left, sending anyway");
    int give = -1;
    if (cfc) {
        give = 10 - rx_given;
        if (give < 0) give = 0;
        rx_given += give;
        tx_credits--;
    }
    return send_frame(dlci, UIH, (const uint8_t *)s, (int)strlen(s), give);
}

static void reply(const char *s) {
    char b[256];
    snprintf(b, sizeof b, "\r\n%s\r\n", s);
    say("AG> %s", s);
    send_data(b);
}

// ---- 免提协议（AG 一侧） ---------------------------------------------------------------
static int slc = 0;
static char at_buf[512];
static int at_len = 0;

static void at_command(const char *cmd) {
    say("HF< %s", cmd);
    if (!strncmp(cmd, "AT+BRSF=", 8)) {
        // 不声明编解码协商、三方通话等：只用窄带语音，握手最短。
        reply("+BRSF: 0");
        reply("OK");
    } else if (!strcmp(cmd, "AT+CIND=?")) {
        reply("+CIND: (\"service\",(0,1)),(\"call\",(0,1)),(\"callsetup\",(0-3)),(\"callheld\",(0-2)),"
              "(\"signal\",(0-5)),(\"roam\",(0,1)),(\"battchg\",(0-5))");
        reply("OK");
    } else if (!strcmp(cmd, "AT+CIND?")) {
        reply("+CIND: 1,0,0,0,5,0,5");
        reply("OK");
    } else if (!strncmp(cmd, "AT+CMER", 7)) {
        reply("OK");
        slc = 1;
    } else if (!strcmp(cmd, "AT+CHLD=?")) {
        reply("+CHLD: (0,1,2,3)");
        reply("OK");
    } else if (!strcmp(cmd, "AT+COPS?")) {
        reply("+COPS: 0,0,\"PaperPro\"");
        reply("OK");
    } else if (!strncmp(cmd, "AT+BCS=", 7) || !strncmp(cmd, "AT+BAC=", 7)) {
        reply("OK");
    } else {
        // 其余（音量、电量、来电显示等）一律接受。
        reply("OK");
    }
}

static void at_data(const uint8_t *p, int n) {
    for (int i = 0; i < n; i++) {
        char c = (char)p[i];
        if (c == '\r' || c == '\n') {
            if (at_len) {
                at_buf[at_len] = 0;
                at_command(at_buf);
                at_len = 0;
            }
        } else if (at_len < (int)sizeof at_buf - 1) {
            at_buf[at_len++] = c;
        }
    }
}

// ---- 收帧 -----------------------------------------------------------------------------
enum { ST_MUX, ST_PN, ST_DLC, ST_OPEN };
static int state = ST_MUX;
static int msc_sent = 0;

static void open_dlc(void) {
    uint8_t pn[8] = {(uint8_t)dlci, 0xf0, 7, 0, 127, 0, 0, 7};
    rx_given = 7;
    send_mcc(MCC_PN, 1, pn, 8);
    state = ST_PN;
}

static int handle_frame(const uint8_t *b, int n) {
    if (n < 4) return 0;
    int d = b[0] >> 2;
    int ctrl = b[1] & ~PF, pf = b[1] & PF;
    int len, hdr;
    if (b[2] & 1) {
        len = b[2] >> 1;
        hdr = 3;
    } else {
        len = (b[2] >> 1) | (b[3] << 7);
        hdr = 4;
    }
    if (ctrl == UIH && pf && d != 0) {
        tx_credits += b[hdr];
        hdr++;
    }
    if (hdr + len > n) return 0;
    const uint8_t *data = b + hdr;

    switch (ctrl) {
    case UA:
        if (d == 0 && state == ST_MUX) {
            say("multiplexer up");
            open_dlc();
        } else if (d == dlci && state == ST_DLC) {
            say("hands-free channel open (credits %d, flow control %s)", tx_credits, cfc ? "credit" : "none");
            state = ST_OPEN;
            uint8_t msc[2] = {(uint8_t)((dlci << 2) | 0x03), 0x8d};
            send_mcc(MCC_MSC, 1, msc, 2);
            msc_sent = 1;
        }
        break;
    case DM:
        say("refused on DLCI %d", d);
        return -1;
    case SABM:
        send_frame(d, UA, NULL, 0, -1);
        break;
    case DISC:
        send_frame(d, UA, NULL, 0, -1);
        say("peer closed DLCI %d", d);
        return -1;
    case UIH:
        if (d == 0) {
            if (len < 2) break;
            int type = data[0] >> 2, command = (data[0] >> 1) & 1, vlen = data[1] >> 1;
            const uint8_t *v = data + 2;
            if (type == MCC_PN && !command && state == ST_PN) {
                cfc = (v[1] & 0xf0) == 0xe0;
                if (cfc) tx_credits = v[7] & 0x07;
                send_frame(dlci, SABM, NULL, 0, -1);
                state = ST_DLC;
            } else if (type == MCC_PN && command) {
                uint8_t r[8];
                memcpy(r, v, 8);
                r[1] = 0xe0;
                r[7] = 7;
                send_mcc(MCC_PN, 0, r, 8);
            } else if (type == MCC_MSC && command) {
                send_mcc(MCC_MSC, 0, v, vlen);
            } else if (type == MCC_RPN && command) {
                send_mcc(MCC_RPN, 0, v, vlen);
            } else if (type == MCC_TEST && command) {
                send_mcc(MCC_TEST, 0, v, vlen);
            } else if (command) {
                uint8_t t = data[0];
                send_mcc(MCC_NSC, 0, &t, 1);
            }
        } else if (d == dlci) {
            if (cfc && len > 0) {
                rx_given--;
                if (rx_given < 4) {
                    int give = 10 - rx_given;
                    rx_given += give;
                    send_frame(dlci, UIH, NULL, 0, give);
                }
            }
            if (len > 0) at_data(data, len);
        }
        break;
    }
    return 0;
}

// ---- 不让连接进入省电模式 -------------------------------------------------------------
// 闲置几秒后，麦克风会把连接切到 Sniff 省电模式；之后要开语音链路时，退出
// Sniff 的请求得不到回应，整条连接超时断开（DJI Mic Mini 上实测如此）。所以
// 连上后就把这条连接的链路策略设为只允许主从切换、不允许 Sniff。
static void no_sniff(int l2sock) {
    struct l2cap_conninfo ci = {0};
    socklen_t len = sizeof ci;
    if (getsockopt(l2sock, SOL_L2CAP, L2CAP_CONNINFO, &ci, &len) < 0) {
        say("L2CAP_CONNINFO: %s", strerror(errno));
        return;
    }
    int h = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
    if (h < 0) {
        say("HCI socket: %s", strerror(errno));
        return;
    }
    struct sockaddr_hci ha = {AF_BLUETOOTH, 0, 0};
    if (bind(h, (struct sockaddr *)&ha, sizeof ha) < 0) {
        say("HCI bind: %s", strerror(errno));
        close(h);
        return;
    }
    // HCI 命令包：Write Link Policy Settings（OGF 0x02，OCF 0x000D）；策略 0x0001 = 只允许主从切换
    uint16_t op = (0x02 << 10) | 0x000d;
    uint8_t cmd[] = {0x01, (uint8_t)(op & 0xff), (uint8_t)(op >> 8), 4,
                     (uint8_t)(ci.hci_handle & 0xff), (uint8_t)(ci.hci_handle >> 8), 0x01, 0x00};
    if (write(h, cmd, sizeof cmd) != (ssize_t)sizeof cmd) say("HCI write: %s", strerror(errno));
    else say("link 0x%04x: sniff mode off", ci.hci_handle);
    close(h);
}

// ---- 主流程 -----------------------------------------------------------------------------
int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: voice-hfp ADDRESS CHANNEL [SECONDS]\n");
        return 2;
    }
    signal(SIGPIPE, SIG_IGN);
    crc_init();
    bdaddr_t addr;
    if (parse_addr(argv[1], &addr)) {
        say("bad address %s", argv[1]);
        return 2;
    }
    dlci = atoi(argv[2]) * 2;
    double seconds = argc > 3 ? atof(argv[3]) : 0;

    l2 = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_L2CAP);
    if (l2 < 0) {
        say("L2CAP socket: %s", strerror(errno));
        return 1;
    }
    struct bt_security sec = {BT_SECURITY_MEDIUM, 0};
    setsockopt(l2, SOL_BLUETOOTH, BT_SECURITY, &sec, sizeof sec);
    struct sockaddr_l2 la = {0};
    la.l2_family = AF_BLUETOOTH;
    la.l2_psm = 3;  // RFCOMM
    la.l2_bdaddr = addr;
    say("connecting to %s channel %d", argv[1], dlci / 2);
    if (connect(l2, (struct sockaddr *)&la, sizeof la) < 0) {
        int e = errno;
        say("L2CAP connect: %s", strerror(e));
        if (seconds <= 0) {
            // 最常见的两种：麦克风没开或太远；正连着手机、电脑或它的接收器。
            if (e == ECONNRESET || e == ECONNREFUSED)
                printf("S error 麦克风拒绝了连接，可能正连着其他设备\n");
            else
                printf("S error 连不上麦克风，请确认它已开机、在附近\n");
        }
        return 1;
    }
    no_sniff(l2);
    send_frame(0, SABM, NULL, 0, -1);

    // 常驻模式（不给秒数）：连好后待命，按标准输入的 start / stop / quit 打开、
    // 关闭语音链路。标准输出是给输入法模块的帧：
    //   S <状态>\n            ready / recording / stopped / error <原因>
    //   A <字节数>\n<PCM>      录到的音频（8 kHz、16 位、单声道）
    int daemon_mode = seconds <= 0;
    double start = now(), recording = 0;
    long bytes = 0, packets = 0;
    uint8_t buf[1024];
    int stop = 0, want_sco = !daemon_mode, ready_sent = 0;
    char in_buf[256];
    int in_len = 0;
    while (!stop) {
        struct pollfd p[3] = {{l2, POLLIN, 0}, {sco, POLLIN, 0}, {daemon_mode ? 0 : -1, POLLIN, 0}};
        if (poll(p, 3, 500) < 0 && errno != EINTR) break;
        if (p[0].revents & (POLLERR | POLLHUP)) {
            say("connection lost");
            break;
        }
        if (p[0].revents & POLLIN) {
            int n = (int)read(l2, buf, sizeof buf);
            if (n <= 0) {
                say("connection closed");
                break;
            }
            if (handle_frame(buf, n) < 0) break;
        }
        if (slc && !ready_sent) {
            ready_sent = 1;
            say("service level connection up");
            if (daemon_mode) {
                printf("S ready\n");
                fflush(stdout);
            }
        }
        if (slc && want_sco && sco < 0) {
            sco = socket(AF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_SCO);
            struct bt_voice voice = {BT_VOICE_CVSD_16BIT};
            if (setsockopt(sco, SOL_BLUETOOTH, BT_VOICE, &voice, sizeof voice) < 0)
                say("BT_VOICE: %s", strerror(errno));
            struct sockaddr_sco sa = {AF_BLUETOOTH, addr};
            if (connect(sco, (struct sockaddr *)&sa, sizeof sa) < 0) {
                say("SCO connect: %s", strerror(errno));
                close(sco);
                sco = -1;
                want_sco = 0;
                if (!daemon_mode) break;
                printf("S error 无法打开语音链路\n");
                fflush(stdout);
            } else {
                struct sco_options so = {0};
                socklen_t sl = sizeof so;
                getsockopt(sco, SOL_SCO, SCO_OPTIONS, &so, &sl);
                say("voice link open (mtu %u)", so.mtu);
                recording = now();
                packets = 0;
            }
        }
        if (sco >= 0 && (p[1].revents & POLLIN)) {
            int n = (int)read(sco, buf, sizeof buf);
            if (n > 0) {
                packets++;
                bytes += n;
                if (daemon_mode) {
                    if (packets == 1) printf("S recording\n");
                    printf("A %d\n", n);
                }
                if (fwrite(buf, 1, n, stdout) != (size_t)n) stop = 1;
                if (daemon_mode) fflush(stdout);
                // 同样大小的静音回送给麦克风一侧：有的控制器要双向都有数据才持续送音频。
                static uint8_t zero[1024];
                if (write(sco, zero, n) < 0) {}
                if (packets == 1) say("first audio packet: %d bytes", n);
            } else if (n < 0) {
                say("SCO read: %s", strerror(errno));
                p[1].revents |= POLLHUP;
            }
        }
        if (sco >= 0 && (p[1].revents & (POLLERR | POLLHUP))) {
            say("voice link closed by the microphone");
            close(sco);
            sco = -1;
            want_sco = 0;
            if (!daemon_mode) break;
            printf("S stopped\n");
            fflush(stdout);
        }
        if (daemon_mode && (p[2].revents & (POLLIN | POLLHUP))) {
            ssize_t n = read(0, in_buf + in_len, sizeof in_buf - 1 - in_len);
            if (n <= 0) break;  // 输入法模块已退出
            in_len += (int)n;
            in_buf[in_len] = 0;
            char *nl;
            while ((nl = strchr(in_buf, '\n'))) {
                *nl = 0;
                if (!strcmp(in_buf, "start")) {
                    if (!slc) {
                        printf("S error 麦克风还没连好\n");
                        fflush(stdout);
                    } else {
                        want_sco = 1;
                    }
                } else if (!strcmp(in_buf, "stop")) {
                    want_sco = 0;
                    if (sco >= 0) {
                        close(sco);
                        sco = -1;
                        say("voice link closed (%.1f s)", now() - recording);
                    }
                    printf("S stopped\n");
                    fflush(stdout);
                } else if (!strcmp(in_buf, "quit")) {
                    stop = 1;
                }
                in_len -= (int)(nl + 1 - in_buf);
                memmove(in_buf, nl + 1, in_len + 1);
            }
        }
        if (!daemon_mode && recording > 0 && now() - recording >= seconds) stop = 1;
        if (!slc && now() - start > 20) {
            say("timed out before the hands-free connection was up");
            if (daemon_mode) printf("S error 麦克风没有响应\n");
            break;
        }
    }
    fflush(stdout);
    say("recorded %ld bytes (%.1f s of 8 kHz audio)", bytes, bytes / 16000.0);
    if (sco >= 0) close(sco);
    if (state == ST_OPEN) send_frame(dlci, DISC, NULL, 0, -1);
    send_frame(0, DISC, NULL, 0, -1);
    usleep(200000);
    close(l2);
    return daemon_mode ? 0 : (bytes > 0 ? 0 : 1);
}

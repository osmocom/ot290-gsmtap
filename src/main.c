/* (C) 2012 by Thomas Bertani <mail@thomasbertani.it>
 *
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 */

#include <sys/file.h>
#include <termios.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "gsmtap.h"


static volatile int run = 1;
static int gsmtap_fd, serial_fd;

struct frame
{
    unsigned char AppID, FCS, AppMsg[8192];
    unsigned short AppMsgLength;
};

static void interrupt(int sign){ run = 0; }

static int serial_init(const char *serial_port)
{
    struct termios t;
    serial_fd = open(serial_port, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (serial_fd < 0)
    {
        char errstr[50];
        sprintf(errstr, "Failed to open serial port %s", serial_port);
        perror(errstr);
        return -1;
    }
    t.c_cflag = CBAUD | B115200 | CRTSCTS | CS8 | CREAD | CLOCAL;
    t.c_cc[VMIN] = 255;
    t.c_cc[VTIME] = 0;
    tcsetattr(serial_fd, TCSAFLUSH, &t);
    return 0;
}

static int serial_read(void *buf, size_t nbytes)
{
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(serial_fd, &readfds);
    struct timeval timeout = {0, 1000000};
    int actual = select(serial_fd+1, &readfds, NULL, NULL, &timeout);
    if(actual > 0)
    {
        actual = read(serial_fd, buf, nbytes);
        if( actual == 0 )
        {
            run = 0;
            return -1;
        }
    }
    if((actual == -1) && (errno != EINTR))
    {
        perror("serial_read");
        run = 0;
    }
    return actual;
}

static void gsmtap_open(const char *gsmtap_host)
{
    struct sockaddr_in sin;
    sin.sin_family= AF_INET;
    sin.sin_port = htons(GSMTAP_UDP_PORT);
    if (inet_aton(gsmtap_host, &sin.sin_addr) < 0) perror("parsing GSMTAP destination address");
    gsmtap_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (gsmtap_fd < 0) perror("GSMTAP socket initialization");
    if (connect(gsmtap_fd, (struct sockaddr *)&sin, sizeof(sin)) < 0) perror("connecting GSMTAP socket");
}

void testgsmtap(unsigned char *AppMsg, unsigned short AppMsgLength)
{
    int length = 0;
    if ((AppMsg[0] == 0x03) && (AppMsg[1] < 0x04)) //trace messages except for GPRS RLC/MAC header and RATSCCH trace ones
    {
        int format = (AppMsg[2] >> 1 & 0b1111);
        length = AppMsg[3] << 8 | AppMsg[4];
        const size_t buf_len = sizeof(struct gsmtap_hdr) + length;
        unsigned char buf[buf_len];
        struct gsmtap_hdr *gh = (struct gsmtap_hdr *) buf;    
        memset(gh, 0, sizeof(*gh));
        gh->version = GSMTAP_VERSION;
        gh->hdr_len = sizeof(*gh)/4;
        if ((AppMsg[2] & 1) == 1) gh->arfcn = htons(GSMTAP_ARFCN_F_UPLINK);
        if ((format == 0) || (format == 2)) gh->type = GSMTAP_TYPE_UM_BURST;
        else gh->type = GSMTAP_TYPE_UM;
        gh->sub_type = GSMTAP_CHANNEL_UNKNOWN;
        switch (format){
            case 0: gh->sub_type = GSMTAP_BURST_NORMAL; break;
            case 1: gh->sub_type = GSMTAP_CHANNEL_RACH; break;
            case 2: gh->sub_type = GSMTAP_BURST_ACCESS; break;
            case 3: gh->sub_type = GSMTAP_CHANNEL_BCCH; break;
            case 4: gh->sub_type = GSMTAP_CHANNEL_PCH; break;
            case 5: gh->sub_type = GSMTAP_CHANNEL_SDCCH; /*CBCH*/ break;
            case 7: gh->sub_type = GSMTAP_CHANNEL_SDCCH; break;
            case 8: gh->sub_type = GSMTAP_CHANNEL_ACCH | GSMTAP_CHANNEL_SDCCH; break;
            case 9: gh->sub_type = GSMTAP_CHANNEL_TCH_F; break;
            case 10: gh->sub_type = GSMTAP_CHANNEL_TCH_H; break;
            case 11: gh->sub_type = GSMTAP_CHANNEL_CCCH; break;
            case 12: gh->sub_type = GSMTAP_CHANNEL_AGCH; break;
            default: gh->sub_type = GSMTAP_CHANNEL_UNKNOWN;
        }
        memcpy(buf+sizeof(*gh), AppMsg+5, length);
        if (write(gsmtap_fd, buf, buf_len) < 0) perror("write gsmtap");
    }
}

void req(unsigned char *msg, int length)
{
    int sent = 0;
    do
    {
        int ret = write(serial_fd, msg, length - sent);
        if (ret < 0)
        {
            if (errno == EAGAIN)
            {
                usleep(1000);
                continue;
            }
            break;
        }
        sent += ret;
        msg += ret;
    } while (sent < length);
    if (sent != length) perror("Something went wrong while sending");
    printf("Sent %dB frame!\n", length);
}

int main(int argc, char **argv)
{
    struct stats { int packets, totalErrors, checksumErrors; } s;
    struct frame f;
    unsigned char actual, buf[3], fcs;
    unsigned short i;
    if (serial_init(argv[1]) < 0) return -1;
    gsmtap_open(argv[2]);
    printf("Press Ctrl+C to interrupt...\n");
    signal(SIGINT, interrupt);
    
    unsigned char myreq[] = {2, 0, 0, 6, 0, 31, 0, 0, 0, 63, 38, 3}; //LayerMessage-enable request
    req(myreq, sizeof(myreq));
    
    while (run)
    {
        if ((serial_read(buf, 1) == 1) && (buf[0] == 0x02))
        {
            s.packets++;
            serial_read(buf, 3);
            f.AppID = buf[0];
            f.AppMsgLength = ((buf[1] & 0x1F) << 8) | buf[2];
            fcs = f.AppID ^ buf[1] ^ buf[2];
            if (f.AppMsgLength > 255) //FIXME: not yet implemented, actually we don't need it at all
            {
                s.packets--;
                continue;
            }
            if (f.AppID != 0x00) continue; // we are just interested in parsing AppID == 0x00 (OTR) packets
            serial_read(f.AppMsg, f.AppMsgLength);
            for (i = 0; i < f.AppMsgLength; i++) fcs ^= f.AppMsg[i];
            actual = serial_read(buf, 2);
            if ((actual != 2) || (buf[1] != 0x03))
            {
                printf("(ERROR: ETX expected, got %d instead)\n", buf[1]);
                s.totalErrors++;
                continue;
            }
            printf("(OK %dB)", f.AppMsgLength);
            f.FCS = buf[0];
            if (fcs != f.FCS)
            {
                printf(" ~ WRONG CHECKSUM!!\n");
                s.totalErrors++; s.checksumErrors++;
                continue;
            }
            printf(" ~ AppMsg: [");
            for (i = 0; i < f.AppMsgLength; i++) printf("%d ", f.AppMsg[i]);
            printf("\b]\n");
            testgsmtap(f.AppMsg, f.AppMsgLength);
        }
    }
    myreq[9] = 0; myreq[10] = 25; //LayerMessage-disable request
    req(myreq, sizeof(myreq));
    printf("[*] Checksum errors: %d/%d | Total errors: %d/%d\n", s.checksumErrors, s.packets, s.totalErrors, s.packets);
    return (s.totalErrors == 0) ? 0 : 1;
}

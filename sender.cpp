#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <signal.h>
#include <string.h>
#include <sys/time.h>
#include <iostream>
#include <vector>
#include <cmath>
using namespace ::std;

#define MSS 1 
#define SLOW_START 0
#define FAST_RECOVERY 1
#define CONGESTION_AVOIDENCE 2

struct sockaddr_in si_other;
socklen_t slen;
int s;

typedef struct Segment
{
    int seq_num; 
    int len;
    char data[MSS];
} Segment;

void diep(string s)
{
    perror(s);
    exit(1);
}

int send_packet(int s, int last_sent, int cwnd_base, double cwnd, FILE *fp, unsigned long long int bytesToTransfer, int packet_num, vector<timeval> &time_list)
{
    int seq = cwnd_base;
    Segment buf[int(cwnd)];

    int offset = 0;
    offset = MSS * (cwnd_base) * sizeof(char);
    fseek(fp, offset, 0);

    for (int i = 0; i < int(cwnd); i++)
    {
        buf[i].seq_num = 0;
        buf[i].len = 0;
        memset(buf[i].data, 0, MSS);
    }

    while (!feof(fp) && seq < int(cwnd) + cwnd_base)
    {
        if (seq >= packet_num - 1) 
        {
            buf[seq - cwnd_base].seq_num = seq;
            fread(buf[seq - cwnd_base].data, sizeof(char), bytesToTransfer - (packet_num - 1) * MSS, fp);
            buf[seq - cwnd_base].len = bytesToTransfer - (packet_num - 1) * MSS;
        }
        else 
        {
            buf[seq - cwnd_base].seq_num = seq;
            fread(buf[seq - cwnd_base].data, sizeof(char), MSS, fp); 
            buf[seq - cwnd_base].len = sizeof(buf[seq - cwnd_base].data);
        }
        seq+=1;
    }

    for (int i = last_sent - cwnd_base; i < int(cwnd); i++)
    {
        Segment *seg_ptr = &(buf[i]);
        sendto(s, (char *)seg_ptr, sizeof(buf[i]), 0, (struct sockaddr *)&si_other, slen);
        struct timeval cur_time;
        gettimeofday(&cur_time, NULL);
        time_list.push_back(cur_time);
        last_sent+=1;
    }
    return last_sent;
}

void reliablyTransfer(char *hostname, unsigned short int hostUDPport, char *filename, unsigned long long int bytesToTransfer)
{
    double cwnd = 1;
    int cwnd_base = 0;
    int SST = 5;
    int dup_ack_count = 0;
    int last_ack,cur_ack = -1;
    int last_sent = 0;
    int state = SLOW_START;

    struct timeval cur_time;
    struct timeval time_out;
    time_out.tv_sec = 0;
    time_out.tv_usec = 20000;
    
    vector<timeval> time_list;
    FILE *fp;
    fp = fopen(filename, "rb");
    if (fp == NULL)
    {
        printf("Could not open file to send.");
        exit(1);
    }

    
    slen = sizeof(si_other);

    if ((s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1)
        diep("socket");

    memset((char *)&si_other, 0, sizeof(si_other));
    si_other.sin_family = AF_INET;
    si_other.sin_port = htons(hostUDPport);
    if (inet_aton(hostname, &si_other.sin_addr) == 0)
    {
        fprintf(stderr, "inet_aton() failed\n");
        exit(1);
    }

    int packet_num = ceil(double(bytesToTransfer) / double(MSS));
    last_sent = send_packet(s, 0, cwnd_base, cwnd, fp, bytesToTransfer, packet_num, time_list);

    while (1)
    {

        last_ack = cur_ack;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &time_out, sizeof(time_out));
        int numbytes = recvfrom(s, &cur_ack, sizeof(int), 0, (struct sockaddr *)&si_other, &slen);
        gettimeofday(&cur_time, NULL);

        int delta_time = 1000000 * (cur_time.tv_sec - time_list[last_ack + 1].tv_sec) + (cur_time.tv_usec - time_list[last_ack + 1].tv_usec);
        if (numbytes == -1 || delta_time > 30000)
        {
            state = SLOW_START;
            SST = int(cwnd / 2);
            cwnd = 1;
            Segment buf;
            Segment *seg_ptr = &(buf);
            buf.seq_num = cwnd_base;
            buf.len = MSS;
            fseek(fp, (last_ack + 1) * MSS * sizeof(char), 0);
            fread(buf.data, sizeof(char), MSS, fp);
            
            sendto(s, (char *)seg_ptr, sizeof(buf), 0, (struct sockaddr *)&si_other, slen);
            gettimeofday(&cur_time, NULL);
            time_list[last_ack + 1] = cur_time;
        }

        if (cur_ack + 1 == packet_num)
        {
            break;
        }
        if (cur_ack == last_ack)
        {
            dup_ack_count+=1;
            cwnd+=1;

            if (dup_ack_count == 3)
            {
                dup_ack_count = 0;
                state = FAST_RECOVERY;
                SST = cwnd / 2;
                cwnd = SST + 3;

                Segment buf;
                fseek(fp, (last_ack + 1) * MSS * sizeof(char), 0);
                fread(buf.data, sizeof(char), MSS, fp);
                buf.seq_num = cwnd_base;
                buf.len = MSS;
                Segment *seg_ptr = &(buf);
                sendto(s, (char *)seg_ptr, sizeof(buf), 0, (struct sockaddr *)&si_other, slen);
                gettimeofday(&cur_time, NULL);
                time_list[last_ack + 1] = cur_time;

            }
        }
        else 
        {
            cwnd_base = cur_ack + 1;
            dup_ack_count = 0;
            if (state == SLOW_START)
            {
                if (cwnd > SST)
                {
                    state = CONGESTION_AVOIDENCE;
                    cwnd+=1.0/int(cwnd);
                }
                else
                {
                    cwnd+=1;
                }
            }
            else if (state == CONGESTION_AVOIDENCE)
            {
                cwnd+=1.0/int(cwnd);
            }
            else if (state == FAST_RECOVERY)
            {
                cwnd+=1;
            }
        };
       
        if (cwnd_base+int(cwnd)>packet_num)
        {
            
            if (cwnd_base == packet_num)
            {
                break;
            }
            else if (cwnd)
            {
                last_sent = send_packet(s, last_sent, cwnd_base, packet_num - cwnd_base, fp, bytesToTransfer, packet_num, time_list);
            }
                
        }
        else
        {
            last_sent = send_packet(s, last_sent, cwnd_base, cwnd, fp, bytesToTransfer, packet_num, time_list);
        }
    }

    Segment close_connect;
    Segment *end_ptr = &close_connect;
    close_connect.seq_num = -3;

    while (1)
    {
        sendto(s, (char *)end_ptr, sizeof(close_connect), 0, (struct sockaddr *)&si_other, slen);
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &time_out, sizeof(time_out));
        int numbytes = recvfrom(s, &cur_ack, sizeof(int), 0, (struct sockaddr *)&si_other, &slen);
        if (cur_ack == -4)
        {
            break;
        }
    }

    cur_ack = -4;
    sendto(s, &cur_ack, sizeof(cur_ack), 0, (struct sockaddr *)&si_other, slen);
    fclose(fp);
    printf("Closing the socket\n");
    close(s);
    return;
}

int main(int argc, char **argv)
{

    unsigned short int udpPort;
    unsigned long long int numBytes;

    if (argc != 5)
    {
        fprintf(stderr, "usage: %s receiver_hostname receiver_port filename_to_xfer bytes_to_xfer\n\n", argv[0]);
        exit(1);
    }
    udpPort = (unsigned short int)atoi(argv[2]);
    numBytes = atoll(argv[4]);

    reliablyTransfer(argv[1], udpPort, argv[3], numBytes);

    return (EXIT_SUCCESS);
}

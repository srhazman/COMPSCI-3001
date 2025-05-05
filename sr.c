#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include "emulator.h"
#include "sr.h"

/* Selective Repeat protocol implementation */

#define RTT  16.0       /* round trip time.  MUST BE SET TO 16.0 when submitting assignment */
#define WINDOWSIZE 6    /* the maximum number of buffered unacked packet
                          MUST BE SET TO 6 when submitting assignment */
#define SEQSPACE 7      /* the min sequence space for GBN must be at least windowsize + 1 */
#define NOTINUSE (-1)   /* used to fill header fields that are not being used */

/* generic procedure to compute the checksum of a packet.  Used by both sender and receiver
   the simulator will overwrite part of your packet with 'z's.  It will not overwrite your
   original checksum.  This procedure must generate a different checksum to the original if
   the packet is corrupted.
*/
int ComputeChecksum(struct pkt packet)
{
  int checksum = 0;
  int i;

  checksum = packet.seqnum;
  checksum += packet.acknum;
  for ( i=0; i<20; i++ )
    checksum += (int)(packet.payload[i]);

  return checksum;
}

bool IsCorrupted(struct pkt packet)
{
  if (packet.checksum == ComputeChecksum(packet))
    return (false);
  else
    return (true);
}

/********* Selective Repeat Implementation ************/

/* Sender (A) variables */
static struct pkt snd_buffer[6];  /* sender window buffer (WINDOWSIZE=6) */
static bool snd_acked[6];         /* tracks ACK status for each packet */
static int snd_base = 0;          /* oldest unACKed packet */
static int snd_nextseqnum = 0;    /* next sequence number to use */
static int snd_windowcount = 0;   /* packets currently in window */

/* Receiver (B) variables */
static struct pkt rcv_buffer[6];  /* receiver window buffer */
static bool rcv_received[6];      /* tracks received packets */
static int rcv_base = 0;          /* expected sequence number */

void A_init(void)
{
    int i;
    snd_base = 0;
    snd_nextseqnum = 0;
    snd_windowcount = 0;
    for (i = 0; i < 6; i++) {
        snd_acked[i] = false;
    }
}

void A_output(struct msg message)
{
    int i;
    int index;  /* Declared at start of block for C90 compliance */
    
    if (snd_windowcount < 6) {
        struct pkt sendpkt;
        
        /* Create packet */
        sendpkt.seqnum = snd_nextseqnum;
        sendpkt.acknum = -1;  /* NOTINUSE */
        for (i = 0; i < 20; i++) {
            sendpkt.payload[i] = message.data[i];
        }
        sendpkt.checksum = ComputeChecksum(sendpkt);
        
        /* Store in buffer */
        index = snd_nextseqnum % 6;
        snd_buffer[index] = sendpkt;
        snd_acked[index] = false;
        
        /* Send packet */
        if (TRACE > 0) printf("Sending packet %d\n", sendpkt.seqnum);
        tolayer3(0, sendpkt);  /* 0 = entity A */
        
        /* Start timer if first packet in window */
        if (snd_windowcount == 0) starttimer(0, 16.0);  /* RTT=16.0 */
        
        snd_nextseqnum = (snd_nextseqnum + 1) % 7;  /* SEQSPACE=7 */
        snd_windowcount++;
    } else {
        if (TRACE > 0) printf("Window full, message dropped\n");
        window_full++;
    }
}

void A_input(struct pkt packet)
{
    int index;  /* Declared at start of block */
    
    if (!IsCorrupted(packet)) {
        if (TRACE > 0) printf("Received ACK %d\n", packet.acknum);
        total_ACKs_received++;
        
        /* Check if ACK is within window */
        if ((snd_base <= packet.acknum && packet.acknum < snd_base + 6) ||
            (snd_base > packet.acknum && packet.acknum < (snd_base + 6) % 7)) {
            
            index = packet.acknum % 6;
            if (!snd_acked[index]) {
                snd_acked[index] = true;
                new_ACKs++;
                
                /* Slide window forward */
                while (snd_acked[snd_base % 6] && snd_windowcount > 0) {
                    snd_acked[snd_base % 6] = false;
                    snd_base = (snd_base + 1) % 7;
                    snd_windowcount--;
                }
                
                /* Manage timer */
                if (snd_windowcount > 0) {
                    stoptimer(0);
                    starttimer(0, 16.0);
                } else {
                    stoptimer(0);
                }
            }
        }
    } else {
        if (TRACE > 0) printf("Corrupted ACK received\n");
    }
}

void A_timerinterrupt(void)
{
    int i;
    int seqnum;
    int index;
    
    if (TRACE > 0) printf("Timeout occurred\n");
    
    /* Find oldest unACKed packet */
    for (i = 0; i < 6 && i < snd_windowcount; i++) {
        seqnum = (snd_base + i) % 7;
        index = seqnum % 6;
        if (!snd_acked[index]) {
            if (TRACE > 0) printf("Resending packet %d\n", seqnum);
            tolayer3(0, snd_buffer[index]);
            packets_resent++;
            break; /* SR: Only resend one packet */
        }
    }
    starttimer(0, 16.0);
}

void B_init(void)
{
    int i;
    rcv_base = 0;
    for (i = 0; i < 6; i++) {
        rcv_received[i] = false;
    }
}

void B_input(struct pkt packet)
{
    struct pkt ackpkt;
    int i;
    int index;
    
    /* Prepare ACK packet */
    ackpkt.seqnum = 0; /* Not used */
    ackpkt.acknum = -1;  /* NOTINUSE */
    for (i = 0; i < 20; i++) ackpkt.payload[i] = '0';
    
    if (!IsCorrupted(packet)) {
        /* Check if packet is within window */
        if ((rcv_base <= packet.seqnum && packet.seqnum < rcv_base + 6) ||
            (rcv_base > packet.seqnum && packet.seqnum < (rcv_base + 6) % 7)) {
            
            index = packet.seqnum % 6;
            if (!rcv_received[index]) {
                rcv_buffer[index] = packet;
                rcv_received[index] = true;
                packets_received++;
                if (TRACE > 0) printf("Received packet %d\n", packet.seqnum);
            }
            ackpkt.acknum = packet.seqnum;
            
            /* Deliver in-order packets */
            while (rcv_received[rcv_base % 6]) {
                tolayer5(1, rcv_buffer[rcv_base % 6].payload);  /* 1 = entity B */
                rcv_received[rcv_base % 6] = false;
                rcv_base = (rcv_base + 1) % 7;
            }
        } else {
            if (TRACE > 0) printf("Packet %d outside window\n", packet.seqnum);
            ackpkt.acknum = (rcv_base + 6) % 7;  /* SEQSPACE-1 = 6 */
        }
    } else {
        if (TRACE > 0) printf("Corrupted packet received\n");
        ackpkt.acknum = (rcv_base + 6) % 7;
    }
    
    ackpkt.checksum = ComputeChecksum(ackpkt);
    tolayer3(1, ackpkt);  /* 1 = entity B */
}

/* Unused functions */
void B_output(struct msg message) {}
void B_timerinterrupt(void) {}
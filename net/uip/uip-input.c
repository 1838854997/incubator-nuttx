/****************************************************************************
 * net/uip/uip-input.c
 * The uIP TCP/IP stack code.
 *
 *   Copyright (C) 2007 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <spudmonkey@racsa.co.cr>
 *
 * Adapted for NuttX from logic in uIP which also has a BSD-like license:
 *
 * uIP is an implementation of the TCP/IP protocol stack intended for
 * small 8-bit and 16-bit microcontrollers.
 *
 * uIP provides the necessary protocols for Internet communication,
 * with a very small code footprint and RAM requirements - the uIP
 * code size is on the order of a few kilobytes and RAM usage is on
 * the order of a few hundred bytes.
 *
 *   Original author Adam Dunkels <adam@dunkels.com>
 *   Copyright () 2001-2003, Adam Dunkels.
 *   All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
 * GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * uIP is a small implementation of the IP, UDP and TCP protocols (as
 * well as some basic ICMP stuff). The implementation couples the IP,
 * UDP, TCP and the application layers very tightly. To keep the size
 * of the compiled code down, this code frequently uses the goto
 * statement. While it would be possible to break the uip_input()
 * function into many smaller functions, this would increase the code
 * size because of the overhead of parameter passing and the fact that
 * the optimier would not be as efficient.
 *
 * The principle is that we have a small buffer, called the d_buf,
 * in which the device driver puts an incoming packet. The TCP/IP
 * stack parses the headers in the packet, and calls the
 * application. If the remote host has sent data to the application,
 * this data is present in the d_buf and the application read the
 * data from there. It is up to the application to put this data into
 * a byte stream if needed. The application will not be fed with data
 * that is out of sequence.
 *
 * If the application whishes to send data to the peer, it should put
 * its data into the d_buf. The d_appdata pointer points to the
 * first available byte. The TCP/IP stack will calculate the
 * checksums, and fill in the necessary header fields and finally send
 * the packet back to the peer.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#ifdef CONFIG_NET

#include <sys/types.h>
#include <sys/ioctl.h>

#include <debug.h>
#include <string.h>

#include <net/uip/uipopt.h>
#include <net/uip/uip.h>
#include <net/uip/uip-arch.h>

#ifdef CONFIG_NET_IPv6
# include "uip-neighbor.h"
#endif /* CONFIG_NET_IPv6 */

#include "uip-internal.h"

/****************************************************************************
 * Definitions
 ****************************************************************************/

/* Macros. */

#define BUF     ((struct uip_ip_hdr *)&dev->d_buf[UIP_LLH_LEN])
#define FBUF    ((struct uip_ip_hdr *)&uip_reassbuf[0])

/* IP fragment re-assembly */

#define IP_MF                   0x20
#define UIP_REASS_BUFSIZE       (CONFIG_NET_BUFSIZE - UIP_LLH_LEN)
#define UIP_REASS_FLAG_LASTFRAG 0x01

/****************************************************************************
 * Public Variables
 ****************************************************************************/

/****************************************************************************
 * Private Variables
 ****************************************************************************/

#if UIP_REASSEMBLY && !defined(CONFIG_NET_IPv6)
static uint8 uip_reassbuf[UIP_REASS_BUFSIZE];
static uint8 uip_reassbitmap[UIP_REASS_BUFSIZE / (8 * 8)];
static const uint8 bitmap_bits[8] = {0xff, 0x7f, 0x3f, 0x1f, 0x0f, 0x07, 0x03, 0x01};
static uint16 uip_reasslen;
static uint8 uip_reassflags;
#endif /* UIP_REASSEMBLY */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Function: uip_reass
 *
 * Description:
 *   IP fragment reassembly: not well-tested.
 *
 * Assumptions:
 *
 ****************************************************************************/

#if UIP_REASSEMBLY && !defined(CONFIG_NET_IPv6)
static uint8 uip_reass(void)
{
  uint16 offset, len;
  uint16 i;

  /* If uip_reasstmr is zero, no packet is present in the buffer, so we
   * write the IP header of the fragment into the reassembly
   * buffer. The timer is updated with the maximum age.
   */

  if (!uip_reasstmr)
    {
      memcpy(uip_reassbuf, &BUF->vhl, UIP_IPH_LEN);
      uip_reasstmr   = UIP_REASS_MAXAGE;
      uip_reassflags = 0;

      /* Clear the bitmap. */
      memset(uip_reassbitmap, 0, sizeof(uip_reassbitmap));
    }

  /* Check if the incoming fragment matches the one currently present
   * in the reasembly buffer. If so, we proceed with copying the
   * fragment into the buffer.
   */

  if (uiphdr_addr_cmp(BUF->srcipaddr, FBUF->srcipaddr) && 
      uiphdr_addr_cmp(BUF->destipaddr == FBUF->destipaddr) &&
      BUF->g_ipid[0] == FBUF->g_ipid[0] && BUF->g_ipid[1] == FBUF->g_ipid[1])
    {
      len = (BUF->len[0] << 8) + BUF->len[1] - (BUF->vhl & 0x0f) * 4;
      offset = (((BUF->ipoffset[0] & 0x3f) << 8) + BUF->ipoffset[1]) * 8;

      /* If the offset or the offset + fragment length overflows the
       * reassembly buffer, we discard the entire packet.
       */

      if (offset > UIP_REASS_BUFSIZE || offset + len > UIP_REASS_BUFSIZE)
        {
          uip_reasstmr = 0;
          goto nullreturn;
        }

      /* Copy the fragment into the reassembly buffer, at the right offset. */

      memcpy(&uip_reassbuf[UIP_IPH_LEN + offset], (char *)BUF + (int)((BUF->vhl & 0x0f) * 4), len);

    /* Update the bitmap. */

    if (offset / (8 * 8) == (offset + len) / (8 * 8))
      {
        /* If the two endpoints are in the same byte, we only update that byte. */

        uip_reassbitmap[offset / (8 * 8)] |=
          bitmap_bits[(offset / 8 ) & 7] & ~bitmap_bits[((offset + len) / 8 ) & 7];

      }
    else
      {
        /* If the two endpoints are in different bytes, we update the bytes
         * in the endpoints and fill the stuff inbetween with 0xff.
         */

        uip_reassbitmap[offset / (8 * 8)] |= bitmap_bits[(offset / 8 ) & 7];
        for (i = 1 + offset / (8 * 8); i < (offset + len) / (8 * 8); ++i)
          {
            uip_reassbitmap[i] = 0xff;
          }
        uip_reassbitmap[(offset + len) / (8 * 8)] |= ~bitmap_bits[((offset + len) / 8 ) & 7];
      }

    /* If this fragment has the More Fragments flag set to zero, we know that
     * this is the last fragment, so we can calculate the size of the entire
     * packet. We also set the IP_REASS_FLAG_LASTFRAG flag to indicate that
     * we have received the final fragment.
     */

    if ((BUF->ipoffset[0] & IP_MF) == 0)
      {
        uip_reassflags |= UIP_REASS_FLAG_LASTFRAG;
        uip_reasslen = offset + len;
      }

    /* Finally, we check if we have a full packet in the buffer. We do this
     * by checking if we have the last fragment and if all bits in the bitmap
     * are set.
     */

    if (uip_reassflags & UIP_REASS_FLAG_LASTFRAG)
      {
        /* Check all bytes up to and including all but the last byte in
         * the bitmap.
         */

        for (i = 0; i < uip_reasslen / (8 * 8) - 1; ++i)
          {
            if (uip_reassbitmap[i] != 0xff)
              {
                goto nullreturn;
              }
          }

        /* Check the last byte in the bitmap. It should contain just the
         * right amount of bits.
         */

        if (uip_reassbitmap[uip_reasslen / (8 * 8)] != (uint8)~bitmap_bits[uip_reasslen / 8 & 7])
          {
            goto nullreturn;
          }

        /* If we have come this far, we have a full packet in the buffer,
         * so we allocate a pbuf and copy the packet into it. We also reset
         * the timer.
         */

        uip_reasstmr = 0;
        memcpy(BUF, FBUF, uip_reasslen);

        /* Pretend to be a "normal" (i.e., not fragmented) IP packet from
         * now on.
         */

        BUF->ipoffset[0] = BUF->ipoffset[1] = 0;
        BUF->len[0] = uip_reasslen >> 8;
        BUF->len[1] = uip_reasslen & 0xff;
        BUF->ipchksum = 0;
        BUF->ipchksum = ~(uip_ipchksum(dev));

        return uip_reasslen;
      }
  }

nullreturn:
  return 0;
}
#endif /* UIP_REASSEMBLY */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Function: uip_input
 *
 * Description:
 *
 * Assumptions:
 *
 ****************************************************************************/

void uip_input(struct uip_driver_s *dev)
{
  /* This is where the input processing starts. */

#ifdef CONFIG_NET_STATISTICS
  uip_stat.ip.recv++;
#endif

  /* Start of IP input header processing code. */

#ifdef CONFIG_NET_IPv6
  /* Check validity of the IP header. */

  if ((BUF->vtc & 0xf0) != 0x60) 
    {
      /* IP version and header length. */

#ifdef CONFIG_NET_STATISTICS
      uip_stat.ip.drop++;
      uip_stat.ip.vhlerr++;
#endif
      ndbg("Invalid IPv6 version: %d\n", BUF->vtc >> 4);
      goto drop;
    }
#else /* CONFIG_NET_IPv6 */
  /* Check validity of the IP header. */

  if (BUF->vhl != 0x45)
    {
      /* IP version and header length. */

#ifdef CONFIG_NET_STATISTICS
      uip_stat.ip.drop++;
      uip_stat.ip.vhlerr++;
#endif
      ndbg("Invalid IP version or header length: %02x\n", BUF->vhl);
      goto drop;
    }
#endif /* CONFIG_NET_IPv6 */

  /* Check the size of the packet. If the size reported to us in d_len is
   * smaller the size reported in the IP header, we assume that the packet
   * has been corrupted in transit. If the size of d_len is larger than the
   * size reported in the IP packet header, the packet has been padded and
   * we set d_len to the correct value.
   */

  if ((BUF->len[0] << 8) + BUF->len[1] <= dev->d_len)
    {
      dev->d_len = (BUF->len[0] << 8) + BUF->len[1];
#ifdef CONFIG_NET_IPv6
      /* The length reported in the IPv6 header is the length of the
       * payload that follows the header. However, uIP uses the d_len
       * variable for holding the size of the entire packet, including the
       * IP header. For IPv4 this is not a problem as the length field in
       * the IPv4 header contains the length of the entire packet. But
       * for IPv6 we need to add the size of the IPv6 header (40 bytes).
       */

      dev->d_len += 40;
#endif /* CONFIG_NET_IPv6 */
    }
  else
    {
      ndbg("IP packet shorter than length in IP header\n");
      goto drop;
    }

#ifndef CONFIG_NET_IPv6
  /* Check the fragment flag. */

  if ((BUF->ipoffset[0] & 0x3f) != 0 || BUF->ipoffset[1] != 0)
    {
#if UIP_REASSEMBLY
      dev->d_len = uip_reass();
      if (dev->d_len == 0)
        {
          goto drop;
        }
#else /* UIP_REASSEMBLY */
#ifdef CONFIG_NET_STATISTICS
      uip_stat.ip.drop++;
      uip_stat.ip.fragerr++;
#endif
      ndbg("IP fragment dropped\n");
      goto drop;
#endif /* UIP_REASSEMBLY */
    }
#endif /* CONFIG_NET_IPv6 */

   /* If IP broadcast support is configured, we check for a broadcast
    * UDP packet, which may be destined to us (even if there is no IP
    * address yet assigned to the device as is the case when we are
    * negotiating over DHCP for an address).
    */

#if defined(CONFIG_NET_BROADCAST) && defined(CONFIG_NET_UDP)
  if (BUF->proto == UIP_PROTO_UDP &&
#ifndef CONFIG_NET_IPv6
      uip_ipaddr_cmp(uip_ip4addr_conv(BUF->destipaddr), g_alloneaddr))
#else
      uip_ipaddr_cmp(BUF->destipaddr, g_alloneaddr))
#endif
    {
      uip_udpinput(dev);
      return;
    }

  /* In most other cases, the device must be assigned a non-zero IP
   * address.  Another exception is when CONFIG_NET_PINGADDRCONF is
   * enabled...
   */

  else
#endif
#ifdef CONFIG_NET_ICMP
  if (uip_ipaddr_cmp(dev->d_ipaddr, g_allzeroaddr))
    {
      /* If we are configured to use ping IP address configuration and
       * hasn't been assigned an IP address yet, we accept all ICMP
       * packets.
       */

#if defined(CONFIG_NET_PINGADDRCONF) && !defined(CONFIG_NET_IPv6)
      if (BUF->proto == UIP_PROTO_ICMP)
        {
          ndbg("Possible ping config packet received\n");
          uip_icmpinput(dev);
          goto done;
        }
      else
#endif
        {
          ndbg("No IP address assigned\n");
          goto drop;
        }
    }

  /* Check if the pack is destined for out IP address */
  else
#endif
    {
      /* Check if the packet is destined for our IP address. */
#ifndef CONFIG_NET_IPv6
      if (!uip_ipaddr_cmp(uip_ip4addr_conv(BUF->destipaddr), dev->d_ipaddr))
        {
#ifdef CONFIG_NET_STATISTICS
          uip_stat.ip.drop++;
#endif
          goto drop;
        }
#else /* CONFIG_NET_IPv6 */
      /* For IPv6, packet reception is a little trickier as we need to
       * make sure that we listen to certain multicast addresses (all
       * hosts multicast address, and the solicited-node multicast
       * address) as well. However, we will cheat here and accept all
       * multicast packets that are sent to the ff02::/16 addresses.
       */

      if (!uip_ipaddr_cmp(BUF->destipaddr, dev->d_ipaddr) &&
           BUF->destipaddr & HTONL(0xffff0000) != HTONL(0xff020000))
        {
#ifdef CONFIG_NET_STATISTICS
          uip_stat.ip.drop++;
#endif
          goto drop;
        }
#endif /* CONFIG_NET_IPv6 */
    }

#ifndef CONFIG_NET_IPv6
  if (uip_ipchksum(dev) != 0xffff)
    {
      /* Compute and check the IP header checksum. */

#ifdef CONFIG_NET_STATISTICS
      uip_stat.ip.drop++;
      uip_stat.ip.chkerr++;
#endif
      ndbg("Bad IP checksum\n");
      goto drop;
    }
#endif /* CONFIG_NET_IPv6 */

  /* Everything looks good so far.  Now process the incoming packet
   * according to the protocol.
   */

  switch (BUF->proto)
    {
#ifdef CONFIG_NET_TCP
      case UIP_PROTO_TCP:   /* TCP input */
        uip_tcpinput(dev);
        break;
#endif

#ifdef CONFIG_NET_UDP
      case UIP_PROTO_UDP:   /* UDP input */
        uip_udpinput(dev);
        break;
#endif

  /* Check for ICMP input */

#ifdef CONFIG_NET_ICMP
#ifndef CONFIG_NET_IPv6
      case UIP_PROTO_ICMP:  /* ICMP input */
#else
      case UIP_PROTO_ICMP6: /* ICMP6 input */
#endif
        uip_icmpinput(dev);
        break;
#endif

      default:              /* Unrecognized/unsupported protocol */
#ifdef CONFIG_NET_STATISTICS
        uip_stat.ip.drop++;
        uip_stat.ip.protoerr++;
#endif

        ndbg("Unrecognized IP protocol\n");
        goto drop;
    }

  /* Return and let the caller do any actual transmission. */

  return;

drop:
  dev->d_len = 0;
}
#endif /* CONFIG_NET */


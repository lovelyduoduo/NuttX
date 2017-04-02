/****************************************************************************
 * net/sixlowpan/sixlowpan_tcpsend.c
 *
 *   Copyright (C) 2017 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <assert.h>
#include <errno.h>
#include <debug.h>

#include "nuttx/net/netdev.h"

#include "netdev/netdev.h"
#include "socket/socket.h"
#include "tcp/tcp.h"
#include "sixlowpan/sixlowpan_internal.h"

#if defined(CONFIG_NET_6LOWPAN) && defined(CONFIG_NET_TCP)

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Function: psock_6lowpan_tcp_send
 *
 * Description:
 *   psock_6lowpan_tcp_send() call may be used only when the TCP socket is in a
 *   connected state (so that the intended recipient is known).
 *
 * Parameters:
 *   psock - An instance of the internal socket structure.
 *   buf   - Data to send
 *   len   - Length of data to send
 *
 * Returned Value:
 *   On success, returns the number of characters sent.  On  error,
 *   -1 is returned, and errno is set appropriately.  Returned error numbers
 *   must be consistent with definition of errors reported by send() or
 *   sendto().
 *
 * Assumptions:
 *   Called with the network locked.
 *
 ****************************************************************************/

ssize_t psock_6lowpan_tcp_send(FAR struct socket *psock, FAR const void *buf,
                               size_t len)
{
  FAR struct tcp_conn_s *conn;
  FAR struct net_driver_s *dev;
  struct ipv6tcp_hdr_s ipv6tcp;
  struct rimeaddr_s destmac;
  uint16_t timeout;
  int ret;

  DEBUGASSERT(psock != NULL && psock->s_crefs > 0);
  DEBUGASSERT(psock->s_type == SOCK_STREAM);

  /* Make sure that this is a valid socket */

  if (psock != NULL || psock->s_crefs <= 0)
    {
      nerr("ERROR: Invalid socket\n");
      return (ssize_t)-EBADF;
    }

  /* Make sure that this is a connected TCP socket */

  if (psock->s_type != SOCK_STREAM || !_SS_ISCONNECTED(psock->s_flags))
    {
      nerr("ERROR: Not connected\n");
      return (ssize_t)-ENOTCONN;
    }

  /* Get the underlying TCP connection structure */

  conn = (FAR struct tcp_conn_s *)psock->s_conn;
  DEBUGASSERT(conn != NULL);

#ifdef CONFIG_NET_IPv4
  /* Ignore if not IPv6 domain */

  if (conn->domain != PF_INET6)
    {
      nwarn("WARNING: Not IPv6\n");
      return (ssize_t)-EPROTOTYPE;
    }
#endif

  /* Route outgoing message to the correct device */

#ifdef CONFIG_NETDEV_MULTINIC
  dev = netdev_findby_ipv6addr(conn->u.ipv6.laddr, conn->u.ipv6.raddr);
#ifdef CONFIG_NETDEV_MULTILINK
  if (dev == NULL || dev->d_lltype != NET_LL_IEEE802154)
#else
  if (dev == NULL)
#endif
    {
      nwarn("WARNING: Not routable or not IEEE802.15.4 MAC\n");
      return (ssize_t)-ENETUNREACH;
    }
#else
  dev = netdev_findby_ipv6addr(conn->u.ipv6.raddr);
#ifdef CONFIG_NETDEV_MULTILINK
  if (dev == NULL || dev->d_lltype != NET_LL_IEEE802154)
#else
  if (dev == NULL)
#endif
    {
      nwarn("WARNING: Not routable\n");
      return (ssize_t)-ENETUNREACH;
    }
#endif

#ifdef CONFIG_NET_ICMPv6_NEIGHBOR
  /* Make sure that the IP address mapping is in the Neighbor Table */

  ret = icmpv6_neighbor(conn->u.ipv6.raddr);
  if (ret < 0)
    {
      nerr("ERROR: Not reachable\n");
      return (ssize_t)-ENETUNREACH;
    }
#endif

  /* Initialize the IPv6/TCP headers */
#warning Missing logic

  /* Set the socket state to sending */

  psock->s_flags = _SS_SETSTATE(psock->s_flags, _SF_SEND);

  /* Get the Rime MAC address of the destination  This assumes an encoding
   * of the MAC address in the IPv6 address.
   */

  sixlowpan_rimefromip(conn->u.ipv6.raddr, &destmac);

  /* If routable, then call sixlowpan_send() to format and send the 6loWPAN
   * packet.
   */

#ifdef CONFIG_NET_SOCKOPTS
  timeout = psock->s_sndtimeo;
#else
  timeout = 0;
#endif

  ret = sixlowpan_send(dev, (FAR const struct ipv6_hdr_s *)&ipv6tcp,
                       buf, len, &destmac, timeout);
  if (ret < 0)
    {
      nerr("ERROR: sixlowpan_send() failed: %d\n", ret);
    }

  /* Set the socket state to idle */

  psock->s_flags = _SS_SETSTATE(psock->s_flags, _SF_IDLE);
  return ret;
}

/****************************************************************************
 * Function: sixlowpan_tcp_send
 *
 * Description:
 *   TCP output comes through two different mechansims.  Either from:
 *
 *   1. TCP socket output.  For the case of TCP output to an
 *      IEEE802.15.4, the TCP output is caught in the socket
 *      send()/sendto() logic and and redirected to psock_6lowpan_tcp_send().
 *   2. TCP output from the TCP state machine.  That will occur
 *      during TCP packet processing by the TCP state meachine.  It
 *      is detected there when ipv6_tcp_input() returns with d_len > 0. This
 *      will be redirected here.
 *
 * Parameters:
 *   dev - An instance of nework device state structure
 *
 * Returned Value:
 *   None
 *
 * Assumptions:
 *   Called with the network locked.
 *
 ****************************************************************************/

void sixlowpan_tcp_send(FAR struct net_driver_s *dev)
{
  DEBUGASSERT(dev != NULL && dev->d_len > 0);

  /* Double check */

  if (dev != NULL && dev->d_len > 0)
    {
      FAR struct ipv6_hdr_s *ipv6hdr;

      /* The IPv6 header followed by a TCP headers should lie at the
       * beginning of d_buf since there is no link layer protocol header
       * and the TCP state machine should only response with TCP packets.
       */

      ipv6hdr = (FAR struct ipv6_hdr_s *)(dev->d_buf);

      /* The TCP data payload should follow the IPv6 header plus the
       * protocol header.
       */

      if (ipv6hdr->proto != IP_PROTO_TCP)
        {
          nwarn("WARNING: Expected TCP protoype: %u\n", ipv6hdr->proto);
        }
      else
        {
          size_t hdrlen;

          hdrlen = IPv6_HDRLEN + TCP_HDRLEN;
          if (hdrlen < dev->d_len)
            {
              nwarn("WARNING: Packet to small:  Have %u need >%u\n",
                  dev->d_len, hdrlen);
            }
          else
            {
              struct rimeaddr_s destmac;

              /* Get the Rime MAC address of the destination.  This assumes
               * an encoding of the MAC address in the IPv6 address.
               */

              sixlowpan_rimefromip(ipv6hdr->destipaddr, &destmac);

              /* Convert the outgoing packet into a frame list. */

              (void)sixlowpan_queue_frames(
                      (FAR struct ieee802154_driver_s *)dev, ipv6hdr,
                      dev->d_buf + hdrlen, dev->d_len - hdrlen, &destmac);
            }
        }
    }

  dev->d_len = 0;
}

#endif /* CONFIG_NET_6LOWPAN && CONFIG_NET_TCP */
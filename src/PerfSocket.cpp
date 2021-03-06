/*--------------------------------------------------------------- 
 * Copyright (c) 1999,2000,2001,2002,2003                              
 * The Board of Trustees of the University of Illinois            
 * All Rights Reserved.                                           
 *--------------------------------------------------------------- 
 * Permission is hereby granted, free of charge, to any person    
 * obtaining a copy of this software (Iperf) and associated       
 * documentation files (the "Software"), to deal in the Software  
 * without restriction, including without limitation the          
 * rights to use, copy, modify, merge, publish, distribute,        
 * sublicense, and/or sell copies of the Software, and to permit     
 * persons to whom the Software is furnished to do
 * so, subject to the following conditions: 
 *
 *     
 * Redistributions of source code must retain the above 
 * copyright notice, this list of conditions and 
 * the following disclaimers. 
 *
 *     
 * Redistributions in binary form must reproduce the above 
 * copyright notice, this list of conditions and the following 
 * disclaimers in the documentation and/or other materials 
 * provided with the distribution. 
 * 
 *     
 * Neither the names of the University of Illinois, NCSA, 
 * nor the names of its contributors may be used to endorse 
 * or promote products derived from this Software without
 * specific prior written permission. 
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, 
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES 
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND 
 * NONINFRINGEMENT. IN NO EVENT SHALL THE CONTIBUTORS OR COPYRIGHT 
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, 
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, 
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE. 
 * ________________________________________________________________
 * National Laboratory for Applied Network Research 
 * National Center for Supercomputing Applications 
 * University of Illinois at Urbana-Champaign 
 * http://www.ncsa.uiuc.edu
 * ________________________________________________________________ 
 *
 * PerfSocket.cpp
 * by Mark Gates <mgates@nlanr.net>
 *    Ajay Tirumala <tirumala@ncsa.uiuc.edu>
 * -------------------------------------------------------------------
 * Has routines the Client and Server classes use in common for
 * performance testing the network.
 * Changes in version 1.2.0
 *     for extracting data from files
 * -------------------------------------------------------------------
 * headers
 * uses
 *   <stdlib.h>
 *   <stdio.h>
 *   <string.h>
 *
 *   <sys/types.h>
 *   <sys/socket.h>
 *   <unistd.h>
 *
 *   <arpa/inet.h>
 *   <netdb.h>
 *   <netinet/in.h>
 *   <sys/socket.h>
 * ------------------------------------------------------------------- */


#define HEADERS()

#include "headers.h"

#include "PerfSocket.hpp"
#include "SocketAddr.h"
#include "Locale.h"
#include "util.h"

#ifdef HAVE_IPV6_MULTICAST
#include <net/if.h>
#include <sys/ioctl.h>
#include <ifaddrs.h>
#include <linux/rtnetlink.h>
#endif

/* -------------------------------------------------------------------
 * Set socket options before the listen() or connect() calls.
 * These are optional performance tuning factors.
 * ------------------------------------------------------------------- */

void SetSocketOptions( thread_Settings *inSettings ) {
    // set the TCP window size (socket buffer sizes)
    // also the UDP buffer size
    // must occur before call to accept() for large window sizes
    setsock_tcp_windowsize( inSettings->mSock, inSettings->mTCPWin,
                            (inSettings->mThreadMode == kMode_Client ? 1 : 0) );

    if ( isCongestionControl( inSettings ) ) {
#ifdef TCP_CONGESTION
	Socklen_t len = strlen( inSettings->mCongestion ) + 1;
	int rc = setsockopt( inSettings->mSock, IPPROTO_TCP, TCP_CONGESTION,
			     inSettings->mCongestion, len);
	if (rc == SOCKET_ERROR ) {
		fprintf(stderr, "Attempt to set '%s' congestion control failed: %s\n",
			inSettings->mCongestion, strerror(errno));
		exit(1);
	}
#else
	fprintf( stderr, "The -Z option is not available on this operating system\n");
#endif
    }

    // check if we're sending multicast, and set TTL
    if ( isMulticast( inSettings ) && ( inSettings->mTTL > 0 ) ) {
	int val = inSettings->mTTL;
#ifdef HAVE_MULTICAST
	if ( !SockAddr_isIPv6( &inSettings->local ) ) {
	    int rc = setsockopt( inSettings->mSock, IPPROTO_IP, IP_MULTICAST_TTL,
		    (const void*) &val, (Socklen_t) sizeof(val));

	    WARN_errno( rc == SOCKET_ERROR, "multicast ttl" );
	}
#ifdef HAVE_IPV6_MULTICAST
	else {
	    int rc = setsockopt( inSettings->mSock, IPPROTO_IPV6, IPV6_MULTICAST_HOPS,
		    (const void*) &val, (Socklen_t) sizeof(val));
	    WARN_errno( rc == SOCKET_ERROR, "multicast ttl" );
	}
#endif
#endif
    }


#ifdef IP_TOS

    // set IP TOS (type-of-service) field
    if ( inSettings->mTOS > 0 ) {
        int  tos = inSettings->mTOS;
        Socklen_t len = sizeof(tos);
        int rc = setsockopt( inSettings->mSock, IPPROTO_IP, IP_TOS,
                             (char*) &tos, len );
        WARN_errno( rc == SOCKET_ERROR, "setsockopt IP_TOS" );
    }
#endif

    if ( !isUDP( inSettings ) ) {
        // set the TCP maximum segment size
        setsock_tcp_mss( inSettings->mSock, inSettings->mMSS );

#ifdef TCP_NODELAY

        // set TCP nodelay option
        if ( isNoDelay( inSettings ) ) {
            int nodelay = 1;
            Socklen_t len = sizeof(nodelay);
            int rc = setsockopt( inSettings->mSock, IPPROTO_TCP, TCP_NODELAY,
                                 (char*) &nodelay, len );
            WARN_errno( rc == SOCKET_ERROR, "setsockopt TCP_NODELAY" );
        }
#endif
    }
}
// end SetSocketOptions

static int cmp_bits(const uint8_t *a, const uint8_t *b, unsigned int bits)
{
	unsigned int i;

	for (i = 0; i < bits; i++) {
		uint8_t abit = !!(a[i / 8] & (1 << (i % 8)));
		uint8_t bbit = !!(b[i / 8] & (1 << (i % 8)));
		if (abit != bbit) {
			return 1;
		}
	}

	return 0;
}

int FindIPv6MulticastInterface( thread_Settings *inSettings ) {
	int ret = -1;
#ifdef HAVE_IPV6_MULTICAST
	struct sockaddr_nl me;
	struct sockaddr_nl them;

	int rtnl_socket;

	struct msghdr msg;
	struct iovec iov;
	struct {
		struct nlmsghdr nl;
		struct rtmsg    rt;
		uint8_t		buf[1024];
	} request;

	const size_t reply_buf_size = 64 * 1024;
	uint8_t *reply_buf = new uint8_t[reply_buf_size];

	struct nlmsghdr *nlp;
	struct rtmsg *rtp;
	struct rtattr *rtap;
	uint8_t *const rtattr_start = request.buf;
	uint8_t *rtattr_end = rtattr_start;
	unsigned int nl_len;
	unsigned int rt_len;
	int rc;
	const struct in6_addr *cmp_ip6;

	if (SockAddr_isIPv6( &inSettings->peer )) {
		const struct in6_addr *dst_ip6 = SockAddr_get_in6_addr(&inSettings->peer);
		cmp_ip6 = dst_ip6;
	} else if (SockAddr_isIPv6( &inSettings->local )) {
		const struct in6_addr *src_ip6 = SockAddr_get_in6_addr(&inSettings->local);
		cmp_ip6 = src_ip6;
	} else {
		return ret;
	}

	if ((rtnl_socket = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE)) < 0) {
		perror("socket");
		goto out;
	}

	memset(&me, 0, sizeof(me));
	me.nl_family = AF_NETLINK;
	me.nl_pid = getpid();

	if (bind(rtnl_socket, (struct sockaddr *)&me, sizeof(me)) < 0) {
		perror("bind");
		goto out;
	}

	memset(&request, 0, sizeof(request));
	request.nl.nlmsg_len = NLMSG_LENGTH(sizeof(struct rtmsg));
	request.nl.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
	request.nl.nlmsg_type = RTM_GETROUTE;
	request.rt.rtm_family = AF_INET6;
	request.nl.nlmsg_len += (rtattr_end - rtattr_start);

	/* address it */
	memset(&them, 0, sizeof(them));
	them.nl_family = AF_NETLINK;

	memset(&msg, 0, sizeof(msg));
	msg.msg_name = (void *)&them;
	msg.msg_namelen = sizeof(them);

	iov.iov_base = (void *) &request.nl;
	iov.iov_len  = request.nl.nlmsg_len;

	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;

	/* send it */
	rc = sendmsg(rtnl_socket, &msg, 0);
	if (rc < 0) {
		perror("sendmsg");
		goto out;
	}

	memset(reply_buf, 0, reply_buf_size);
	rc = recv(rtnl_socket, reply_buf, reply_buf_size, 0);
	if (rc < 0) {
		perror("recv");
		goto out;
	}

	nl_len = rc;
	for (nlp = (struct nlmsghdr *) reply_buf; NLMSG_OK(nlp, nl_len); nlp = NLMSG_NEXT(nlp, nl_len)) {
		rtp = (struct rtmsg *) NLMSG_DATA(nlp);
		rt_len = RTM_PAYLOAD(nlp);

		const int addr_bits = rtp->rtm_dst_len;

		int match = 0;

		for (rtap = (struct rtattr *) RTM_RTA(rtp); RTA_OK(rtap, rt_len); rtap = RTA_NEXT(rtap, rt_len)) {
			if (rtap->rta_type == RTA_DST) {
				if (cmp_bits((uint8_t *) cmp_ip6, (uint8_t *) RTA_DATA(rtap), addr_bits) == 0) {
					match = 1;
					break;
				}
			}
		}

		if (!match) {
			continue;
		}

		/* address is matched; look for output interface, set, and finish */
		for (rtap = (struct rtattr *) RTM_RTA(rtp); RTA_OK(rtap, rt_len); rtap = RTA_NEXT(rtap, rt_len)) {

			if (rtap->rta_type == RTA_OIF) {
				unsigned int ipv6_multicast_if;
				ipv6_multicast_if = *((unsigned int *) RTA_DATA(rtap));
				inSettings->mIPv6MulticastInterface = ipv6_multicast_if;
				ret = 0;
				goto out;
			}
		}
	}

out:
	if (rtnl_socket >= 0) {
		close(rtnl_socket);
	}
	delete [] reply_buf;
#endif
	return ret;
}
// end FindIPv6MulticastInterface

void SetIPv6MulticastInterface( thread_Settings *inSettings )
{
#ifdef HAVE_IPV6_MULTICAST
	if (FindIPv6MulticastInterface(inSettings)) {
		return;
	}
	unsigned int ipv6_multicast_if = inSettings->mIPv6MulticastInterface;
	int rc;
	char ifname[32];
	if_indextoname(ipv6_multicast_if, ifname);

	if ((rc = setsockopt(inSettings->mSock, IPPROTO_IPV6, IPV6_MULTICAST_IF,
					&ipv6_multicast_if, sizeof(ipv6_multicast_if))) < 0) {
		WARN_errno(rc == SOCKET_ERROR, "setsockopt IPV6_MULTICAST_IF");
		return;
	}

	printf(multicast_ipv6_if, ifname, ipv6_multicast_if);
#endif
}
// end SetIPv6MulticastInterface


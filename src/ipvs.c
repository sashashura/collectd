/**
 * collectd - src/ipvs.c (based on ipvsadm and libipvs)
 * Copyright (C) 1997  Steven Clarke <steven@monmouth.demon.co.uk>
 * Copyright (C) 1998-2004  Wensong Zhang <wensong@linuxvirtualserver.org>
 * Copyright (C) 2003-2004  Peter Kese <peter.kese@ijs.si>
 * Copyright (C) 2007  Sebastian Harl
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; only version 2 of the License is applicable.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * Authors:
 *   Sebastian Harl <sh at tokkee.org>
 **/

/*
 * This plugin collects statistics about IPVS connections. It requires Linux
 * kernels >= 2.6.
 *
 * See http://www.linuxvirtualserver.org/software/index.html for more
 * information about IPVS.
 */

#include "collectd.h"
#include "plugin.h"
#include "common.h"

#if HAVE_ARPA_INET_H
# include <arpa/inet.h>
#endif /* HAVE_ARPA_INET_H */
#if HAVE_SYS_SOCKET_H
# include <sys/socket.h>
#endif /* HAVE_SYS_SOCKET_H */
#if HAVE_NETINET_IN_H
# include <netinet/in.h>
#endif /* HAVE_NETINET_IN_H */

/* this can probably only be found in the kernel sources */
#if HAVE_NET_IP_VS_H
# include <net/ip_vs.h>
#elif HAVE_IP_VS_H
# include <ip_vs.h>
#endif /* HAVE_IP_VS_H */

#define log_err(...) ERROR ("ipvs: " __VA_ARGS__)


/*
 * private variables
 */

static int   sockfd    = -1;
static void *ipvs_func = NULL;

static struct ip_vs_getinfo ipvs_info;


/*
 * libipvs API
 */

static struct ip_vs_get_services *ipvs_get_services (void);
static struct ip_vs_get_dests *ipvs_get_dests (struct ip_vs_service_entry *);

static const char *ipvs_strerror (int err)
{
	char errbuf[1024];
	unsigned int i;

	struct {
		void *func;
		int   err;
		const char *message;
	} table [] = {
		{ 0, EPERM, "Permission denied (you must be root)" },
		{ 0, EINVAL, "Module is wrong version" },
		{ 0, ENOPROTOOPT, "Protocol not available" },
		{ 0, ENOMEM, "Memory allocation problem" },
		{ ipvs_get_services, ESRCH, "No such service" },
		{ ipvs_get_dests, ESRCH, "No such service" },
	};

	for (i = 0; i < sizeof (table) / sizeof (table[0]); i++) {
		if (((NULL == table[i].func) || (table[i].func == ipvs_func))
				&& (table[i].err == err))
			return table[i].message;
	}
	return sstrerror (err, errbuf, sizeof (errbuf));
} /* ipvs_strerror */

static struct ip_vs_get_services *ipvs_get_services (void)
{
	struct ip_vs_get_services *ret;
	socklen_t len;

	len = sizeof (*ret) +
		sizeof (struct ip_vs_service_entry) * ipvs_info.num_services;

	if (NULL == (ret = malloc (len))) {
		log_err ("ipvs_get_services: Out of memory.");
		exit (3);
	}

	ipvs_func = ipvs_get_services;

	ret->num_services = ipvs_info.num_services;

	if (0 != getsockopt (sockfd, IPPROTO_IP, IP_VS_SO_GET_SERVICES,
				(void *)ret, &len)) {
		log_err ("ipvs_get_services: getsockopt failed: %s",
				ipvs_strerror (errno));

		free(ret);
		return NULL;
	}
	return ret;
} /* ipvs_get_services */

static struct ip_vs_get_dests *ipvs_get_dests (struct ip_vs_service_entry *se)
{
	struct ip_vs_get_dests *ret;
	socklen_t len;

	len = sizeof (*ret) + sizeof (struct ip_vs_dest_entry) * se->num_dests;

	if (NULL == (ret = malloc (len))) {
		log_err ("ipvs_get_dests: Out of memory.");
		exit (3);
	}

	ipvs_func = ipvs_get_dests;

	ret->fwmark    = se->fwmark;
	ret->protocol  = se->protocol;
	ret->addr      = se->addr;
	ret->port      = se->port;
	ret->num_dests = se->num_dests;

	if (0 != getsockopt (sockfd, IPPROTO_IP, IP_VS_SO_GET_DESTS,
				(void *)ret, &len)) {
		log_err ("ipvs_get_dests: getsockopt() failed: %s",
				ipvs_strerror (errno));
		free (ret);
		return NULL;
	}
	return ret;
} /* ip_vs_get_dests */


/*
 * collectd plugin API and helper functions
 */

static int cipvs_init (void)
{
	socklen_t len;

	len = sizeof (ipvs_info);

	if (-1 == (sockfd = socket (AF_INET, SOCK_RAW, IPPROTO_RAW))) {
		log_err ("cipvs_init: socket() failed: %s", ipvs_strerror (errno));
		return -1;
	}

	if (0 != getsockopt (sockfd, IPPROTO_IP, IP_VS_SO_GET_INFO,
				(void *)&ipvs_info, &len)) {
		log_err ("cipvs_init: getsockopt() failed: %s", ipvs_strerror (errno));
		return -1;
	}
	return 0;
} /* cipvs_init */

/*
 * ipvs-<virtual IP>_{UDP,TCP}<port>/<type>-total
 * ipvs-<virtual IP>_{UDP,TCP}<port>/<type>-<real IP>_<port>
 */

/* plugin instance */
static int get_pi (struct ip_vs_service_entry *se, char *pi, size_t size)
{
	struct in_addr addr;
	int len = 0;

	if ((NULL == se) || (NULL == pi))
		return 0;

	addr.s_addr = se->addr;

	/* inet_ntoa() returns a pointer to a statically allocated buffer
	 * I hope non-glibc systems behave the same */
	len = snprintf (pi, size, "%s_%s%u", inet_ntoa (addr),
			(se->protocol == IPPROTO_TCP) ? "TCP" : "UDP",
			ntohs (se->port));

	if ((0 > len) || (size <= len)) {
		log_err ("plugin instance truncated: %s", pi);
		return -1;
	}
	return 0;
} /* get_pi */

/* type instance */
static int get_ti (struct ip_vs_dest_entry *de, char *ti, size_t size)
{
	struct in_addr addr;
	int len = 0;

	if ((NULL == de) || (NULL == ti))
		return 0;

	addr.s_addr = de->addr;

	/* inet_ntoa() returns a pointer to a statically allocated buffer
	 * I hope non-glibc systems behave the same */
	len = snprintf (ti, size, "%s_%u", inet_ntoa (addr),
			ntohs (de->port));

	if ((0 > len) || (size <= len)) {
		log_err ("type instance truncated: %s", ti);
		return -1;
	}
	return 0;
} /* get_ti */

static void cipvs_submit_connections (char *pi, char *ti, counter_t value)
{
	value_t values[1];
	value_list_t vl = VALUE_LIST_INIT;

	values[0].counter = value;

	vl.values     = values;
	vl.values_len = 1;

	vl.time     = time (NULL);
	vl.interval = interval_g;

	strcpy (vl.host, hostname_g);
	strcpy (vl.plugin, "ipvs");
	strcpy (vl.plugin_instance, pi);
	strcpy (vl.type_instance, (NULL != ti) ? ti : "total");

	plugin_dispatch_values ("connections", &vl);
	return;
} /* cipvs_submit_connections */

static void cipvs_submit_if (char *pi, char *t, char *ti,
		counter_t rx, counter_t tx)
{
	value_t values[2];
	value_list_t vl = VALUE_LIST_INIT;

	values[0].counter = rx;
	values[1].counter = tx;

	vl.values     = values;
	vl.values_len = 2;

	vl.time     = time (NULL);
	vl.interval = interval_g;

	strcpy (vl.host, hostname_g);
	strcpy (vl.plugin, "ipvs");
	strcpy (vl.plugin_instance, pi);
	strcpy (vl.type_instance, (NULL != ti) ? ti : "total");

	plugin_dispatch_values (t, &vl);
	return;
} /* cipvs_submit_if */

static void cipvs_submit_dest (char *pi, struct ip_vs_dest_entry *de) {
	struct ip_vs_stats_user stats = de->stats;

	char ti[DATA_MAX_NAME_LEN];

	if (0 != get_ti (de, ti, DATA_MAX_NAME_LEN))
		return;

	cipvs_submit_connections (pi, ti, stats.conns);
	cipvs_submit_if (pi, "if_packets", ti, stats.inpkts, stats.outpkts);
	cipvs_submit_if (pi, "if_octets", ti, stats.inbytes, stats.outbytes);
	return;
} /* cipvs_submit_dest */

static void cipvs_submit_service (struct ip_vs_service_entry *se)
{
	struct ip_vs_stats_user  stats = se->stats;
	struct ip_vs_get_dests  *dests = ipvs_get_dests (se);

	char pi[DATA_MAX_NAME_LEN];

	int i = 0;

	if (0 != get_pi (se, pi, DATA_MAX_NAME_LEN))
		return;

	cipvs_submit_connections (pi, NULL, stats.conns);
	cipvs_submit_if (pi, "if_packets", NULL, stats.inpkts, stats.outpkts);
	cipvs_submit_if (pi, "if_octets", NULL, stats.inbytes, stats.outbytes);

	for (i = 0; i < dests->num_dests; ++i)
		cipvs_submit_dest (pi, &dests->entrytable[i]);

	free (dests);
	return;
} /* cipvs_submit_service */

static int cipvs_read (void)
{
	struct ip_vs_get_services *services = NULL;

	int i = 0;

	if (NULL == (services = ipvs_get_services ()))
		return -1;

	for (i = 0; i < services->num_services; ++i)
		cipvs_submit_service (&services->entrytable[i]);

	free (services);
	return 0;
} /* cipvs_read */

static int cipvs_shutdown (void)
{
	close (sockfd);
	return 0;
} /* cipvs_shutdown */

void module_register (void)
{
	plugin_register_init ("ipvs", cipvs_init);
	plugin_register_read ("ipvs", cipvs_read);
	plugin_register_shutdown ("ipvs", cipvs_shutdown);
	return;
} /* module_register */

/* vim: set sw=4 ts=4 tw=78 noexpandtab : */


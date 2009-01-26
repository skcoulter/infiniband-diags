/*
 * Copyright (c) 2004-2008 Voltaire Inc.  All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

#if HAVE_CONFIG_H
#  include <config.h>
#endif /* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <getopt.h>

#include <infiniband/umad.h>
#include <infiniband/mad.h>

#include "ibdiag_common.h"

#define MAX_CPUS 8

enum ib_sysstat_attr_t {
	IB_PING_ATTR = 0x10,
	IB_HOSTINFO_ATTR = 0x11,
	IB_CPUINFO_ATTR = 0x12,
};

typedef struct cpu_info {
	char *model;
	char *mhz;
} cpu_info;

static cpu_info cpus[MAX_CPUS];
static int host_ncpu;

static int server_respond(void *umad, int size)
{
	ib_rpc_t rpc = {0};
	ib_rmpp_hdr_t rmpp = {0};
	ib_portid_t rport;
	uint8_t *mad = umad_get_mad(umad);
	ib_mad_addr_t *mad_addr;

	if (!(mad_addr = umad_get_mad_addr(umad)))
		return -1;

	memset(&rport, 0, sizeof(rport));

	rport.lid = ntohs(mad_addr->lid);
	rport.qp = ntohl(mad_addr->qpn);
	rport.qkey = ntohl(mad_addr->qkey);
	rport.sl = mad_addr->sl;
	if (!rport.qkey && rport.qp == 1)
		rport.qkey = IB_DEFAULT_QP1_QKEY;

	rpc.mgtclass = mad_get_field(mad, 0, IB_MAD_MGMTCLASS_F);
	rpc.method = IB_MAD_METHOD_GET | IB_MAD_RESPONSE;
	rpc.attr.id = mad_get_field(mad, 0, IB_MAD_ATTRID_F);
	rpc.attr.mod = mad_get_field(mad, 0, IB_MAD_ATTRMOD_F);
	rpc.oui = mad_get_field(mad, 0, IB_VEND2_OUI_F);
	rpc.trid = mad_get_field64(mad, 0, IB_MAD_TRID_F);

	rmpp.flags = IB_RMPP_FLAG_ACTIVE;

	DEBUG("responding %d bytes to %s, attr 0x%x mod 0x%x qkey %x",
	      size, portid2str(&rport), rpc.attr.id, rpc.attr.mod, rport.qkey);

	if (mad_build_pkt(umad, &rpc, &rport, &rmpp, 0) < 0)
		return -1;

	if (ibdebug > 1)
		xdump(stderr, "mad respond pkt\n", mad, IB_MAD_SIZE);

	if (umad_send(madrpc_portid(), mad_class_agent(rpc.mgtclass), umad,
		      size, rpc.timeout, 0) < 0) {
		DEBUG("send failed; %m");
		return -1;
	}

	return 0;
}

static int mk_reply(int attr, void *data, int sz)
{
	char *s = data;
	int n, i, ret = 0;

	switch (attr) {
	case IB_PING_ATTR:
		break;		/* nothing to do here, just reply */
	case IB_HOSTINFO_ATTR:
		if (gethostname(s, sz) < 0)
			snprintf(s, sz, "?hostname?");
		s[sz-1] = 0;
		if ((n = strlen(s)) >= sz - 1) {
			ret = sz;
			break;
		}
		s[n] = '.';
		s += n+1;
		sz -= n+1;
		ret += n + 1;
		if (getdomainname(s, sz) < 0)
			snprintf(s, sz, "?domainname?");
		if ((n = strlen(s)) == 0)
			s[-1] = 0;	/* no domain */
		else
			ret += n;
		break;
	case IB_CPUINFO_ATTR:
		s[0] = '\0';
		for (i = 0; i < host_ncpu && sz > 0; i++) {
			n = snprintf(s, sz, "cpu %d: model %s MHZ %s\n",
				     i, cpus[i].model, cpus[i].mhz);
			if (n >= sz) {
				IBWARN("cpuinfo truncated");
				ret = sz;
				break;
			}
			sz -= n;
			s += n;
			ret += n;
		}
		ret++;
		break;
	default:
		DEBUG("unknown attr %d", attr);
	}
	return ret;
}

static uint8_t buf[2048];

static char *ibsystat_serv(void)
{
	void *umad;
	void *mad;
	int attr, mod, size;

	DEBUG("starting to serve...");

	while ((umad = mad_receive(buf, -1))) {

		mad = umad_get_mad(umad);

		attr = mad_get_field(mad, 0, IB_MAD_ATTRID_F);
		mod = mad_get_field(mad, 0, IB_MAD_ATTRMOD_F);

		DEBUG("got packet: attr 0x%x mod 0x%x", attr, mod);

		size = mk_reply(attr, mad + IB_VENDOR_RANGE2_DATA_OFFS,
				sizeof(buf) - umad_size() - IB_VENDOR_RANGE2_DATA_OFFS);

		if (server_respond(umad, IB_VENDOR_RANGE2_DATA_OFFS + size) < 0)
			DEBUG("respond failed");
	}

	DEBUG("server out");
	return 0;
}

static int
match_attr(char *str)
{
	if (!strcmp(str, "ping"))
		return IB_PING_ATTR;
	if (!strcmp(str, "host"))
		return IB_HOSTINFO_ATTR;
	if (!strcmp(str, "cpu"))
		return IB_CPUINFO_ATTR;
	return -1;
}

static char *ibsystat(ib_portid_t *portid, int attr)
{
	ib_rpc_t rpc = { 0 };
	int fd, agent, timeout, len;
	void *data = umad_get_mad(buf) + IB_VENDOR_RANGE2_DATA_OFFS;

	DEBUG("Sysstat ping..");

	rpc.mgtclass = IB_VENDOR_OPENIB_SYSSTAT_CLASS;
	rpc.method = IB_MAD_METHOD_GET;
	rpc.attr.id = attr;
	rpc.attr.mod = 0;
	rpc.oui = IB_OPENIB_OUI;
	rpc.timeout = 0;
	rpc.datasz = IB_VENDOR_RANGE2_DATA_SIZE;
	rpc.dataoffs = IB_VENDOR_RANGE2_DATA_OFFS;

	portid->qp = 1;
	if (!portid->qkey)
		portid->qkey = IB_DEFAULT_QP1_QKEY;

	if ((len = mad_build_pkt(buf, &rpc, portid, NULL, NULL)) < 0)
		IBPANIC("cannot build packet.");

	fd = madrpc_portid();
	agent = mad_class_agent(rpc.mgtclass);
	timeout = ibd_timeout ? ibd_timeout : MAD_DEF_TIMEOUT_MS;

	if (umad_send(fd, agent, buf, len, timeout, 0) < 0)
		IBPANIC("umad_send failed.");

	len = sizeof(buf) - umad_size();
	if (umad_recv(fd, buf, &len, timeout) < 0)
		IBPANIC("umad_recv failed.");

	DEBUG("Got sysstat pong..");
	if (attr != IB_PING_ATTR)
		puts(data);
	else
		printf("sysstat ping succeeded\n");
	return 0;
}

int
build_cpuinfo(void)
{
	char line[1024] = {0}, *s, *e;
	FILE *f;
	int ncpu = 0;

	if (!(f = fopen("/proc/cpuinfo", "r"))) {
		IBWARN("couldn't open /proc/cpuinfo");
		return 0;
	}

	while (fgets(line, sizeof(line) - 1, f)) {
		if (!strncmp(line, "processor\t", 10)) {
			ncpu++;
			if (ncpu > MAX_CPUS)
				return MAX_CPUS;
			continue;
		}

		if (!ncpu || !(s = strchr(line, ':')))
			continue;

		if ((e = strchr(s, '\n')))
			*e = 0;
		if (!strncmp(line, "model name\t", 11))
			cpus[ncpu-1].model = strdup(s+1);
		else if (!strncmp(line, "cpu MHz\t", 8))
			cpus[ncpu-1].mhz = strdup(s+1);
	}

	fclose(f);

	DEBUG("ncpu %d", ncpu);

	return ncpu;
}

static int server = 0, oui = IB_OPENIB_OUI;

static int process_opt(void *context, int ch, char *optarg)
{
	switch (ch) {
	case 'o':
		oui = strtoul(optarg, 0, 0);
		break;
	case 'S':
		server++;
		break;
	default:
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	int mgmt_classes[3] = {IB_SMI_CLASS, IB_SMI_DIRECT_CLASS, IB_SA_CLASS};
	int sysstat_class = IB_VENDOR_OPENIB_SYSSTAT_CLASS;
	ib_portid_t portid = {0};
	int attr = IB_PING_ATTR;
	char *err;

	const struct ibdiag_opt opts[] = {
		{ "oui", 'o', 1, NULL, "use specified OUI number" },
		{ "Server", 'S', 0, NULL, "start in server mode" },
		{ }
	};
	char usage_args[] = "<dest lid|guid> [<op>]";

	ibdiag_process_opts(argc, argv, NULL, "D", opts, process_opt,
			    usage_args, NULL);

	argc -= optind;
	argv += optind;

	if (!argc && !server)
		ibdiag_show_usage();

	if (argc > 1 && (attr = match_attr(argv[1])) < 0)
		ibdiag_show_usage();

	madrpc_init(ibd_ca, ibd_ca_port, mgmt_classes, 3);

	if (server) {
		if (mad_register_server(sysstat_class, 1, 0, oui) < 0)
			IBERROR("can't serve class %d", sysstat_class);

		host_ncpu = build_cpuinfo();

		if ((err = ibsystat_serv()))
			IBERROR("ibssystat to %s: %s", portid2str(&portid), err);
		exit(0);
	}

	if (mad_register_client(sysstat_class, 1) < 0)
		IBERROR("can't register to sysstat class %d", sysstat_class);

	if (ib_resolve_portid_str(&portid, argv[0], ibd_dest_type, ibd_sm_id) < 0)
		IBERROR("can't resolve destination port %s", argv[0]);

	if ((err = ibsystat(&portid, attr)))
		IBERROR("ibsystat to %s: %s", portid2str(&portid), err);

	exit(0);
}

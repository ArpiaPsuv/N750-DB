/*
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston,
 * MA 02111-1307 USA
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <nvram/bcmnvram.h>
#include <shutils.h>

#include "rc.h"

#define VPNS_LOG_NAME		"VPN server"
#define VPNS_PPP_UP_SCRIPT	"/tmp/ppp/ip-up.vpns"
#define VPNS_PPP_DW_SCRIPT	"/tmp/ppp/ip-down.vpns"
#define VPNS_CLIENT_SCRIPT	"/etc/storage/vpns_client_script.sh"
#define VPNS_PPP_UNIT		10

static int xl2tpd_killed_vpns = 0;

int 
start_vpn_server(void)
{
	int i_type, i_cast, i_mppe, i_auth, i_mtu, i_mru, i_dhcp, i_cli0, i_cli1;
	int i, i_max, i_cli2, i_dns;
	char *vpns_cfg, *vpns_opt, *vpns_sec, *lanip, *wins, *dns1, *dns2, *acl_user, *acl_pass;
	char acl_user_var[32], acl_pass_var[32], acl_addr_var[32];
	struct in_addr pool_in;
	unsigned int laddr, lmask;
	FILE *fp;

	if (nvram_invmatch("vpns_enable", "1") || is_ap_mode())
		return 0;

	unlink(VPN_SERVER_LEASE_FILE);

	i_type = nvram_get_int("vpns_type");
#if defined(APP_OPENVPN)
	if (i_type == 2)
		return start_openvpn_server();
#endif
	vpns_cfg = "/etc/pptpd.conf";
	vpns_sec = "/tmp/ppp/chap-secrets";
	vpns_opt = VPN_SERVER_PPPD_OPTIONS;

	mkdir("/tmp/ppp", 0777);
	symlink("/sbin/rc", VPNS_PPP_UP_SCRIPT);
	symlink("/sbin/rc", VPNS_PPP_DW_SCRIPT);
	
	i_cast = nvram_get_int("vpns_cast");
	i_auth = nvram_get_int("vpns_auth");
	i_mppe = nvram_get_int("vpns_mppe");
	i_mtu  = nvram_get_int("vpns_mtu");
	i_mru  = nvram_get_int("vpns_mru");
	i_dhcp = nvram_get_int("dhcp_enable_x");

	lanip  = nvram_safe_get("lan_ipaddr");

	if (!nvram_get_int("vpns_vuse"))
	{
		laddr = ntohl(inet_addr(lanip));
		lmask = ntohl(inet_addr(nvram_safe_get("lan_netmask")));
		
		i_cli0 = nvram_get_int("vpns_cli0");
		i_cli1 = nvram_get_int("vpns_cli1");
		if (i_cli0 <   2) i_cli0 =   2;
		if (i_cli0 > 254) i_cli0 = 254;
		if (i_cli1 <   2) i_cli1 =   2;
		if (i_cli1 > 254) i_cli1 = 254;
		if (i_cli1 < i_cli0) i_cli1 = i_cli0;
	}
	else
	{
		laddr = ntohl(inet_addr(nvram_safe_get("vpns_vnet")));
		lmask = ntohl(inet_addr(VPN_SERVER_SUBNET_MASK));
		laddr = (laddr & lmask) | 1;
		
		i_cli0 = 2;
		i_cli1 = 11;
	}

	if (i_mtu < 1000) i_mtu = 1000;
	if (i_mtu > 1460) i_mtu = 1460;
	if (i_mru < 1000) i_mru = 1000;
	if (i_mru > 1460) i_mru = 1460;

	if (i_type != 1)
	{
		if (!(fp = fopen(vpns_cfg, "w")))
			return -1;
		
		fprintf(fp, "option %s\n", vpns_opt);
		fprintf(fp, "connections %d\n", 10);
		
		pool_in.s_addr = htonl(laddr);
		fprintf(fp, "localip %s\n", inet_ntoa(pool_in));
		
		pool_in.s_addr = htonl((laddr & lmask) | (unsigned int)i_cli0);
		fprintf(fp, "remoteip %s-%d\n", inet_ntoa(pool_in), i_cli1);
		
		fclose(fp);
		
		chmod(vpns_cfg, 0644);
	}

	// Create options for pppd
	if (!(fp = fopen(vpns_opt, "w"))) {
		return -1;
	}

	fprintf(fp, "lock\n");
	fprintf(fp, "name %s\n", get_our_hostname());
	fprintf(fp, "auth\n");
	fprintf(fp, "refuse-eap\n");
	fprintf(fp, "refuse-pap\n");
	fprintf(fp, "refuse-mschap\n");

	if (i_auth == 0)
	{
		fprintf(fp, "refuse-chap\n");
		fprintf(fp, "require-mschap-v2\n");
	}

	/* looks like pptp also likes them */
	fprintf(fp, "default-asyncmap nopcomp noaccomp\n");

	/* ccp should still be enabled - mppe/mppc requires this */
	fprintf(fp, "novj nobsdcomp nodeflate\n");

	if (i_mppe == 3) {
		fprintf(fp, "nomppe nomppc\n");
	} else {
		if (i_mppe == 1) {
			fprintf(fp, "+mppe\n");
			fprintf(fp, "-mppe-40\n");
			fprintf(fp, "+mppe-128\n");
		} else if (i_mppe == 2) {
			fprintf(fp, "+mppe\n");
			fprintf(fp, "+mppe-40\n");
			fprintf(fp, "-mppe-128\n");
		} else {
			fprintf(fp, "+mppe-40\n");
			fprintf(fp, "+mppe-128\n");
		}
		fprintf(fp, "nomppe-stateful\n");
	}

	// DNS Server
	i_dns = 0;
	if (i_dhcp == 1)
	{
		dns1 = nvram_safe_get("dhcp_dns1_x");
		dns2 = nvram_safe_get("dhcp_dns2_x");
		if ((inet_addr_(dns1) != INADDR_ANY) && (strcmp(dns1, lanip))) {
			i_dns++;
			fprintf(fp, "ms-dns %s\n", dns1);
		}
		if ((inet_addr_(dns2) != INADDR_ANY) && (strcmp(dns2, lanip)) && (strcmp(dns2, dns1))) {
			i_dns++;
			fprintf(fp, "ms-dns %s\n", dns2);
		}
	}

	if (i_dns < 2)
		fprintf(fp, "ms-dns %s\n", lanip);

	if (i_dhcp == 1)
	{
		// WINS Server
		wins = nvram_safe_get("dhcp_wins_x");
		if (inet_addr_(wins) != INADDR_ANY) 
		{
			fprintf(fp, "ms-wins %s\n", wins);
		}
	}

	fprintf(fp, "mtu %d\n", i_mtu);
	fprintf(fp, "mru %d\n", i_mru);
	fprintf(fp, "ipcp-accept-remote ipcp-accept-local\n");
	fprintf(fp, "nodefaultroute\n");
	fprintf(fp, "proxyarp\n");

	if (i_type == 1)
	{
		// L2TP: Don't wait for LCP term responses; exit immediately when killed
		fprintf(fp, "lcp-max-terminate %d\n", 0);
	}

	/* echo failures (6*20s) */
	fprintf(fp, "lcp-echo-interval %d\n", 20);
	fprintf(fp, "lcp-echo-failure %d\n", 6);
	fprintf(fp, "lcp-echo-adaptive\n");

	fprintf(fp, "ip-up-script %s\n", VPNS_PPP_UP_SCRIPT);
	fprintf(fp, "ip-down-script %s\n", VPNS_PPP_DW_SCRIPT);
	fprintf(fp, "minunit %d\n", VPNS_PPP_UNIT);

	fclose(fp);

	chmod(vpns_opt, 0644);

	/* create /tmp/ppp/chap-secrets */
	fp = fopen(vpns_sec, "w+");
	if (fp) {
		i_max = nvram_get_int("vpns_num_x");
		if (i_max > 10) i_max = 10;
		for (i = 0; i < i_max; i++) {
			sprintf(acl_user_var, "vpns_user_x%d", i);
			sprintf(acl_pass_var, "vpns_pass_x%d", i);
			acl_user = nvram_safe_get(acl_user_var);
			acl_pass = nvram_safe_get(acl_pass_var);
			if (*acl_user && *acl_pass)
			{
				sprintf(acl_addr_var, "vpns_addr_x%d", i);
				i_cli2 = nvram_get_int(acl_addr_var);
				if (i_cli2 >= i_cli0 && i_cli2 <= i_cli1 ) {
					pool_in.s_addr = htonl((laddr & lmask) | (unsigned int)i_cli2);
					strcpy(acl_addr_var, inet_ntoa(pool_in));
				}
				else
					strcpy(acl_addr_var, "*");
				
				fprintf(fp, "\"%s\"	*	\"%s\"	%s\n", acl_user, acl_pass, acl_addr_var);
			}
		}
		fclose(fp);
		
		chmod(vpns_sec, 0600);
	}

	if (i_type == 1)
	{
		nvram_set_int("l2tp_srv_t", 1);
		
		safe_start_xl2tpd();
	}
	else
	{
		nvram_set_int("l2tp_srv_t", 0);
		
		/* execute pptpd daemon */
		return eval("/usr/sbin/pptpd", "-c", vpns_cfg);
	}

	return 0;
}

void 
stop_vpn_server(void)
{
	int i;
	char pppd_pid[32];
	char* svcs[] = { "bcrelay", "pptpd", NULL,  NULL };

	if ((nvram_match("vpns_enable", "1") && nvram_match("vpns_type", "1")) || nvram_match("l2tp_srv_t", "1")) {
		svcs[2] = "xl2tpd";
		xl2tpd_killed_vpns = 1;
	}

	kill_services(svcs, 5, 1);

	/* force kill all pppd */
	for (i=0; i<10; i++) {
		sprintf(pppd_pid, "/var/run/ppp%d.pid", VPNS_PPP_UNIT+i);
		kill_pidfile_s(pppd_pid, SIGKILL);
	}

	nvram_set_int("l2tp_srv_t", 0);

	unlink(VPNS_PPP_UP_SCRIPT);
	unlink(VPNS_PPP_DW_SCRIPT);

#if defined(APP_OPENVPN)
	stop_openvpn_server();
#endif

	unlink(VPN_SERVER_LEASE_FILE);
}

void 
restart_vpn_server(void)
{
	xl2tpd_killed_vpns = 0;

	stop_vpn_server();
	start_vpn_server();

	restart_firewall();

	/* restore L2TP WAN client or L2TP VPNC */
	if (xl2tpd_killed_vpns && (nvram_match("l2tp_wan_t", "1") || nvram_match("l2tp_cli_t", "1")))
		safe_start_xl2tpd();
}

int
ipup_vpns_main(int argc, char **argv)
{
	FILE *fp;
	int i_cast;
	char *peer_name;
	char *script_name = VPNS_CLIENT_SCRIPT;

	if (argc < 7)
		return -1;

	peer_name = safe_getenv("PEERNAME");

	logmessage(VPNS_LOG_NAME, "peer %s (%s) connected - ifname: %s, local IP: %s",
			argv[6], peer_name, argv[1], argv[5]);

	fp = fopen(VPN_SERVER_LEASE_FILE, "a+");
	if (fp)
	{
		fprintf(fp, "%s %s %s %s\n", argv[1], argv[5], argv[6], peer_name);
		fclose(fp);
	}

	if (!nvram_get_int("vpns_vuse") && !pids("bcrelay"))
	{
		i_cast = nvram_get_int("vpns_cast");
		if (i_cast == 1 || i_cast == 3)
			eval("/usr/sbin/bcrelay", "-d", "-i", IFNAME_BR, "-o", "ppp[1-2][0-9]", "-n");
		if (i_cast == 2 || i_cast == 3)
			eval("/usr/sbin/bcrelay", "-d", "-i", "ppp[1-2][0-9]", "-o", IFNAME_BR, "-n");
	}

	if (check_if_file_exist(script_name))
		doSystem("%s %s %s %s %s %s", script_name, "up", argv[1], argv[5], argv[6], peer_name);

	return 0;
}

int
ipdown_vpns_main(int argc, char **argv)
{
	FILE *fp1, *fp2;
	int i_clients;
	char ifname[16], addr_l[32], addr_r[64], name_p[64];
	char *peer_name;
	char *clients_l1 = VPN_SERVER_LEASE_FILE;
	char *clients_l2 = "/tmp/.vpns.leases";
	char *script_name = VPNS_CLIENT_SCRIPT;
	char *svcs[] = { "bcrelay", NULL };

	if (argc < 7)
		return -1;

	peer_name = safe_getenv("PEERNAME");

	logmessage(VPNS_LOG_NAME, "peer %s (%s) disconnected", argv[6], peer_name);

	i_clients = 0;
	fp1 = fopen(clients_l1, "r");
	fp2 = fopen(clients_l2, "w");
	if (fp1)
	{
		while(fscanf(fp1, "%s %s %s %[^\n]\n", ifname, addr_l, addr_r, name_p) == 4)
		{
			if (strcmp(ifname, argv[1]))
			{
				i_clients++;
				if (fp2)
					fprintf(fp2, "%s %s %s %s\n", ifname, addr_l, addr_r, name_p);
			}
		}
		
		fclose(fp1);
	}

	if (fp2)
	{
		fclose(fp2);
		rename(clients_l2, clients_l1);
		unlink(clients_l2);
	}

	if (check_if_file_exist(script_name))
		doSystem("%s %s %s %s %s %s", script_name, "down", argv[1], argv[5], argv[6], peer_name);

	if (i_clients == 0 && pids(svcs[0]))
		kill_services(svcs, 3, 1);

	return 0;
}

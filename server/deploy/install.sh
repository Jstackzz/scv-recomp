#!/bin/sh
# Installs the SC5 Online server as an always-on service on a Linux machine with a public address (an Oracle Cloud
# VM, a VPS: Ubuntu, Debian, Fedora...). Copy the repository (server/ at least) to the machine and run it as root from
# the repository:
#
#     sudo sh server/deploy/install.sh [name-or-address-players-use [oldest-launcher-version]]
#
# It copies server/ to /opt/sc5, keeps the database and certificate in /var/lib/sc5, makes the certificate the
# first time, runs the server under systemd as the service sc5-server and the user sc5 (restarted if it stops, started
# at boot, logging to the journal), and opens its TCP and UDP ports in the machine's own firewall: ufw or firewalld
# when one is active, and the iptables rules Oracle Cloud's Ubuntu images keep in /etc/iptables/rules.v4 (they let in
# only SSH). Running it again updates the server and keeps the certificate and the accounts. The cloud's network
# firewall (Oracle's security list, a security group) must allow the same two ports. Given an oldest launcher version
# (after a release every player must take), the server tells older launchers to update; it is kept in
# /var/lib/sc5/min-client for later runs.
#
# The ports are TCP 7777 and UDP 7778 unless SC5_TCP_PORT and SC5_UDP_PORT choose others, for a machine that already
# runs another server on those: this one has its own service, user, folders and firewall rules and touches none of
# the other's.
#
#     sudo SC5_TCP_PORT=7787 SC5_UDP_PORT=7788 sh server/deploy/install.sh lobby.example.org
set -eu
tcp=${SC5_TCP_PORT:-7777}
udp=${SC5_UDP_PORT:-7778}

repo=$(cd "$(dirname "$0")/../.." && pwd)
data=/var/lib/sc5
app=/opt/sc5

for port in "$tcp" "$udp"; do
    if ! printf '%s\n' "$port" | grep -Eqx '[1-9][0-9]{0,4}' || [ "$port" -gt 65535 ]; then
        echo "SC5_TCP_PORT and SC5_UDP_PORT are port numbers from 1 to 65535, not $port." >&2
        exit 1
    fi
done
if ! python3 -c 'import sys; sys.exit(sys.version_info < (3, 11))' 2>/dev/null; then
    echo "The server needs Python 3.11 or newer as python3 (Ubuntu 24.04 has 3.12)." >&2
    exit 1
fi
id sc5 >/dev/null 2>&1 || useradd --system --home-dir "$data" --shell /usr/sbin/nologin sc5
mkdir -p "$data" "$app"
if [ -n "${2:-}" ]; then
    if ! printf '%s\n' "$2" | grep -Eqx '[0-9]+(\.[0-9]+)*'; then
        echo "An oldest launcher version looks like 0.1.4, not $2." >&2
        exit 1
    fi
    echo "$2" > "$data/min-client"
fi
min_client=$(cat "$data/min-client" 2>/dev/null || echo 0.1.0)
rm -rf "$app/server"
cp -r "$repo/server" "$app/server"
rm -rf "$app/server/tests" "$app/server/deploy"
if [ ! -f "$data/server-cert.pem" ]; then
    (cd "$app" && python3 -m server.makecert --out-dir "$data" ${1:+--host "$1"})
fi
chown -R sc5: "$data"
chmod 600 "$data/server-key.pem"

cat > /etc/systemd/system/sc5-server.service <<EOF
[Unit]
Description=SC5 Online server
After=network-online.target
Wants=network-online.target

[Service]
User=sc5
WorkingDirectory=$app
ExecStart=/usr/bin/env python3 -m server --host 0.0.0.0 --port $tcp --udp-port $udp --db $data/server.sqlite --min-client $min_client
Restart=always
RestartSec=3
NoNewPrivileges=yes
PrivateTmp=yes
ProtectSystem=full
ProtectHome=yes

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable sc5-server >/dev/null
systemctl restart sc5-server

if command -v ufw >/dev/null 2>&1 && ufw status | grep -q "Status: active"; then
    ufw allow "$tcp/tcp" >/dev/null && ufw allow "$udp/udp" >/dev/null
fi
if command -v firewall-cmd >/dev/null 2>&1 && firewall-cmd --state >/dev/null 2>&1; then
    firewall-cmd --quiet --permanent --add-port="$tcp/tcp" --add-port="$udp/udp" && firewall-cmd --quiet --reload
fi
if [ -f /etc/iptables/rules.v4 ] && command -v iptables >/dev/null 2>&1; then
    # ahead of the rule that rejects everything else, then saved so a reboot keeps them
    iptables -C INPUT -p tcp --dport "$tcp" -j ACCEPT 2>/dev/null || iptables -I INPUT 1 -p tcp --dport "$tcp" -j ACCEPT
    iptables -C INPUT -p udp --dport "$udp" -j ACCEPT 2>/dev/null || iptables -I INPUT 1 -p udp --dport "$udp" -j ACCEPT
    if command -v netfilter-persistent >/dev/null 2>&1; then
        netfilter-persistent save >/dev/null 2>&1
    else
        iptables-save > /etc/iptables/rules.v4
    fi
fi

sleep 2
if ! systemctl is-active --quiet sc5-server; then
    journalctl -u sc5-server --no-pager -n 20 >&2
    echo "The server did not stay up; its last log lines are above." >&2
    exit 1
fi
echo
echo "SC5 server running on TCP $tcp and UDP $udp; launchers older than $min_client are told to update."
echo "Certificate fingerprint:"
(cd "$app" && python3 -m server.makecert --out-dir "$data" --show)
echo "Players need the address ${1:-<the server address>}:$tcp and the fingerprint above: put both in the launcher's"
echo "defaults.ini ([server] address= and fingerprint=) or type them into its Settings."
echo "Its log: journalctl -u sc5-server -f"

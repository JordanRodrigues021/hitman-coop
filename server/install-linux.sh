#!/bin/sh
# Installs the HITMAN Co-op relay server (created by Jordan Rodrigues) as a systemd service on a
# Linux server with a public IP address (any small VPS works: 1 CPU, 512 MB RAM is plenty).
#
#   curl -O .../relay.py -O .../install-linux.sh     (or copy both files over with scp)
#   sudo sh install-linux.sh [port]
#
# The service runs as an unprivileged user, restarts on failure, and starts on boot.
set -eu

PORT="${1:-47100}"
HERE="$(cd "$(dirname "$0")" && pwd)"

if [ "$(id -u)" -ne 0 ]; then
    echo "Run as root: sudo sh install-linux.sh" >&2
    exit 1
fi

command -v python3 >/dev/null || { apt-get update && apt-get install -y python3; } || { dnf install -y python3; }

id hitmancoop >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin hitmancoop
install -d -m 755 /opt/hitmancoop
install -m 644 "$HERE/relay.py" /opt/hitmancoop/relay.py

cat > /etc/systemd/system/hitmancoop-relay.service <<EOF
[Unit]
Description=HITMAN Co-op relay server (created by Jordan Rodrigues)
After=network-online.target
Wants=network-online.target

[Service]
User=hitmancoop
ExecStart=/usr/bin/python3 -I /opt/hitmancoop/relay.py --port ${PORT}
Restart=always
RestartSec=2
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable --now hitmancoop-relay

# Open the port in the local firewall if one is active (cloud providers may also need it opened
# in their web console, e.g. a "security group" or "firewall" rule for UDP ${PORT}).
if command -v ufw >/dev/null && ufw status | grep -q active; then ufw allow "${PORT}/udp"; fi
if command -v firewall-cmd >/dev/null && firewall-cmd --state >/dev/null 2>&1; then
    firewall-cmd --permanent --add-port="${PORT}/udp" && firewall-cmd --reload
fi

sleep 1
systemctl --no-pager --lines=3 status hitmancoop-relay || true
echo
echo "Relay running on UDP port ${PORT}. In the mod, set Co-op server to this machine's address,"
echo "or build the mod with -DCOOP_DEFAULT_RELAY=<address>:${PORT} so everyone gets it by default."

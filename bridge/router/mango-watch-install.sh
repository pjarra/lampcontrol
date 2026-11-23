#!/bin/sh
# Install the Mango watchdog and a nightly restart (04:30), and set the router's time zone to Berlin.
#   ./mango-watch-install.sh            (SSH key as for mango-diag.sh; ROUTER overridable)
# Undo on the router: crontab -e (remove both lines), rm /root/mango-watch.sh
set -e
cd "$(dirname "$0")"
ROUTER=${ROUTER:-192.168.8.1}
SSH="ssh -i $HOME/.ssh/mango_rsa -o IdentitiesOnly=yes -o UserKnownHostsFile=$HOME/.ssh/known_hosts_mango root@$ROUTER"
$SSH 'cat > /root/mango-watch.sh && chmod +x /root/mango-watch.sh' < mango-watch.sh
$SSH sh <<'EOF'
set -e
uci set system.@system[0].zonename='Europe/Berlin'
uci set system.@system[0].timezone='CET-1CEST,M3.5.0,M10.5.0/3'
uci commit system
/etc/init.d/system reload
touch /etc/crontabs/root
sed -i '/mango-watch\|nightly restart/d' /etc/crontabs/root
# The restart waits 70 s and touches /etc/banner: without a clock chip the router may boot with a time
# before 04:30 and would otherwise restart again (OpenWrt's usual recipe).
cat >> /etc/crontabs/root <<'CRON'
* * * * * /root/mango-watch.sh
30 4 * * * sleep 70 && touch /etc/banner && reboot  # nightly restart
CRON
grep -q mango-watch /etc/sysupgrade.conf || { echo /root/mango-watch.sh; echo /root/crashlogs/; } >> /etc/sysupgrade.conf
/etc/init.d/cron enable
/etc/init.d/cron restart
date
crontab -l
EOF

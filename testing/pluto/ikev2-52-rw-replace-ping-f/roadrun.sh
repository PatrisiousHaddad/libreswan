#!/bin/sh
../../guestbin/ping-once.sh --up 192.1.2.23
ipsec auto --up road-east-x509-ipv4
../../guestbin/ping-once.sh --up -I 192.0.2.100 192.1.2.23
ipsec whack --trafficstatus
ping -n -q -c 1230000 -f -I 192.0.2.100 192.1.2.23 &
sleep 60
sleep 60
grep -E  'EVENT_SA_EXPIRE|EVENT_v2_REPLACE' OUTPUT/road.pluto.log  | head -9
echo "re-authenticateded. The state number should 3 and 4"
ipsec whack --trafficstatus
# expect only 8 ICMP packets
tcpdump -t -nn -r OUTPUT/swan12.pcap icmp 2>/dev/null |wc -l
echo done

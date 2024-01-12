/testing/guestbin/swan-prep --fips
ip addr add 192.0.100.254/24 dev eth0:1
ip route add 192.0.200.0/24 via 192.1.2.23  dev eth1
# ensure that clear text does not get through
iptables -A INPUT -i eth1 -s 192.0.2.0/24 -j DROP
iptables -A INPUT -i eth1 -s 192.0.200.0/24 -j DROP
iptables -I INPUT -m policy --dir in --pol ipsec -j ACCEPT
ipsec start
../../guestbin/wait-until-pluto-started
ipsec whack --impair suppress_retransmits
echo "initdone"

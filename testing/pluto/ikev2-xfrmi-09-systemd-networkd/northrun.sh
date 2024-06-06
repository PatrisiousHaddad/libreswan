ipsec auto --up north
sleep  2 # why?
../../guestbin/tcpdump.sh --start -i ipsec2
../../guestbin/ping-once.sh --up  192.0.2.254
../../guestbin/ping-once.sh --up  192.0.2.254
../../guestbin/ping-once.sh --up  192.0.2.254
../../guestbin/ping-once.sh --up  192.0.2.254
../../guestbin/ip.sh -s link show ipsec2
../../guestbin/ip.sh rule show
../../guestbin/ip.sh route
../../guestbin/ip.sh route show table 50
sleep 20 # wait for tcpdump to collect all events
../../guestbin/tcpdump.sh --stop -i ipsec2
# rm the test specific systemd-networkd file for next test
rm /etc/systemd/network/ipsec2.netdev
echo done

# unfortunately does not yet indicate it is using TCP
ipsec up ikev2-west-east
../../guestbin/ping-once.sh --up 192.1.2.23
ipsec whack --trafficstatus
# should show tcp being used
../../guestbin/ipsec-kernel-state.sh
../../guestbin/ipsec-kernel-policy.sh 2>/dev/null | grep encap
../../guestbin/ipsec-kernel-state.sh
../../guestbin/ipsec-kernel-policy.sh
ipsec auto --down ikev2-west-east
echo "done"

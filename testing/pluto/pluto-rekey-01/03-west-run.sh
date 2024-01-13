ipsec auto --up westnet-eastnet-ipv4-psk-ikev2
../../guestbin/ping-once.sh --up -I 192.0.1.254 192.0.2.254

# wait for an IKE rekey which happens at the 1m mark
sleep 45
../../guestbin/wait-for-pluto.sh '#3: initiating .* to replace #1'
../../guestbin/wait-for.sh --match '#3: .* SA established' -- ipsec whack --showstates

# because both ends are fighting over who is establishing the ISAKMP
# SA, showstates is always changing.

# ready for shutdown test in final.sh
echo done

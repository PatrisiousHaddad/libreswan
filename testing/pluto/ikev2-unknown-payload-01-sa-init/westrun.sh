ipsec whack --impair none
ipsec whack --impair revival
ipsec whack --impair suppress_retransmits # one packet
ipsec whack --impair add_unknown_v2_payload_to:IKE_SA_INIT
: good
../../guestbin/libreswan-up-down.sh westnet-eastnet-ipv4-psk-ikev2 -I 192.0.1.254 192.0.2.254
: bad
ipsec whack --impair none
ipsec whack --impair revival
ipsec whack --impair timeout_on_retransmit # one packet
ipsec whack --impair add_unknown_v2_payload_to:IKE_SA_INIT
ipsec whack --impair unknown_v2_payload_critical
../../guestbin/libreswan-up-down.sh westnet-eastnet-ipv4-psk-ikev2 -I 192.0.1.254 192.0.2.254
echo done

/testing/guestbin/swan-prep --nokeys
ipsec start
../../guestbin/wait-until-pluto-started
ipsec whack --impair add_unknown_v2_payload_to:IKE_SA_INIT
ipsec auto --add westnet-eastnet-ipv4-psk-ikev2
echo "initdone"

/testing/guestbin/swan-prep
ipsec start
../../guestbin/wait-until-pluto-started
ipsec auto --add west-east
ipsec whack --impair suppress_retransmits
echo "initdone"

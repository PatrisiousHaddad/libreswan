/testing/guestbin/swan-prep
../../guestbin/route.sh del 192.0.1.0/24
ipsec start
../../guestbin/wait-until-pluto-started
ipsec add east
echo "initdone"

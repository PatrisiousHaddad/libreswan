/testing/guestbin/swan-prep --x509
ipsec start
../../guestbin/wait-until-pluto-started
ipsec add north-east
ipsec whack --impair revival
echo initdone

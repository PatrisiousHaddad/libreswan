/testing/guestbin/swan-prep --x509
ipsec certutil -D -n west
ipsec start
../../guestbin/wait-until-pluto-started
ipsec whack --impair suppress_retransmits
ipsec auto --add nss-cert-ocsp
ipsec auto --status |grep nss-cert-ocsp
echo "initdone"

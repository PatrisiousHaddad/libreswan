/testing/guestbin/swan-prep --x509 --x509name key4096
ipsec start
../../guestbin/wait-until-pluto-started
iptables -I INPUT -p udp -m length --length 0x5dc:0xffff -j DROP
ipsec auto --add x509
ipsec whack --impair suppress_retransmits
echo done

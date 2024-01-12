/testing/guestbin/swan-prep
cp policies/* /etc/ipsec.d/policies/
cp ikev2-oe.conf /etc/ipsec.d/ikev2-oe.conf
echo "192.1.2.0/24"  >> /etc/ipsec.d/policies/private-or-clear
ipsec start
../../guestbin/wait-until-pluto-started
ipsec whack --impair suppress_retransmits
ipsec auto --add road-east
ipsec auto --add road-west
# give OE a chance to load conns
../../guestbin/wait-for.sh --match 'loaded 9,' -- ipsec auto --status
echo "initdone"

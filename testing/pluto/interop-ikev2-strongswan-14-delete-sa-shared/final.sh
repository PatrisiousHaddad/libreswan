if [ -f /var/run/pluto/pluto.pid ]; then ../../guestbin/ipsec-kernel-state.sh ; fi
if [ -f /var/run/pluto/pluto.pid ]; then ../../guestbin/ipsec-kernel-policy.sh ; fi
if [ -f /var/run/pluto/pluto.pid ]; then grep "Message ID:" /tmp/pluto.log | sed "s/last_.*$//" ; fi
if [ -f /var/run/charon.pid -o -f /var/run/strongswan/charon.pid ]; then strongswan status ; fi

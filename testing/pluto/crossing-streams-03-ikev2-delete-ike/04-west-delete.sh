# stop all traffic, will be drip feeding

ipsec whack --impair block_inbound
ipsec whack --impair block_outbound

# initiate delete; but block it

ipsec whack --delete-ike --name east-west --asynchronous
../../guestbin/wait-for-pluto.sh 'IMPAIR: blocking outbound message 1'

# now do same on east

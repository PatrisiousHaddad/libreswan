ipsec auto --up west-x509
ipsec whack --impair timeout_on_retransmit
# this one should fail, as east is only expecting selfsigned cert of west, not road
ipsec auto --up road-x509
echo "done"

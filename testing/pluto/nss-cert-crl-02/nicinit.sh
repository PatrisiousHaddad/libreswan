# set up the crl server
cp /testing/x509/crls/cacrlvalid.crl /testing/pluto/nss-cert-crl-02/revoked.crl
cd /testing/pluto/nss-cert-crl-02
/usr/bin/python -m SimpleHTTPServer 80 &
echo "done."

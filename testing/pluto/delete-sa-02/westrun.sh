ipsec auto --up west-east
# enable sending a bogus Notify with the Delete
ipsec whack --debug-all --impair ikev1_del_with_notify
ipsec auto --down west-east

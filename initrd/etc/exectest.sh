# run by /etc/shtest.sh: the exec builtin replaces this shell
echo before exec
exec nosuchprog
exec echo replaced by exec
echo never printed

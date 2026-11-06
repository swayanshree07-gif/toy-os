# tsh script run by the kernel's shell test (make test). Every output line is
# checked by a marker in tests/expected.txt.
echo "script says" hi   # quotes group words, comments are ignored
cd /etc
pwd
ls /etc
cat /etc/motd
cd /nowhere
status
/bin/segv
status
nosuchprog
uptime
echo -n "no newline, "
echo done
exit 5

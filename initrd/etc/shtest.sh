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
# Day 9 session: redirection, file tools, ps/mem/kill
mkdir /work
echo "first line" > /work/f.txt
echo second >>/work/f.txt
cat /work/f.txt
wc < /work/f.txt
wc /work/f.txt > /work/count 2> /work/err
cat /work/count
ls /work
rm /work/f.txt
rm /work
rm /work/count /work/err /work
ls /work
cat < /nowhere
echo >
mkdir /etc
ps
mem
kill 99999
status
exit 5

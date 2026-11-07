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
# Day 10 session: pipelines, seq/grep/head/yes/tee, cp/mv
seq 1 300 | grep 7 | wc
yes tern | head -n 3
mkdir /w2
seq 5 | tee /w2/nums | grep -v 3
cat /w2/nums | wc
cp /w2/nums /w2/copy
mv /w2/copy /w2/moved
ls /w2
mv /w2 /w2/inner
head -n 2 /w2/moved
grep -c 4 < /w2/moved
seq 3 | nosuchprog | wc
status
echo a | cd /
echo a | | wc
echo "a|b" | wc
rm /w2/nums /w2/moved /w2
exit 5

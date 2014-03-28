#
# Send notification about activity above threshold and log it
#
msg=`date +"%a %H:%M"`
wd=`pwd`
/Users/dmitrym/terminal-notifier_1.4.2/terminal-notifier.app/Contents/MacOS/terminal-notifier -message "$msg Activity above threshold" -title Magnetophon -execute "open -R \"$wd/$1 $2\""
date
blinkstick --serial=BS000686-1.1 --set-color red

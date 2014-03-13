#magnetophon

Magnetophon is a command-line Mac OS X audio recorder. It records only when volume is 
above a certain threshold, creating timestamped AIFF files in current directory.

If a file called magnetophon.stats exists in current directory, it is used to store
statistics on audio activity. When activity is higher than usual, magnetophon executes
shell script magnetophon.command in the same folder where magnetophon executable is.
This script can provide notifications, for example, 
using https://github.com/alloy/terminal-notifier 
or https://github.com/arvydas/blinkstick-client.


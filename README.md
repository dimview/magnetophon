#magnetophon

Magnetophon is command-line audio recorder for Mac OS X. Each time audio volume exceeds
predefined RMS threshold, magnetophon creates a new AIFF file in current folder. File name
reflects date and time when recording started, e.g., 2013-12-31 23.59.59.aiff.

This can be useful for time shifting audio. For example, if a VHF radio is connected to
audio input, magnetophon will record all transmissions.

When activity is higher than usual, magnetophon executes shell script magnetophon.command
in the same folder where magnetophon executable is. This script can provide notifications,
for example, using https://github.com/alloy/terminal-notifier or 
https://github.com/arvydas/blinkstick-client.

## Usage

```
$ nohup [[[hours] rms] decay] >magnetophone.log 2>&1 &
```
hours is return period, or average number of hours between notifications, default 168 
(one week).

rms is RMS threshold, default 1000. Audio samples are 16 bit signed integers, so maximum
possible RMS is 23165.

decay is time constant (in seconds) used in exponential smooting, default 600. 

## Notification Algorithm

magnetophon maintains running estimate of file frequency (number of files generated per
hour) using exponential smoothing. To establish historical baseline, mean and standard 
deviation of previously observed file frequency are stored in hourly buckets, separately 
for weekdays and weekends (local time).

magnetophon estimates expected file frequency by interpolating hourly historical baseline.
If file frequency is above the threshold, a notification is triggered. Future
notifications are suppressed until file frequency estimate falls below expected mean plus 
one expected standard deviation. Threshold is determined by return period (desired average 
number of hours between notifications) as well as estimated mean and standard deviation of
file frequency.

If no data is available for either end of interpolation interval, overall mean and
standard deviation are used instead, except during first hour when notifications are
suppressed.

magnetophon maintains two CSV files in current folder. magnetophon.csv contains one line
per audio file with values of variables that went into decision whether to trigger 
notification. This file is also read when magnetophon is started to restore historical
baseline in memory. magnetophon.stats.csv contains hourly historical baseline.
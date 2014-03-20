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
hours is average number of hours between notifications, default 168 (one week).

rms is RMS threshold, default 1000. Audio samples are 16 bit signed integers. 

decay is time constant used in exponential decay, default 600. 

## Notification Algorithm

magnetophon maintains running estimate of duty cycle using exponential smoothing.
Smoothing factor is by default set to 1/600 s<sup>-1</sup>, so it takes approximately
half an hour of uninterrupted audio to bring duty cycle estimate from 0 to 0.95.
If average transmission lengths are much longer or much shorter, time constant
(reciprocal of smoothing factor) can be specified in command line.

Duty cycle estimate is updated once per second. To establish historical baseline, mean
and standard deviation of duty cycle estimate are stored in hourly buckets, separately
for weekdays and weekends (local time). This data is persisted in file magnetophon.stats
in current folder so that it survives magnetophon restart. Duty cycle measurements
are taken at the end of each recording.

magnetophon estimates expected mean and standard deviation of duty cycle by interpolating 
hourly historical baseline. If duty cycle estimate is above the threshold, a notification
is triggered. Future notifications are suppressed until duty cycle estimate falls below 
expected mean plus one expected standard deviation. Threshold is determined by number of 
hours between notifications, estimated expected mean and standard deviation of duty cycle,
and observed frequency of recordings since magnetophon start.

If no data is available for either end of interpolation interval, overall mean and
standard deviation are used instead, except during first hour when notifications are
suppressed.

magnetophon maintains two CSV files in current folder. magnetophon.csv contains one line
per audio file with values of variables that went into decision whether to trigger 
notification. magnetophon.stats.csv contains hourly historical baseline.
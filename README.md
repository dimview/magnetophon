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

magnetophon maintains running estimate of activity: exponentially smoothed square root of 
last audio file duration in seconds.

To establish historical baseline, mean and standard deviation of previously observed 
activity are stored in hourly buckets, separately for weekdays and weekends (local time).
To remove high-frequency noise, before using historical baseline in comparison it is
transformed to frequency domain with discrete Fourier transform, then frequencies
up to and including third harmonic are converted back to time domain. This is done 
separately for mean and standard deviation of activity.

Threshold is determined by return period (desired average number of hours between 
notifications) and expected mean and standard deviation of activity.

If activity is above the threshold, a notification is triggered. Future notifications are
suppressed until activity falls below expected mean plus one expected standard deviation. 

If data is not available for all hourly buckets, overall mean and standard deviation are 
used instead.

magnetophon maintains two CSV files in current folder. magnetophon.csv contains one line
per audio file with values of variables that went into decision whether to trigger 
notification. This file is also read when magnetophon is started to restore historical
baseline in memory. magnetophon.stats.csv contains hourly historical baseline.
#magnetophon

Magnetophon is command-line audio recorder for Mac OS X. Each time audio volume exceeds
predefined threshold, magnetophon creates a new AIFF file in current folder. File name
reflects date and time when recording started, e.g., 2013-12-31 23.59.59.aiff.

This can be useful for time shifting audio. For example, if a VHF radio is connected to
audio input, magnetophon will record all transmissions.

When activity is higher than usual, magnetophon executes shell script magnetophon.command
in the same folder where magnetophon executable is. This script can provide notifications,
for example, using https://github.com/alloy/terminal-notifier or 
https://github.com/arvydas/blinkstick-client.

## Usage

```
$ nohup [[rms] decay] >magnetophone.log 2>&1 &
```

rms is RMS threshold, default 1000. Audio samples are 16 bit signed integers. 

decay is time constant used in exponential decay, default 100. 

## Notification Algorithm

magnetophon maintains running estimate of duty cycle using exponential smoothing.
Smoothing factor is by default set to 1/100 s-1, so it takes approximately 300 seconds
of uninterrupted audio to bring duty cycle estimate from 0 to approximately 0.95.
If average transmission lengths are much longer or much shorter, time constant
(reciprocal of smoothing factor) can be specified in command line.

Duty cycle estimate is updated once per second. To establish historical baseline, mean
and standard deviation of duty cycle estimate are stored in hourly buckets, separately
for weekdays and weekends (local time). This data is persisted in file magnetophon.stats
in current folder so that it survives magnetophon restart.

magnetophon estimates expected mean and standard deviation by interpolating hourly 
historical baseline data. If duty cycle estimate is above expected mean plus two 
expected standard deviations, a notification is triggered. Future notifications are 
suppressed until duty cycle estimate falls below expected mean plus one expected 
standard deviation.


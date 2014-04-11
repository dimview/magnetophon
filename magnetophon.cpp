// magnetophon.cpp
//
// Command-line audio recorder for Mac OS X
//
// Records audio (above some volume threshold) into time-stamped files in current folder.
// Keeps track of historical usage in magnetophon.csv.
// Launches magnetophon.command when usage is unusually high.
//
// Dm. Mayorov
//
#include <CoreAudio/CoreAudioTypes.h>
#include <AudioToolbox/AudioFile.h>
#include <AudioToolbox/AudioQueue.h>
#include <CoreFoundation/CFURL.h>
#include <stdio.h>
#include <time.h>
#include <math.h>

// Approximate inverse cumulative density function of a standard normal random variable
double static standard_normal_inverse_cdf(double p)
{
  if (p <= 0 || p >= 1) return 0;
  
  double t = sqrt(-2 * log((p < 0.5) ? p : 1 - p));
    
  // Abramowitz and Stegun formula 26.2.23
  double rational_approximation = t - ((0.010328 * t + 0.802853) * t + 2.515517) / 
    (((0.001308 * t + 0.189269) * t + 1.432788) * t + 1);
  
  return (p < 0.5) ? -rational_approximation : rational_approximation;
}

// Online calculation of mean and variance 
// See Knuth TAOCP vol 2, 3rd edition, page 232
class RunningStat
{
  public:
    RunningStat() : n_(0) {}
    
    void push(double x)
    {
      if (!n_++) {
        m_ = x;
        s_ = 0;
      } else {
        double new_m = m_ + (x - m_) / n_;
        double new_s = s_ + (x - m_) * (x - new_m);
        m_ = new_m;
        s_ = new_s;
      }
    }

    double mean(void) const
    { 
      return (n_ > 0) ? m_ : 0;
    }

    double variance(void) const
    {
      return ( (n_ > 1) ? s_ / (n_ - 1) : 0 );
    }

    double stdev(void) const
    {
      return sqrt(variance());
    }

    int count(void) const
    {
      return n_;
    }

  private:
    int n_;
    double m_;
    double s_;
};

class BaselineBusinessCurve {
  public:
    RunningStat overall_;
    RunningStat weekday_[24];
    RunningStat weekend_[24];

    RunningStat* push( double x
                     , int tm_wday // days since Sunday (0...6)
                     , int tm_hour // hours since midnight (0...23)
                     ) {
      overall_.push(x);
      RunningStat* rspa = (tm_wday == 0 || tm_wday == 6 ? &weekend_[0] : &weekday_[0]);
      rspa += tm_hour;
      rspa->push(x);
      return rspa;
    }
};

enum MagnitophonState {
  magnitophonWaiting, magnitophonRecording, magnitophonDone 
};

static const int kNumberBuffers = 3;

// Information is passed between main program loop and audio recorder callback via this structure
struct AQRecorderState {
  AudioStreamBasicDescription  mDataFormat; 
  AudioQueueRef                mQueue;
  AudioQueueBufferRef          mBuffers[kNumberBuffers];
  AudioFileID                  mAudioFile;
  UInt32                       bufferByteSize;
  SInt64                       mCurrentPacket;
  int                          mState;
  time_t                       mRecordingStartTime;
  int                          mRecordingLength;
  int                          mRmsThreshold;
};

// Audio recorder callback
static void HandleInputBuffer(
  void                                 *aqData,
  AudioQueueRef                        inAQ,
  AudioQueueBufferRef                  inBuffer,
  const AudioTimeStamp                 *inStartTime,
  UInt32                               inNumPackets,
  const AudioStreamPacketDescription   *inPacketDesc
) {
  AQRecorderState *pAqData = (AQRecorderState *) aqData;
 
  if ( inNumPackets == 0 
    && pAqData->mDataFormat.mBytesPerPacket != 0
     ) {
    inNumPackets = inBuffer->mAudioDataByteSize / pAqData->mDataFormat.mBytesPerPacket;
  }
  
  int min = 0;
  int max = 0;
  RunningStat stat;
  for (int i = 0; i < inBuffer->mAudioDataByteSize; i += sizeof(SInt16)) {
    SInt16 sample = *((SInt16*)((char*)inBuffer->mAudioData + i));
    stat.push(sample);
    if (i == 0) {
      min = max = sample;
    } else {
      if (min > sample) min = sample;
      if (max < sample) max = sample;
    }
  }
  if (inBuffer->mAudioDataByteSize) {
    if ( (pAqData->mState == magnitophonWaiting)
      || (pAqData->mState == magnitophonRecording)
       ) {
      if (stat.stdev() > pAqData->mRmsThreshold) {
        //printf("%d samples, RMS=%g, range=%d...%d, ", stat.count(), stat.stdev(), min, max);
        if (pAqData->mState == magnitophonWaiting) {
          //printf("starting to record\n");
          pAqData->mState = magnitophonRecording;
          time(&pAqData->mRecordingStartTime);
          pAqData->mRecordingLength = 0;
        } else {
          //printf("continuing recording\n");
        }
        if ( AudioFileWritePackets( pAqData->mAudioFile
                                  , false
                                  , inBuffer->mAudioDataByteSize
                                  , inPacketDesc
                                  , pAqData->mCurrentPacket
                                  , &inNumPackets
                                  , inBuffer->mAudioData
                                  ) == noErr) {
          pAqData->mCurrentPacket += inNumPackets;
          pAqData->mRecordingLength += (inBuffer->mAudioDataByteSize / sizeof(SInt16));
        }
      } else if (pAqData->mState == magnitophonRecording) {
        //printf("%d samples, RMS=%g, range=%d...%d, finished recording\n", stat.count(), stat.stdev(), min, max);
        pAqData->mState = magnitophonDone;
        // Convert number of samples to seconds
        pAqData->mRecordingLength /= (int)pAqData->mDataFormat.mSampleRate;
      } else {
        //printf("%d samples, RMS=%g, range=%d...%d, waiting\n", stat.count(), stat.stdev(), min, max);
      }
    } else {
      //printf("%d samples, RMS=%g, range=%d...%d, ignoring tail\n", stat.count(), stat.stdev(), min, max);
    }
  }
  if (pAqData->mState == magnitophonDone) return;
 
  AudioQueueEnqueueBuffer(pAqData->mQueue, inBuffer, 0, NULL);
}

static void DeriveBufferSize (
    AudioQueueRef                audioQueue,
    AudioStreamBasicDescription  &ASBDescription,
    Float64                      seconds,
    UInt32                       *outBufferSize
) {
  static const int maxBufferSize = 0x50000;
 
  int maxPacketSize = ASBDescription.mBytesPerPacket;
  if (maxPacketSize == 0) {
    UInt32 maxVBRPacketSize = sizeof(maxPacketSize);
    AudioQueueGetProperty( audioQueue
                         , kAudioQueueProperty_MaximumOutputPacketSize
                         , &maxPacketSize
                         , &maxVBRPacketSize
                         );
  }
 
  Float64 numBytesForTime = ASBDescription.mSampleRate * maxPacketSize * seconds;
  *outBufferSize = UInt32(numBytesForTime < maxBufferSize ? numBytesForTime : maxBufferSize);
}

static double business_update(double business, int seconds_on, int seconds_off, double decay)
{
  if (seconds_on < 0 || seconds_off < 0) return business;

  double transmissions_per_hour = 3600. / (seconds_on + seconds_off + 1.); // "how often"
  double duty_cycle = (seconds_on + 1.) / (seconds_on + seconds_off + 1.); // "for how long"
  double activity = transmissions_per_hour * duty_cycle;   
    
  // Exponential decay
  double tail_weight = pow(1. - decay, seconds_on + seconds_off);
  return (1 - tail_weight) * activity + tail_weight * business;
}

int main(int argc, char* argv[])
{
  double business = 0; // Activity metric
  int return_period = 24 * 7; // On average, one notification per week
  double decay = 1. / 600; // Exponential decay constant
  int rms_threshold = 1000;
  const char* buffer_filename  = "magnetophon.aif";
  const char* csv_filename  = "magnetophon.csv";
  const char* stats_csv_filename = "magnetophon.stats.csv";
  time_t prev_tm;      time(&prev_tm);
  time_t stats_csv_tm; time(&stats_csv_tm); 
  time_t overall_tm;   time(&overall_tm); 
  bool triggered = false;
  BaselineBusinessCurve stat;
  
  if (argc >= 2) {
    int a = atoi(argv[1]);
    if (a > 0) {
      return_period = a;
    } else {
      fprintf(stderr, "Unexpected hours between notifications: %s\n", argv[1]);
    }
  }
  if (argc >= 3) {
    int a = atoi(argv[2]);
    if (a > 0) {
      rms_threshold = a;
    } else {
      fprintf(stderr, "Unexpected RMS threshold: %s\n", argv[2]);
    }
  }
  if (argc >= 4) {
    int a = atoi(argv[3]);
    if (a > 0) {
      decay = 1. / a;
    } else {
      fprintf(stderr, "Unexpected decay constant: %s\n", argv[3]);
    }
  }
  
  // Read and replay historical data
  {
    FILE* f = fopen(csv_filename, "r");
    if (f) {
      // Skip the header: datetime,seconds_off,seconds_on
      int c;
      do {
        c = fgetc(f);
      } while (c != EOF && c != '\n');
      
      do {
        time_t rawtime;
        time(&rawtime);
        struct tm* t = localtime(&rawtime);
        int seconds_off, seconds_on;
        if (fscanf( f, "%u-%u-%u %u.%u.%u,%u,%u"
                  , &t->tm_year, &t->tm_mon, &t->tm_mday
                  , &t->tm_hour, &t->tm_min, &t->tm_sec
                  , &seconds_off, &seconds_on
                  ) == 8) {
          t->tm_year -= 1900;
          t->tm_mon -= 1;
          mktime(t); // t->tm_wday will be set
          business = business_update(business, seconds_on, seconds_off, decay);
          //printf( "%04d-%02d-%02d %02d.%02d.%02d,%d,%d,%s,%g\n"
          //      , t->tm_year + 1900, t->tm_mon + 1, t->tm_mday
          //      , t->tm_hour, t->tm_min, t->tm_sec
          //      , seconds_on, seconds_off, weekday[t->tm_wday]
          //      , business
          //      );
          stat.push(business, t->tm_wday, t->tm_hour);
        }
        
        // Skip the rest of the line
        do {
          c = fgetc(f);
        } while (c != EOF && c != '\n');
      } while (c != EOF);
      fclose(f);
    } else {
      fprintf(stderr, "Can't open %s\n", csv_filename);
    }
  }

  // Create empty CSV if it does not yet exist
  {
    FILE* f = fopen(csv_filename, "r");
    if (f) {
      fclose(f);
    } else {
      f = fopen(csv_filename, "w");
      if (f) {
        fprintf(f, "datetime,seconds_off,seconds_on,business,interpolated_mean,interpolated_stdev,triggered,a_mean,b_mean,o_mean,threshold\n");
        fclose(f);
      }
    }
  }

  // Main cycle, never ends
  // One audio file is created with each iteration
  for (;;) {
    AQRecorderState aqData;
    aqData.mDataFormat.mFormatID         = kAudioFormatLinearPCM;
    aqData.mDataFormat.mSampleRate       = 44100.0;
    aqData.mDataFormat.mChannelsPerFrame = 1;
    aqData.mDataFormat.mBitsPerChannel   = 16;
    aqData.mDataFormat.mBytesPerPacket   = 
    aqData.mDataFormat.mBytesPerFrame    = aqData.mDataFormat.mChannelsPerFrame * sizeof (SInt16);
    aqData.mDataFormat.mFramesPerPacket  = 1;
    aqData.mRmsThreshold                 = rms_threshold;
    AudioFileTypeID fileType             = kAudioFileAIFFType;
    aqData.mDataFormat.mFormatFlags      = kAudioFormatFlagIsBigEndian 
                                         | kAudioFormatFlagIsSignedInteger
                                         | kAudioFormatFlagIsPacked;

    OSStatus err = 
    AudioQueueNewInput( &aqData.mDataFormat
                      , HandleInputBuffer
                      , &aqData
                      , NULL
                      , kCFRunLoopCommonModes
                      , 0
                      , &aqData.mQueue
                      );
    if (err) {
      fprintf(stderr, "AudioQueueNewInput error %d\n", err);
      return -1;                      
    }

    UInt32 dataFormatSize = sizeof(aqData.mDataFormat);
    err = 
    AudioQueueGetProperty( aqData.mQueue
                         , kAudioQueueProperty_StreamDescription
                         , &aqData.mDataFormat
                         , &dataFormatSize
                         );
    if (err) {
      fprintf(stderr, "AudioQueueGetProperty error %d\n", err);
      return -1;                      
    }

    // Audio files are named according to time the audio started. At this point we don't
    // yet know the time, so we'll give the file a temporary name.
    CFURLRef audioFileURL =
    CFURLCreateFromFileSystemRepresentation( NULL
                                           , (const UInt8 *)buffer_filename
                                           , strlen(buffer_filename)
                                           , false
                                           );
 
    err = AudioFileCreateWithURL( audioFileURL
                                , fileType
                                , &aqData.mDataFormat
                                , kAudioFileFlags_EraseFile
                                , &aqData.mAudioFile
                                );
    if (err) {
      fprintf(stderr, "Can't create file %s: %d\n", buffer_filename, err);
      return -1;                      
    }
                        
    DeriveBufferSize( aqData.mQueue
                    , aqData.mDataFormat
                    , 0.5 // buffer size in seconds
                    , &aqData.bufferByteSize
                    );

    for (int i = 0; i < kNumberBuffers; ++i) {
      AudioQueueAllocateBuffer( aqData.mQueue
                              , aqData.bufferByteSize
                              , &aqData.mBuffers[i]
                              );
      AudioQueueEnqueueBuffer( aqData.mQueue
                             , aqData.mBuffers[i]
                             , 0
                             , NULL
                             );
    }
  
    aqData.mCurrentPacket = 0;
    aqData.mState = magnitophonWaiting; 
    AudioQueueStart(aqData.mQueue, NULL);
  
    do {
      sleep(1);
    } while (aqData.mState != magnitophonDone);

    AudioQueueStop(aqData.mQueue, true);
    AudioQueueDispose(aqData.mQueue, true);
    AudioFileClose(aqData.mAudioFile);

    // Now that we know audio start time, rename audio file to reflect it
    struct tm* tmp = localtime(&aqData.mRecordingStartTime);
    char fileName[80];
    {
      strftime(fileName, sizeof(fileName), "%Y-%m-%d %H.%M.%S", tmp);
      strcat(fileName, ".aiff");
      //printf("Renaming %s to %s\n", buffer_filename, fileName);
      if (rename(buffer_filename, fileName)) {
        fprintf(stderr, "Can't rename %s to %s\n", buffer_filename, fileName);
      } 
      fileName[19] = '\0'; // cut off ".aiff"
    }

    // Collect statistics
    {
      int seconds_of_silence = (int)difftime(aqData.mRecordingStartTime, prev_tm);
      int seconds_of_activity = aqData.mRecordingLength;
      business = business_update(business, seconds_of_silence, seconds_of_activity, decay);
      RunningStat* rspa = stat.push(business, tmp->tm_wday, tmp->tm_hour);
      RunningStat* rspb; // Neighbor bucket for interpolation of thresholds
      double weight_a;
      if (tmp->tm_min >= 30) { // neighbor bucket is the next one
        if (tmp->tm_hour == 23) { // wrap to next day
          // tm_wday is days since Sunday (0...6) so Friday is 5 and Saturday is 6
          rspb = (tmp->tm_wday == 5 || tmp->tm_wday == 6 ? &stat.weekend_[0] : &stat.weekday_[0]);
        } else {
          rspb = rspa + 1;
        }
        weight_a = (90. - tmp->tm_min) / 60;
      } else { // neighbor bucket is the previous one
        if (tmp->tm_hour == 0) { // wrap to previous day
          // tm_wday is days since Sunday (0...6) so Sunday is 0 and Monday is 1
          rspb = (tmp->tm_wday == 0 || tmp->tm_wday == 1 ? &stat.weekend_[23] : &stat.weekday_[23]);
        } else {
          rspb = rspa - 1;
        }
        weight_a = (31. + tmp->tm_min) / 60;
      }
      double weight_b = 1. - weight_a;
      double interpolated_mean, interpolated_stdev;
      if (rspa->count() && rspb->count()) { 
        interpolated_mean = weight_a * rspa->mean() + weight_b * rspb->mean();
        //printf("interpolation: %g*%g+%g*%g=%g\n", weight_a, rspa->mean(), weight_b, rspb->mean(), interpolated_mean);
        interpolated_stdev = weight_a * rspa->stdev() + weight_b * rspb->stdev();
      } else { // No reliable hourly stats yet, use overall as fallback
        if (difftime(aqData.mRecordingStartTime, overall_tm) > 3600) {
          interpolated_mean = stat.overall_.mean();
          interpolated_stdev = stat.overall_.stdev();
        } else { // Less than an hour of overall data
          // Set artificially high expectation to suppress notifications
          interpolated_mean = 1001;
          interpolated_stdev = 1001;
        }
      }
      
      double threshold = 1001;
      if (!triggered) {
        double p = 1. / (stat.overall_.mean() * return_period);
        threshold = interpolated_mean + standard_normal_inverse_cdf(1 - p) * interpolated_stdev;
        //printf( "events_per_hour=%g p=%g 1-p=%g invcdf=%g mean=%g stdev=%g threshold=%g\n"
        //      , stat.overall_.mean(), p, 1-p, standard_normal_inverse_cdf(1 - p)
        //      , interpolated_mean, interpolated_stdev, threshold
        //      );
        if (business > threshold) {
          triggered = true;
          if (system(NULL)) {
            char command[2048];
            // Notification script has the same name as this binary, but with .command suffix
            snprintf(command, sizeof(command), "%s.command %s.aiff", argv[0], fileName);
            //printf("Executing %s\n", command);
            //int ret = 
            system(command);
            //printf("Return code %d\n", ret);
          } else {
            fprintf(stderr, "Can't send notification\n");
          }
        }
      } else {
        if (business < interpolated_mean + interpolated_stdev) {
          triggered = false;
        }
      }

      // Save activity to CSV for future analysis
      {
        FILE* f = fopen(csv_filename, "a");
        if (f) {
          fprintf( f, "%s,%d,%d,%g,%g,%g,%d,%g,%g,%g,%g\n"
                 , fileName, seconds_of_silence, seconds_of_activity
                 , business
                 , interpolated_mean, interpolated_stdev
                 , triggered ? 1 : 0
                 , rspa->mean()
                 , rspb->mean()
                 , stat.overall_.mean()
                 , threshold
                 );
          fclose(f);
        }
      }

      // Once per day, append statistics to a CSV for future analysis
      int day_now = tmp->tm_mday;
      tmp = localtime(&stats_csv_tm);
      int day_then = tmp->tm_mday;      
      if (day_now != day_then) {
        time(&stats_csv_tm); 

        // Create empty CSV if it does not yet exist
        FILE* f = fopen(stats_csv_filename, "r");
        if (f) {
          fclose(f);
        } else {
          f = fopen(stats_csv_filename, "w");
          if (f) {
            fprintf(f, "datetime,hour,weekday_count,weekday_mean,weekday_stdev,weekend_count,weekend_mean,weekend_stdev\n");
            fclose(f);
          }
        }
        f = fopen(stats_csv_filename, "a");
        if (f) {
          for (int h = 0; h < 24; h++) {
            fprintf( f, "%s,%d,%d,%g,%g,%d,%g,%g\n"
                   , fileName
                   , h
                   , stat.weekday_[h].count()
                   , stat.weekday_[h].mean()
                   , stat.weekday_[h].stdev()
                   , stat.weekend_[h].count()
                   , stat.weekend_[h].mean()
                   , stat.weekend_[h].stdev()
                   );
          }
          fclose(f);
        } else {
          fprintf(stderr, "Can't open %s\n", stats_csv_filename);
        }
      }
      
      // End of this audio is beginning of the silence
      time(&prev_tm);
    }
  }
}
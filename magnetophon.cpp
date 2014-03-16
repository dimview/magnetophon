// magnetophon.cpp
//
// Record audio (above some volume threshold) into time-stamped files in current folder.
//
// Optionally collect statistics about historical usage in magnetophon.stats, and
// launch magnetophon.command when usage is unusually high.
//
#include <CoreAudio/CoreAudioTypes.h>
#include <AudioToolbox/AudioFile.h>
#include <AudioToolbox/AudioQueue.h>
#include <CoreFoundation/CFURL.h>
#include <stdio.h>
#include <time.h>
#include <math.h>

// Online calculation of mean and variance 
// See Knuth TAOCP vol 2, 3rd edition, page 232
class RunningStat
{
  public:
    RunningStat() : n_(0) {}
    
    void add_observation(double x)
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

// Break down usage statistics in hourly buckets, separate weekdays and weekends
struct BaselineBusinessCurve {
  RunningStat weekday[24];
  RunningStat weekend[24];
};

static const int kNumberBuffers = 3;

enum MagnitophonState {
  magnitophonWaiting, magnitophonRecording, magnitophonDone 
};

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
    stat.add_observation(sample);
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

int main(int argc, char* argv[])
{
  double business = 0; // Estimate of channel busy cycle, 0 to 1
  double decay = 1. / 100; // Exponential decay constant
  int rms_threshold = 1000;
  const char* stats_filename = "magnetophon.stats";
  const char* buffer_filename  = "magnetophon.aif";
  const char* csv_filename  = "magnetophon.csv";
  const char* stats_csv_filename = "magnetophon.stats.csv";
  time_t prev_tm;
  time(&prev_tm);
  time_t stats_csv_tm; 
  time(&stats_csv_tm); 
  bool triggered = false;
  int files_written_since_last_save = 0;
  
  if (argc >= 2) {
    int decay_constant_denominator = atoi(argv[1]);
    if (decay_constant_denominator > 0) {
      decay = 1. / decay_constant_denominator;
    } else {
      fprintf(stderr, "Unexpected decay constant: %s\n", argv[1]);
    }
  }
  if (argc >= 3) {
    int rms = atoi(argv[2]);
    if (rms > 0) {
      rms_threshold = rms;
    } else {
      fprintf(stderr, "Unexpected RMS threshold: %s\n", argv[2]);
    }
  }
  
  // Read old statistics if available
  struct BaselineBusinessCurve stat;
  {
    FILE* f = fopen(stats_filename, "r");
    if (f) {
      if (fread(&stat, sizeof(BaselineBusinessCurve), 1, f) != 1) {
        fprintf(stderr, "Can't read %s\n", stats_filename);    
      }
      fclose(f);
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
        fprintf(f, "datetime,seconds_off,seconds_on,business,interpolated_mean,interpolated_stdev,triggered\n");
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
      // Determine the hourly bucket statistics should go to
      // tm_wday is days since Sunday (0...6)
      RunningStat* rspa = (tmp->tm_wday == 0 || tmp->tm_wday == 6 ? &stat.weekend[0] : &stat.weekday[0]);
      // tm_hour is hours since midnight (0...23)
      rspa += tmp->tm_hour;

      int seconds_of_silence = (int)difftime(aqData.mRecordingStartTime, prev_tm);
      int seconds_of_activity = aqData.mRecordingLength;

      // Apply exponential smoothing to business 
      printf("%s: %g, off %d, ", fileName, business, seconds_of_silence);
      for (int i = 0; i < seconds_of_silence; i++) {
        business -= business * decay;
        rspa->add_observation(business);
      }
      printf("%g, on %d, ", business, seconds_of_activity);
      for (int i = 0; i < seconds_of_activity; i++) {
        business += (1 - business) * decay;
        rspa->add_observation(business);
      }
      printf("%g\n", business);
      
      RunningStat* rspb; // Neighbor bucket for interpolation of thresholds
      double weight_a;
      if (tmp->tm_min >= 30) { // neighbor bucket is the next one
        if (tmp->tm_hour == 23) { // wrap to next day
          // tm_wday is days since Sunday (0...6) so Friday is 5 and Saturday is 6
          rspb = (tmp->tm_wday == 5 || tmp->tm_wday == 6 ? &stat.weekend[0] : &stat.weekday[0]);
        } else {
          rspb = rspa + 1;
        }
        weight_a = (90. - tmp->tm_min) / 60;
      } else { // neighbor bucket is the previous one
        if (tmp->tm_hour == 0) { // wrap to previous day
          // tm_wday is days since Sunday (0...6) so Sunday is 0 and Monday is 1
          rspb = (tmp->tm_wday == 0 || tmp->tm_wday == 1 ? &stat.weekend[23] : &stat.weekday[23]);
        } else {
          rspb = rspa - 1;
        }
        weight_a = (31. + tmp->tm_min) / 60;
      }
      double weight_b = 1. - weight_a;
      double interpolated_mean = weight_a * rspa->mean() + weight_b * rspb->mean();
      //printf("interpolation: %g*%g+%g*%g=%g\n", weight_a, rspa->mean(), weight_b, rspb->mean(), interpolated_mean);
      double interpolated_stdev = weight_a * rspa->stdev() + weight_b * rspb->stdev();
      
      //printf( "thresholds: %g+%g=%g, %g+2*%g=%g\n"
      //      , interpolated_mean, interpolated_stdev, interpolated_mean + interpolated_stdev
      //      , interpolated_mean, interpolated_stdev, interpolated_mean + 2 * interpolated_stdev
      //      );
    
      // business is at the highest, compare to thresholds
      if (!triggered) {
        if (business > interpolated_mean + 2 * interpolated_stdev) {
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
          fprintf( f, "%s,%d,%d,%g,%g,%g,%d\n"
                 , fileName, seconds_of_silence, seconds_of_activity
                 , business
                 , interpolated_mean, interpolated_stdev
                 , triggered ? 1 : 0
                 );
          fclose(f);
        }
      }

      // From time to time, save statistics to a file
      if (++files_written_since_last_save > 10) {
        files_written_since_last_save = 0;
        FILE* f = fopen(stats_filename, "w");
        if (f) {
          if (fwrite(&stat, sizeof(BaselineBusinessCurve), 1, f) == 1) {
            //printf("Saved stats in %s\n", stats_filename);
          } else {
            fprintf(stderr, "Can't write to %s\n", stats_filename);
          }
          fclose(f);
        } else {
          fprintf(stderr, "Can't open %s\n", stats_filename);
        }
      }

      // Once per day, append statistics to a CSV for future analysis      
      if (localtime(&stats_csv_tm)->tm_mday != tmp->tm_mday) {
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
                   , stat.weekday[h].count()
                   , stat.weekday[h].mean()
                   , stat.weekday[h].stdev()
                   , stat.weekend[h].count()
                   , stat.weekend[h].mean()
                   , stat.weekend[h].stdev()
                   );
          }
        }
      }
      
      // End of this audio is beginning of the silence
      time(&prev_tm);
    }
  }
}
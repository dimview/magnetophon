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
  
  double sum = 0;
  int count = 0;
  int min = 0;
  int max = 0;
  for (int i = 0; i < inBuffer->mAudioDataByteSize; i += sizeof(SInt16)) {
    SInt16 sample = *((SInt16*)((char*)inBuffer->mAudioData + i));
    sum += sample * sample;
    count++;
    if (i == 0) {
      min = max = sample;
    } else {
      if (min > sample) min = sample;
      if (max < sample) max = sample;
    }
  }
  if (inBuffer->mAudioDataByteSize) {
    double rms = sqrt(sum / count);
    if ( (pAqData->mState == magnitophonWaiting)
      || (pAqData->mState == magnitophonRecording)
       ) {
      if (rms > 1000) {
        //printf("%d samples, RMS=%g, range=%d...%d, ", count, rms, min, max);
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
        //printf("%d samples, RMS=%g, range=%d...%d, finished recording\n", count, rms, min, max);
        pAqData->mState = magnitophonDone;
        // Convert number of samples to seconds
        pAqData->mRecordingLength /= (int)pAqData->mDataFormat.mSampleRate;
      } else {
        //printf("%d samples, RMS=%g, range=%d...%d, waiting\n", count, rms, min, max);
      }
    } else {
      //printf("%d samples, RMS=%g, range=%d...%d, ignoring tail\n", count, rms, min, max);
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
  double decay = 1. / 600; // Exponential decay constant
  struct BaselineBusinessCurve stat;
  time_t prev_tm;
  time(&prev_tm);
  bool triggered = false;
  int files_written_since_last_save = 0;
  const char* stats_filename = "magnetophon.stats";
  const char* buffer_filename  = "magnetophon.aif";
  bool collect_statistics = false;
  
  // Read old statistics if available
  {
    FILE* f = fopen(stats_filename, "r");
    if (f) {
      collect_statistics = true;
      if (fread(&stat, sizeof(BaselineBusinessCurve), 1, f) == 1) {
        printf("hour,weekday_mean,weekday_stdev,weekend_mean,weekend_stdev\n");
        for (int h = 0; h < 24; h++) {
          printf( "%d,%g,%g,%g,%g\n"
                , h
                , stat.weekday[h].mean()
                , stat.weekday[h].stdev()
                , stat.weekend[h].mean()
                , stat.weekend[h].stdev()
                );
        }     
      } else {
        fprintf(stderr, "Can't read %s\n", stats_filename);    
      }
      fclose(f);
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
    }

    if (collect_statistics) {
      // Determine the hourly bucket statistics should go to
      // tm_wday is days since Sunday (0...6)
      RunningStat* rspa = (tmp->tm_wday == 0 || tmp->tm_wday == 6 ? &stat.weekend[0] : &stat.weekday[0]);
      // tm_hour is hours since midnight (0...23)
      rspa += tmp->tm_hour;

      // Apply exponential smoothing to business 
      int seconds_of_silence = (int)difftime(aqData.mRecordingStartTime, prev_tm);
      printf("business: %g, off %d, ", business, seconds_of_silence);
      while (seconds_of_silence --> 0) {
        business -= business * decay;
        rspa->add_observation(business);
      }
      int seconds_of_activity = aqData.mRecordingLength;
      printf("%g, on %d, ", business, seconds_of_activity);
      while (seconds_of_activity --> 0) {
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
      printf("interpolation: %g*%g+%g*%g=%g\n", weight_a, rspa->mean(), weight_b, rspb->mean(), interpolated_mean);
      double interpolated_stdev = weight_a * rspa->stdev() + weight_b * rspb->stdev();
      
      printf( "thresholds: %g+%g=%g, %g+2*%g=%g\n"
            , interpolated_mean, interpolated_stdev, interpolated_mean + interpolated_stdev
            , interpolated_mean, interpolated_stdev, interpolated_mean + 2 * interpolated_stdev
            );
    
      // business is at the highest, compare to thresholds
      if (!triggered) {
        if (business > interpolated_mean + 2 * interpolated_stdev) {
          triggered = true;
          if (system(NULL)) {
            char command[2048];
            // Notification script has the same name as this binary, but with .command suffix
            snprintf(command, sizeof(command), "%s.command %s", argv[0], fileName);
            printf("Executing %s\n", command);
            int ret = system(command);
            printf("Return code %d\n", ret);
          } else {
            printf("Can't send notification\n");
          }
        }
      } else {
        if (business < interpolated_mean + interpolated_stdev) {
          triggered = false;
        }
      }
      
      // From time to time, save statistics to a file
      if (++files_written_since_last_save > 10) {
        files_written_since_last_save = 0;
        FILE* f = fopen(stats_filename, "w");
        if (f) {
          if (fwrite(&stat, sizeof(BaselineBusinessCurve), 1, f) == 1) {
            printf("Saved stats in %s\n", stats_filename);
          } else {
            fprintf(stderr, "Can't write to %s\n", stats_filename);
          }
          fclose(f);
        } else {
          fprintf(stderr, "Can't open %s\n", stats_filename);
        }
      }
      
      // End of this audio is beginning of the silence
      time(&prev_tm);
    }
  }
}
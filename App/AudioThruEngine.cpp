#include "AudioThruEngine.h"
#include "AudioRingBuffer.h"
#include <unistd.h>

#define USE_AUDIODEVICEREAD 0
#if USE_AUDIODEVICEREAD
AudioBufferList *gInputIOBuffer = NULL;
#endif

#define kSecondsInRingBuffer 2.

AudioThruEngine::AudioThruEngine() :
  mWorkBuf(NULL),
  mRunning(false),
  mMuting(false),
  mThruing(true),
  mBufferSize(512),
  mExtraLatencyFrames(0),
  mInputLoad(0.),
  mOutputLoad(0.)
{
  mInputBuffer = new AudioRingBuffer(4, 88200);

  // init routing map to default chan->chan
  for (int i = 0; i < 64; i++)
    mChannelMap[i] = i;
}

AudioThruEngine::~AudioThruEngine()
{
  SetDevices(kAudioDeviceUnknown, kAudioDeviceUnknown);
  delete mInputBuffer;
}

void  AudioThruEngine::SetDevices(AudioDeviceID input, AudioDeviceID output)
{
  Stop();

  if (input != kAudioDeviceUnknown)
    mInputDevice.Init(input, true);
  if (output != kAudioDeviceUnknown)
    mOutputDevice.Init(output, false);
}


void  AudioThruEngine::SetInputDevice(AudioDeviceID input)
{
  Stop();
  mInputDevice.Init(input, true);
  SetBufferSize(mBufferSize);
  mInputBuffer->Clear();
  Start();
}

void  AudioThruEngine::SetOutputDevice(AudioDeviceID output)
{
  Stop();
  mOutputDevice.Init(output, false);
  SetBufferSize(mBufferSize);
  Start();
}

void  AudioThruEngine::SetBufferSize(UInt32 size)
{
  bool wasRunning = Stop();
  mBufferSize = size;
  mInputDevice.SetBufferSize(size);
  mOutputDevice.SetBufferSize(size);
  if (wasRunning) Start();
}

void  AudioThruEngine::SetExtraLatency(SInt32 frames)
{
  mExtraLatencyFrames = frames;
  if (mRunning)
    ComputeThruOffset();
}

// change sample rate of one device to match another - returns if successful or not
OSStatus AudioThruEngine::MatchSampleRate(bool useInputDevice)
{
  OSStatus status = kAudioHardwareNoError;

  mInputDevice.UpdateFormat();
  mOutputDevice.UpdateFormat();

  if (mInputDevice.mFormat.mSampleRate != mOutputDevice.mFormat.mSampleRate)
  {
    if (useInputDevice)
      status = mOutputDevice.SetSampleRate(mInputDevice.mFormat.mSampleRate);
    else
      status = mInputDevice.SetSampleRate(mOutputDevice.mFormat.mSampleRate);

    printf("reset sample rate\n");
  }

  return status;
}

void  AudioThruEngine::Start()
{
  if (mRunning) return;
  if (!mInputDevice.Valid() || !mOutputDevice.Valid()) {
    return;
  }

  if (mInputDevice.mFormat.mSampleRate != mOutputDevice.mFormat.mSampleRate) {
    if (MatchSampleRate(false)) {
      printf("Error - sample rate mismatch: %f / %f\n", mInputDevice.mFormat.mSampleRate, mOutputDevice.mFormat.mSampleRate);
      return;
    }
  }

  mInputBuffer->Allocate(mInputDevice.mFormat.mBytesPerFrame, UInt32(kSecondsInRingBuffer * mInputDevice.mFormat.mSampleRate));
  mSampleRate = mInputDevice.mFormat.mSampleRate;

  mWorkBuf = new Byte[mInputDevice.mBufferSizeFrames * mInputDevice.mFormat.mBytesPerFrame];
  memset(mWorkBuf, 0, mInputDevice.mBufferSizeFrames * mInputDevice.mFormat.mBytesPerFrame);

  mRunning = true;

#if USE_AUDIODEVICEREAD
  UInt32 streamListSize;
  verify_noerr (AudioDeviceGetPropertyInfo(gInputDevice, 0, true, kAudioDevicePropertyStreams, &streamListSize, NULL));
  UInt32 nInputStreams = streamListSize / sizeof(AudioStreamID);

  propsize = offsetof(AudioBufferList, mBuffers[nInputStreams]);
  gInputIOBuffer = (AudioBufferList *)malloc(propsize);
  verify_noerr (AudioDeviceGetProperty(gInputDevice, 0, true, kAudioDevicePropertyStreamConfiguration, &propsize, gInputIOBuffer));
  gInputIOBuffer->mBuffers[0].mData = malloc(gInputIOBuffer->mBuffers[0].mDataByteSize);

  verify_noerr (AudioDeviceSetProperty(gInputDevice, NULL, 0, true, kAudioDevicePropertyRegisterBufferList, propsize, gInputIOBuffer));
#endif

  mInputProcState = kStarting;
  mOutputProcState = kStarting;

  verify_noerr (AudioDeviceAddIOProc(mInputDevice.mID, InputIOProc, this));
  verify_noerr (AudioDeviceStart(mInputDevice.mID, InputIOProc));

  mOutputIOProc = OutputIOProc;

  verify_noerr (AudioDeviceAddIOProc(mOutputDevice.mID, mOutputIOProc, this));
  verify_noerr (AudioDeviceStart(mOutputDevice.mID, mOutputIOProc));

  while (mInputProcState != kRunning || mOutputProcState != kRunning)
    usleep(1000);

  ComputeThruOffset();
}

void  AudioThruEngine::ComputeThruOffset()
{
  if (!mRunning) {
    mActualThruLatency = 0;
    mInToOutSampleOffset = 0;
    return;
  }

  mActualThruLatency = SInt32(mInputDevice.mSafetyOffset + /*2 * */ mInputDevice.mBufferSizeFrames +
            mOutputDevice.mSafetyOffset + mOutputDevice.mBufferSizeFrames) + mExtraLatencyFrames;
  mInToOutSampleOffset = mActualThruLatency + mIODeltaSampleCount;
}

bool  AudioThruEngine::Stop()
{
  if (!mRunning) return false;
  mRunning = false;

  mInputProcState = kStopRequested;
  mOutputProcState = kStopRequested;

  while (mInputProcState != kOff || mOutputProcState != kOff)
    usleep(5000);

  AudioDeviceRemoveIOProc(mInputDevice.mID, InputIOProc);
  AudioDeviceRemoveIOProc(mOutputDevice.mID, mOutputIOProc);

  if (mWorkBuf) {
    delete[] mWorkBuf;
    mWorkBuf = NULL;
  }

  return true;
}



// Input IO Proc
// Receiving input for 1 buffer + safety offset into the past
OSStatus AudioThruEngine::InputIOProc (  AudioDeviceID      inDevice,
                    const AudioTimeStamp*  inNow,
                    const AudioBufferList*  inInputData,
                    const AudioTimeStamp*  inInputTime,
                    AudioBufferList*    outOutputData,
                    const AudioTimeStamp*  inOutputTime,
                    void*          inClientData)
{
  AudioThruEngine *This = (AudioThruEngine *)inClientData;

  switch (This->mInputProcState) {
  case kStarting:
    This->mInputProcState = kRunning;
    break;
  case kStopRequested:
    AudioDeviceStop(inDevice, InputIOProc);
    This->mInputProcState = kOff;
    return noErr;
  default:
    break;
  }

  This->mLastInputSampleCount = inInputTime->mSampleTime;
  This->mInputBuffer->Store((const Byte *)inInputData->mBuffers[0].mData,
                This->mInputDevice.mBufferSizeFrames,
                UInt64(inInputTime->mSampleTime));

//  This->ApplyLoad(This->mInputLoad);
  return noErr;
}

// Output IO Proc
// Rendering output for 1 buffer + safety offset into the future
OSStatus AudioThruEngine::OutputIOProc (  AudioDeviceID      inDevice,
                      const AudioTimeStamp*  inNow,
                      const AudioBufferList*  inInputData,
                      const AudioTimeStamp*  inInputTime,
                      AudioBufferList*    outOutputData,
                      const AudioTimeStamp*  inOutputTime,
                      void*          inClientData)
{
  AudioThruEngine *This = (AudioThruEngine *)inClientData;

  switch (This->mOutputProcState) {
  case kStarting:
    if (This->mInputProcState == kRunning) {
      This->mOutputProcState = kRunning;
      This->mIODeltaSampleCount = inOutputTime->mSampleTime - This->mLastInputSampleCount;
    }
    return noErr;
  case kStopRequested:
    AudioDeviceStop(inDevice, This->mOutputIOProc);
    This->mOutputProcState = kOff;
    return noErr;
  default:
    break;
  }

  if (!This->mMuting && This->mThruing) {
    //double delta = This->mInputBuffer->Fetch((Byte *)outOutputData->mBuffers[0].mData,
    //            This->mOutputDevice.mBufferSizeFrames,
    //            UInt64(inOutputTime->mSampleTime - This->mInToOutSampleOffset));
    double delta = This->mInputBuffer->Fetch(This->mWorkBuf,
            This->mInputDevice.mBufferSizeFrames,UInt64(inOutputTime->mSampleTime - This->mInToOutSampleOffset));


    // not the most efficient, but this should handle devices with multiple streams [i think]
    // with identitical formats [we know soundflower input channels are always one stream]
    UInt32 innchnls = This->mInputDevice.mFormat.mChannelsPerFrame;

    // iSchemy's edit
    //
    // this solution will probably be a little bit less efficient
    // but I wanted to retain the functionality of previous solution
    // and only add new function
    // Activity Monitor says it's not bad. 14.8MB and 3% CPU for me
    // is IMHO insignificant
    UInt32* chanstart = new UInt32[64];

    for (UInt32 buf = 0; buf < outOutputData->mNumberBuffers; buf++)
    {
      for (int i = 0; i < 64; i++)
        chanstart[i] = 0;
      UInt32 outnchnls = outOutputData->mBuffers[buf].mNumberChannels;
      for (UInt32 chan = 0; chan <
          ((This->CloneChannels() && innchnls==2) ? outnchnls : innchnls);
          chan++)
      {
        UInt32 outChan = This->GetChannelMap(chan) - chanstart[chan];
        if (outChan >= 0 && outChan < outnchnls)
        {
          // odd-even
          float *in = (float *)This->mWorkBuf + (chan % innchnls);
          float *out = (float *)outOutputData->mBuffers[buf].mData + outChan;
          long framesize = outnchnls * sizeof(float);

          for (UInt32 frame = 0; frame < outOutputData->mBuffers[buf].mDataByteSize; frame += framesize )
          {
            *out += *in;
            in += innchnls;
            out += outnchnls;
          }
        }
        chanstart[chan] += outnchnls;
      }
    }

    delete [] chanstart;

    //
    // end

    This->mThruTime = delta;

    //This->ApplyLoad(This->mOutputLoad);

#if USE_AUDIODEVICEREAD
    AudioTimeStamp readTime;

    readTime.mFlags = kAudioTimeStampSampleTimeValid;
    readTime.mSampleTime = inNow->mSampleTime - gInputSafetyOffset - gOutputSampleCount;

    verify_noerr(AudioDeviceRead(gInputDevice.mID, &readTime, gInputIOBuffer));
    memcpy(outOutputData->mBuffers[0].mData, gInputIOBuffer->mBuffers[0].mData, outOutputData->mBuffers[0].mDataByteSize);
#endif
  } else
    This->mThruTime = 0.;

  return noErr;
}

UInt32 AudioThruEngine::GetOutputNchnls()
{
  if (mOutputDevice.mID != kAudioDeviceUnknown)
    return mOutputDevice.CountChannels();//mFormat.mChannelsPerFrame;

  return 0;
}

#if 0
void  AudioThruEngine::ApplyLoad(double load)
{
  double loadNanos = (load * mBufferSize / mSampleRate) /* seconds */ * 1000000000.;

  UInt64 now = AudioConvertHostTimeToNanos(AudioGetCurrentHostTime());
  UInt64 waitUntil = UInt64(now + loadNanos);

  while (now < waitUntil) {
    now = AudioConvertHostTimeToNanos(AudioGetCurrentHostTime());
  }
}
#endif

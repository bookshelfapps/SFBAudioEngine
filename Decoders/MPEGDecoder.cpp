/*
 *  Copyright (C) 2006, 2007, 2008, 2009 Stephen F. Booth <me@sbooth.org>
 *  All Rights Reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are
 *  met:
 *
 *    - Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *    - Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *    - Neither the name of Stephen F. Booth nor the names of its 
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *  HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <algorithm>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "AudioEngineDefines.h"
#include "MPEGDecoder.h"

#define INPUT_BUFFER_SIZE	(5 * 8192)
#define LAME_HEADER_SIZE	((8 * 5) + 4 + 4 + 8 + 32 + 16 + 16 + 4 + 4 + 8 + 12 + 12 + 8 + 8 + 2 + 3 + 11 + 32 + 32 + 32)

#define BIT_RESOLUTION		24

// From vbrheadersdk:
// ========================================
// A Xing header may be present in the ancillary
// data field of the first frame of an mp3 bitstream
// The Xing header (optionally) contains
//      frames      total number of audio frames in the bitstream
//      bytes       total number of bytes in the bitstream
//      toc         table of contents

// toc (table of contents) gives seek points
// for random access
// the ith entry determines the seek point for
// i-percent duration
// seek point in bytes = (toc[i]/256.0) * total_bitstream_bytes
// e.g. half duration seek point = (toc[50]/256.0) * total_bitstream_bytes

#define FRAMES_FLAG     0x0001
#define BYTES_FLAG      0x0002
#define TOC_FLAG        0x0004
#define VBR_SCALE_FLAG  0x0008

// Clipping and rounding code from madplay(audio.c):
/*
 * madplay - MPEG audio decoder and player
 * Copyright (C) 2000-2004 Robert Leslie
 */
static int32_t 
audio_linear_round(unsigned int bits, 
				   mad_fixed_t sample)
{
	enum {
		MIN = -MAD_F_ONE,
		MAX =  MAD_F_ONE - 1
	};
	
	/* round */
	sample += (1L << (MAD_F_FRACBITS - bits));
	
	/* clip */
	if(MAX < sample)
		sample = MAX;
	else if(MIN > sample)
		sample = MIN;
	
	/* quantize and scale */
	return sample >> (MAD_F_FRACBITS + 1 - bits);
}
// End madplay code


#pragma mark Static Methods


bool MPEGDecoder::HandlesFilesWithExtension(CFStringRef extension)
{
	assert(NULL != extension);
	
	if(kCFCompareEqualTo == CFStringCompare(extension, CFSTR("mp3"), kCFCompareCaseInsensitive))
		return true;
	
	return false;
}

bool MPEGDecoder::HandlesMIMEType(CFStringRef mimeType)
{
	assert(NULL != mimeType);	
	
	if(kCFCompareEqualTo == CFStringCompare(mimeType, CFSTR("audio/mpeg-layer-iii"), kCFCompareCaseInsensitive))
		return true;
	
	return false;
}


#pragma mark Creation and Destruction


MPEGDecoder::MPEGDecoder(CFURLRef url)
	: AudioDecoder(url)
{
	assert(NULL != url);
	
	mInputBuffer = static_cast<unsigned char *>(calloc(INPUT_BUFFER_SIZE + MAD_BUFFER_GUARD, sizeof(unsigned char)));
	
	UInt8 buf [PATH_MAX];
	if(FALSE == CFURLGetFileSystemRepresentation(mURL, FALSE, buf, PATH_MAX))
		ERR("CFURLGetFileSystemRepresentation failed");

	mFile = fopen(reinterpret_cast<const char *>(buf), "r");
	if(NULL == mFile) {
		ERR("Unable to open file for reading");
		return;
	}
		
	mad_stream_init(&mStream);
	mad_frame_init(&mFrame);
	mad_synth_init(&mSynth);
	
	// Scan file to determine sample rate, channels, total frames, etc
	if(false == this->ScanFile()) {
		ERR("Unable to scan file");
		return;
	}
	
	// The source's PCM format
	mSourceFormat.mFormatID				= kAudioFormatLinearPCM;
	mSourceFormat.mFormatFlags			= kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked | kAudioFormatFlagsNativeEndian;
	
	mSourceFormat.mSampleRate			= mFormat.mSampleRate;
	mSourceFormat.mChannelsPerFrame		= mFormat.mChannelsPerFrame;
	mSourceFormat.mBitsPerChannel		= 16;
	
	mSourceFormat.mBytesPerPacket		= ((mSourceFormat.mBitsPerChannel + 7) / 8) * mSourceFormat.mChannelsPerFrame;
	mSourceFormat.mFramesPerPacket		= 1;
	mSourceFormat.mBytesPerFrame		= mSourceFormat.mBytesPerPacket * mSourceFormat.mFramesPerPacket;		
	
	// Allocate the buffer list
	mBufferList = static_cast<AudioBufferList *>(calloc(sizeof(AudioBufferList) + (sizeof(AudioBuffer) * (mFormat.mChannelsPerFrame - 1)), 1));
	
	mBufferList->mNumberBuffers = mFormat.mChannelsPerFrame;
	
	for(UInt32 i = 0; i < mBufferList->mNumberBuffers; ++i) {
		mBufferList->mBuffers[i].mData = calloc(mSamplesPerMPEGFrame, sizeof(float));
		mBufferList->mBuffers[i].mNumberChannels = 1;
	}		
}

MPEGDecoder::~MPEGDecoder()
{
	mad_synth_finish(&mSynth);
	mad_frame_finish(&mFrame);
	mad_stream_finish(&mStream);
	
	free(mInputBuffer), mInputBuffer = NULL;
	fclose(mFile), mFile = NULL;
	
	if(mBufferList) {
		for(UInt32 i = 0; i < mBufferList->mNumberBuffers; ++i)
			free(mBufferList->mBuffers[i].mData), mBufferList->mBuffers[i].mData = NULL;	
		free(mBufferList), mBufferList = NULL;
	}
}

#pragma mark Functionality

SInt64 MPEGDecoder::SeekToFrame(SInt64 frame)
{
	if(mFoundLAMEHeader)
		return this->SeekToFrameAccurately(frame);
	else
		return this->SeekToFrameApproximately(frame);
}

UInt32 MPEGDecoder::ReadAudio(AudioBufferList *bufferList, UInt32 frameCount)
{
	assert(NULL != bufferList);
	assert(bufferList->mNumberBuffers == mFormat.mChannelsPerFrame);
	assert(0 < frameCount);
	
	UInt32			bytesToRead;
	UInt32			bytesRemaining;
	unsigned char	*readStartPointer;
	int32_t			audioSample;
	
	bool			readEOF					= false;
	float			scaleFactor				= (1L << (BIT_RESOLUTION - 1));
	
	UInt32			framesRead				= 0;

	// Reset output buffer data size
	for(UInt32 i = 0; i < bufferList->mNumberBuffers; ++i)
		bufferList->mBuffers[i].mDataByteSize = 0;
	
	for(;;) {
		
		UInt32	framesRemaining	= frameCount - framesRead;
		UInt32	framesToSkip	= bufferList->mBuffers[0].mDataByteSize / sizeof(float);
		UInt32	framesInBuffer	= mBufferList->mBuffers[0].mDataByteSize / sizeof(float);
		UInt32	framesToCopy	= std::min(framesInBuffer, framesRemaining);
		
		// Copy data from the buffer to output
		for(UInt32 i = 0; i < mBufferList->mNumberBuffers; ++i) {
			float *floatBuffer = static_cast<float *>(bufferList->mBuffers[i].mData);
			memcpy(floatBuffer + framesToSkip, mBufferList->mBuffers[i].mData, framesToCopy * sizeof(float));
			bufferList->mBuffers[i].mDataByteSize += (framesToCopy * sizeof(float));
			
			// Move remaining data in buffer to beginning
			if(framesToCopy != framesInBuffer) {
				floatBuffer = static_cast<float *>(mBufferList->mBuffers[i].mData);
				memmove(floatBuffer, floatBuffer + framesToCopy, (framesInBuffer - framesToCopy) * sizeof(float));
			}
			
			mBufferList->mBuffers[i].mDataByteSize -= (framesToCopy * sizeof(float));
		}
		
		framesRead += framesToCopy;

		// All requested frames were read
		if(framesRead == frameCount)
			break;
		
		// If the file contains a Xing header but not LAME gapless information,
		// decode the number of MPEG frames specified by the Xing header
		if(true == mFoundXingHeader && false == mFoundLAMEHeader && 1 + mMPEGFramesDecoded == mTotalMPEGFrames)
			break;
		
		// The LAME header indicates how many samples are in the file
		if(true == mFoundLAMEHeader && this->GetTotalFrames() == mSamplesDecoded)
			break;
		
		// Feed the input buffer if necessary
		if(NULL == mStream.buffer || MAD_ERROR_BUFLEN == mStream.error) {
			if(NULL != mStream.next_frame) {
				bytesRemaining = mStream.bufend - mStream.next_frame;
				memmove(mInputBuffer, mStream.next_frame, bytesRemaining);
				
				readStartPointer	= mInputBuffer + bytesRemaining;
				bytesToRead			= INPUT_BUFFER_SIZE - bytesRemaining;
			}
			else {
				bytesToRead			= INPUT_BUFFER_SIZE,
				readStartPointer	= mInputBuffer,
				bytesRemaining		= 0;
			}
			
			// Read raw bytes from the MP3 file
			size_t bytesRead = fread(readStartPointer, 1, bytesToRead, mFile);
			if(ferror(mFile)) {
#if DEBUG
				LOG("Read error: %s.", strerror(errno));
#endif
				break;
			}
			
			// MAD_BUFFER_GUARD zeroes are required to decode the last frame of the file
			if(feof(mFile)) {
				memset(readStartPointer + bytesRead, 0, MAD_BUFFER_GUARD);
				bytesRead	+= MAD_BUFFER_GUARD;
				readEOF		= true;
			}
			
			mad_stream_buffer(&mStream, mInputBuffer, bytesRead + bytesRemaining);
			mStream.error = MAD_ERROR_NONE;
		}
		
		// Decode the MPEG frame
		int result = mad_frame_decode(&mFrame, &mStream);
		if(-1 == result) {
			if(MAD_RECOVERABLE(mStream.error)) {
				// Prevent ID3 tags from reporting recoverable frame errors
				const uint8_t	*buffer			= mStream.this_frame;
				unsigned		buflen			= mStream.bufend - mStream.this_frame;
				uint32_t		id3_length		= 0;
				
				if(10 <= buflen && 0x49 == buffer[0] && 0x44 == buffer[1] && 0x33 == buffer[2]) {
					id3_length = (((buffer[6] & 0x7F) << (3 * 7)) | ((buffer[7] & 0x7F) << (2 * 7)) |
								  ((buffer[8] & 0x7F) << (1 * 7)) | ((buffer[9] & 0x7F) << (0 * 7)));
					
					// Add 10 bytes for ID3 header
					id3_length += 10;
					
					mad_stream_skip(&mStream, id3_length);
				}
#if DEBUG
				else
					LOG("Recoverable frame level error (%s)", mad_stream_errorstr(&mStream));
#endif
				
				continue;
			}
			// EOS for non-Xing streams occurs when EOF is reached and no further frames can be decoded
			else if(MAD_ERROR_BUFLEN == mStream.error && readEOF)
				break;
			else if(MAD_ERROR_BUFLEN == mStream.error)
				continue;
			else {
#if DEBUG
				LOG("Unrecoverable frame level error (%s)", mad_stream_errorstr(&mStream));
#endif
				break;
			}
		}
		
		// Housekeeping
		++mMPEGFramesDecoded;
		
		// Synthesize the frame into PCM
		mad_synth_frame(&mSynth, &mFrame);
		
		// Skip any samples that remain from last frame
		// This can happen if the encoder delay is greater than the number of samples in a frame
		unsigned startingSample = mSamplesToSkipInNextFrame;
		
		// Skip the Xing header (it contains empty audio)
		if(mFoundXingHeader && 1 == mMPEGFramesDecoded)
			continue;
		// Adjust the first real audio frame for gapless playback
		else if(mFoundLAMEHeader && 2 == mMPEGFramesDecoded)
			startingSample += mEncoderDelay;

		// The number of samples in this frame
		unsigned sampleCount = mSynth.pcm.length;

		// Skip this entire frame if necessary
		if(startingSample > sampleCount) {
			mSamplesToSkipInNextFrame += startingSample - sampleCount;
			continue;
		}
		else
			mSamplesToSkipInNextFrame = 0;
		
		// If a LAME header was found, the total number of audio frames (AKA samples) 
		// is known.  Ensure only that many are output
		if(mFoundLAMEHeader && this->GetTotalFrames() < mSamplesDecoded + (sampleCount - startingSample))
			sampleCount = this->GetTotalFrames() - mSamplesDecoded;
		
		// Output samples in 32-bit float PCM
		unsigned channel, sample;
		for(channel = 0; channel < MAD_NCHANNELS(&mFrame.header); ++channel) {
			float *floatBuffer = static_cast<float *>(mBufferList->mBuffers[channel].mData);
			
			for(sample = startingSample; sample < sampleCount; ++sample) {
				audioSample = audio_linear_round(BIT_RESOLUTION, mSynth.pcm.samples[channel][sample]);
				*floatBuffer++ = (float)(audioSample / scaleFactor);
			}
			
			mBufferList->mBuffers[channel].mNumberChannels	= 1;
			mBufferList->mBuffers[channel].mDataByteSize	= (sampleCount - startingSample) * sizeof(float);
		}
		
		mSamplesDecoded += (sampleCount - startingSample);
	}
	
	mCurrentFrame += framesRead;
	
	return framesRead;
}

bool MPEGDecoder::ScanFile()
{
	uint32_t			framesDecoded = 0;
	UInt32				bytesToRead, bytesRemaining;
	size_t				bytesRead;
	unsigned char		*readStartPointer;
	bool				readEOF;
	
	struct mad_stream	stream;
	struct mad_frame	frame;
	
	int					result;
	struct stat			stat;
	uint32_t			id3_length		= 0;
	
	// Set up	
	mad_stream_init(&stream);
	mad_frame_init(&frame);
	
	readEOF = false;
	
	result = fstat(fileno(mFile), &stat);
	if(-1 == result)
		return false;
	
	mFileBytes = stat.st_size;
	
	for(;;) {
		if(NULL == stream.buffer || MAD_ERROR_BUFLEN == stream.error) {
			if(stream.next_frame) {
				bytesRemaining = stream.bufend - stream.next_frame;
				memmove(mInputBuffer, stream.next_frame, bytesRemaining);
				
				readStartPointer	= mInputBuffer + bytesRemaining;
				bytesToRead			= INPUT_BUFFER_SIZE - bytesRemaining;
			}
			else {
				bytesToRead			= INPUT_BUFFER_SIZE,
				readStartPointer	= mInputBuffer,
				bytesRemaining		= 0;
			}
			
			// Read raw bytes from the MP3 file
			bytesRead = fread(readStartPointer, 1, bytesToRead, mFile);
			if(ferror(mFile)) {
#if DEBUG
				LOG("Read error: %s.", strerror(errno));
#endif
				break;
			}
			
			// MAD_BUFFER_GUARD zeroes are required to decode the last frame of the file
			if(feof(mFile)) {
				memset(readStartPointer + bytesRead, 0, MAD_BUFFER_GUARD);
				bytesRead	+= MAD_BUFFER_GUARD;
				readEOF		= true;
			}
			
			mad_stream_buffer(&stream, mInputBuffer, bytesRead + bytesRemaining);
			stream.error = MAD_ERROR_NONE;
		}
		
		result = mad_frame_decode(&frame, &stream);
		if(-1 == result) {
			if(MAD_RECOVERABLE(stream.error)) {
				// Prevent ID3 tags from reporting recoverable frame errors
				const uint8_t	*buffer			= stream.this_frame;
				unsigned		buflen			= stream.bufend - stream.this_frame;
				
				if(10 <= buflen && 0x49 == buffer[0] && 0x44 == buffer[1] && 0x33 == buffer[2]) {
					id3_length = (((buffer[6] & 0x7F) << (3 * 7)) | ((buffer[7] & 0x7F) << (2 * 7)) |
								  ((buffer[8] & 0x7F) << (1 * 7)) | ((buffer[9] & 0x7F) << (0 * 7)));
					
					// Add 10 bytes for ID3 header
					id3_length += 10;
					
					mad_stream_skip(&stream, id3_length);
				}
#if DEBUG
				else
					LOG("Recoverable frame level error (%s)", mad_stream_errorstr(&stream));
#endif
				
				continue;
			}
			// EOS for non-Xing streams occurs when EOF is reached and no further frames can be decoded
			else if(MAD_ERROR_BUFLEN == stream.error && readEOF)
				break;
			else if(MAD_ERROR_BUFLEN == stream.error)
				continue;
			else {
#if DEBUG
				LOG("Unrecoverable frame level error (%s)", mad_stream_errorstr(&stream));
#endif
				break;
			}
		}
		
		++framesDecoded;
		
		// Look for a Xing header in the first frame that was successfully decoded
		// Reference http://www.codeproject.com/audio/MPEGAudioInfo.asp
		if(1 == framesDecoded) {
			mFormat.mSampleRate			= frame.header.samplerate;
			mFormat.mChannelsPerFrame	= MAD_NCHANNELS(&frame.header);
			
			// MAD_NCHANNELS always returns 1 or 2
			mChannelLayout.mChannelLayoutTag	= (1 == MAD_NCHANNELS(&frame.header) ? kAudioChannelLayoutTag_Mono : kAudioChannelLayoutTag_Stereo);
			mSamplesPerMPEGFrame				= 32 * MAD_NSBSAMPLES(&frame.header);
			
			unsigned ancillaryBitsRemaining = stream.anc_bitlen;
			if(32 > ancillaryBitsRemaining)
				continue;
			
			uint32_t magic = mad_bit_read(&stream.anc_ptr, 32);
			ancillaryBitsRemaining -= 32;
			
			if('Xing' == magic || 'Info' == magic) {
				if(32 > ancillaryBitsRemaining)
					continue;
				
				uint32_t flags = mad_bit_read(&stream.anc_ptr, 32);
				ancillaryBitsRemaining -= 32;
				
				// 4 byte value containing total frames
				// For LAME-encoded MP3s, the number of MPEG frames in the file is one greater than this frame
				if(FRAMES_FLAG & flags) {
					if(32 > ancillaryBitsRemaining)
						continue;
					
					uint32_t frames = mad_bit_read(&stream.anc_ptr, 32);
					ancillaryBitsRemaining -= 32;
					
					mTotalMPEGFrames = frames;
					
					// Determine number of samples, discounting encoder delay and padding
					// Our concept of a frame is the same as CoreAudio's- one sample across all channels
					mTotalFrames = frames * mSamplesPerMPEGFrame;
				}
				
				// 4 byte value containing total bytes
				if(BYTES_FLAG & flags) {
					if(32 > ancillaryBitsRemaining)
						continue;
					
					/*uint32_t bytes =*/ mad_bit_read(&stream.anc_ptr, 32);
					ancillaryBitsRemaining -= 32;
				}
				
				// 100 bytes containing TOC information
				if(TOC_FLAG & flags) {
					if(8 * 100 > ancillaryBitsRemaining)
						continue;
					
					unsigned i;
					for(i = 0; i < 100; ++i)
						mXingTOC[i] = mad_bit_read(&stream.anc_ptr, 8);
					
					ancillaryBitsRemaining -= (8 * 100);
				}
				
				// 4 byte value indicating encoded vbr scale
				if(VBR_SCALE_FLAG & flags) {
					if(32 > ancillaryBitsRemaining)
						continue;
					
					/*uint32_t vbrScale =*/ mad_bit_read(&stream.anc_ptr, 32);
					ancillaryBitsRemaining -= 32;
				}
				
				mFoundXingHeader = true;
				
				// Loook for the LAME header next
				// http://gabriel.mp3-tech.org/mp3infotag.html				
				if(32 > ancillaryBitsRemaining)
					continue;
				magic = mad_bit_read(&stream.anc_ptr, 32);
				
				ancillaryBitsRemaining -= 32;
				
				if('LAME' == magic) {
					
					if(LAME_HEADER_SIZE > ancillaryBitsRemaining)
						continue;
					
					/*unsigned char versionString [5 + 1];
					memset(versionString, 0, 6);*/
					
					for(unsigned i = 0; i < 5; ++i)
						/*versionString[i] =*/ mad_bit_read(&stream.anc_ptr, 8);
					
					/*uint8_t infoTagRevision =*/ mad_bit_read(&stream.anc_ptr, 4);
					/*uint8_t vbrMethod =*/ mad_bit_read(&stream.anc_ptr, 4);
					
					/*uint8_t lowpassFilterValue =*/ mad_bit_read(&stream.anc_ptr, 8);
					
					/*float peakSignalAmplitude =*/ mad_bit_read(&stream.anc_ptr, 32);
					/*uint16_t radioReplayGain =*/ mad_bit_read(&stream.anc_ptr, 16);
					/*uint16_t audiophileReplayGain =*/ mad_bit_read(&stream.anc_ptr, 16);
					
					/*uint8_t encodingFlags =*/ mad_bit_read(&stream.anc_ptr, 4);
					/*uint8_t athType =*/ mad_bit_read(&stream.anc_ptr, 4);
					
					/*uint8_t lameBitrate =*/ mad_bit_read(&stream.anc_ptr, 8);
					
					uint16_t encoderDelay = mad_bit_read(&stream.anc_ptr, 12);
					uint16_t encoderPadding = mad_bit_read(&stream.anc_ptr, 12);
										
					// Adjust encoderDelay and encoderPadding for MDCT/filterbank delays
					mEncoderDelay = encoderDelay + 528 + 1;
					mEncoderPadding = encoderPadding - (528 + 1);

					mTotalFrames = this->GetTotalFrames() - (mEncoderDelay + mEncoderPadding);
					
					/*uint8_t misc =*/ mad_bit_read(&stream.anc_ptr, 8);
					
					/*uint8_t mp3Gain =*/ mad_bit_read(&stream.anc_ptr, 8);
					
					/*uint8_t unused =*/mad_bit_read(&stream.anc_ptr, 2);
					/*uint8_t surroundInfo =*/ mad_bit_read(&stream.anc_ptr, 3);
					/*uint16_t presetInfo =*/ mad_bit_read(&stream.anc_ptr, 11);
					
					/*uint32_t musicGain =*/ mad_bit_read(&stream.anc_ptr, 32);
					
					/*uint32_t musicCRC =*/ mad_bit_read(&stream.anc_ptr, 32);
					
					/*uint32_t tagCRC =*/ mad_bit_read(&stream.anc_ptr, 32);
					
					ancillaryBitsRemaining -= LAME_HEADER_SIZE;
					
					mFoundLAMEHeader = true;
					break;
				}
			}
		}
		else {
			// Just estimate the number of frames based on the file's size
			mTotalFrames = (double)frame.header.samplerate * ((mFileBytes - id3_length) / (frame.header.bitrate / 8.0));
			
			// For now, quit after second frame
			break;
		}		
	}
	
	// Clean up
	mad_frame_finish(&frame);
	mad_stream_finish(&stream);
	
	// Rewind to the beginning of file
	if(-1 == fseek(mFile, 0, SEEK_SET))
		return false;
	
	return true;
}

SInt64 MPEGDecoder::SeekToFrameApproximately(SInt64 frame)
{
	double	fraction	= (double)frame / this->GetTotalFrames();
	long	seekPoint	= 0;
	
	// If a Xing header was found, interpolate in TOC
	if(mFoundXingHeader) {
		double		percent		= 100 * fraction;
		unsigned	firstIndex	= percent;
		
		if(99 < firstIndex)
			firstIndex = 99;
		
		double firstOffset	= mXingTOC[firstIndex];
		double secondOffset	= 256;
		
		if(99 > firstIndex)
			secondOffset = mXingTOC[firstIndex + 1];;
			
			double x = firstOffset + (secondOffset - firstOffset) * (percent - firstIndex);
			seekPoint = (long)((1.0 / 256.0) * x * mFileBytes); 
	}
	else
		seekPoint = (long)mFileBytes * fraction;
	
	int result = fseek(mFile, seekPoint, SEEK_SET);
	if(0 == result) {
		mad_stream_buffer(&mStream, NULL, 0);
		
		// Reset frame count to prevent early termination of playback
		mMPEGFramesDecoded			= 0;
		mSamplesDecoded				= 0;
		mSamplesToSkipInNextFrame	= 0;
		
		mCurrentFrame				= frame;
	}
	
	// Right now it's only possible to return an approximation of the audio frame
	return (-1 == result ? -1 : frame);
}

SInt64 MPEGDecoder::SeekToFrameAccurately(SInt64 frame)
{
	assert(0 <= frame);
	assert(frame < this->GetTotalFrames());
	
	// Brute force seeking is necessary since frame-accurate seeking is required
	
	UInt32			bytesToRead;
	UInt32			bytesRemaining;
	unsigned char	*readStartPointer;
	int32_t			audioSample;
	
	bool			readEOF					= false;
	float			scaleFactor				= (1L << (BIT_RESOLUTION - 1));
	
	// To seek to a frame earlier in the file, rewind to the beginning
	if(this->GetCurrentFrame() > frame) {
		if(-1 == fseek(mFile, 0, SEEK_SET))
			return -1;
		
		// Reset decoder parameters
		mMPEGFramesDecoded			= 0;
		mCurrentFrame				= 0;
		mSamplesToSkipInNextFrame	= 0;
		mSamplesDecoded				= 0;

		mad_stream_buffer(&mStream, NULL, 0);
	}
	// Mark any buffered audio as read
	else
		mCurrentFrame += mBufferList->mBuffers[0].mDataByteSize / sizeof(float);
	
	// Zero the buffers
	unsigned i;
	for(i = 0; i < mBufferList->mNumberBuffers; ++i)
		mBufferList->mBuffers[i].mDataByteSize = 0;
	
	for(;;) {
		// All requested frames were skipped or read
		if(mSamplesDecoded >= frame)
			break;

		// If the file contains a Xing header but not LAME gapless information,
		// decode the number of MPEG frames specified by the Xing header
		if(mFoundXingHeader && false == mFoundLAMEHeader && 1 + mMPEGFramesDecoded == mTotalMPEGFrames)
			break;
		
		// The LAME header indicates how many samples are in the file
		if(mFoundLAMEHeader && this->GetTotalFrames() == mSamplesDecoded)
			break;
		
		// Feed the input buffer if necessary
		if(NULL == mStream.buffer || MAD_ERROR_BUFLEN == mStream.error) {
			if(NULL != mStream.next_frame) {
				bytesRemaining = mStream.bufend - mStream.next_frame;
				memmove(mInputBuffer, mStream.next_frame, bytesRemaining);
				
				readStartPointer	= mInputBuffer + bytesRemaining;
				bytesToRead			= INPUT_BUFFER_SIZE - bytesRemaining;
			}
			else {
				bytesToRead			= INPUT_BUFFER_SIZE,
				readStartPointer	= mInputBuffer,
				bytesRemaining		= 0;
			}
			
			// Read raw bytes from the MP3 file
			size_t bytesRead = fread(readStartPointer, 1, bytesToRead, mFile);
			if(ferror(mFile)) {
#if DEBUG
				LOG("Read error: %s.", strerror(errno));
#endif
				break;
			}
			
			// MAD_BUFFER_GUARD zeroes are required to decode the last frame of the file
			if(feof(mFile)) {
				memset(readStartPointer + bytesRead, 0, MAD_BUFFER_GUARD);
				bytesRead	+= MAD_BUFFER_GUARD;
				readEOF		= true;
			}
			
			mad_stream_buffer(&mStream, mInputBuffer, bytesRead + bytesRemaining);
			mStream.error = MAD_ERROR_NONE;
		}
		
		// Decode the MPEG frame
		int result = mad_frame_decode(&mFrame, &mStream);
		if(-1 == result) {
			if(MAD_RECOVERABLE(mStream.error)) {
				// Prevent ID3 tags from reporting recoverable frame errors
				const uint8_t	*buffer			= mStream.this_frame;
				unsigned		buflen			= mStream.bufend - mStream.this_frame;
				uint32_t		id3_length		= 0;
				
				if(10 <= buflen && 0x49 == buffer[0] && 0x44 == buffer[1] && 0x33 == buffer[2]) {
					id3_length = (((buffer[6] & 0x7F) << (3 * 7)) | ((buffer[7] & 0x7F) << (2 * 7)) |
								  ((buffer[8] & 0x7F) << (1 * 7)) | ((buffer[9] & 0x7F) << (0 * 7)));
					
					// Add 10 bytes for ID3 header
					id3_length += 10;
					
					mad_stream_skip(&mStream, id3_length);
				}
#if DEBUG
				else
					LOG("Recoverable frame level error (%s)", mad_stream_errorstr(&mStream));
#endif
				
				continue;
			}
			// EOS for non-Xing streams occurs when EOF is reached and no further frames can be decoded
			else if(MAD_ERROR_BUFLEN == mStream.error && readEOF)
				break;
			else if(MAD_ERROR_BUFLEN == mStream.error)
				continue;
			else {
#if DEBUG
				LOG("Unrecoverable frame level error (%s)", mad_stream_errorstr(&mStream));
#endif
				break;
			}
		}
		
		// Housekeeping
		++mMPEGFramesDecoded;

		// Skip any samples that remain from last frame
		// This can happen if the encoder delay is greater than the number of samples in a frame
		unsigned startingSample = mSamplesToSkipInNextFrame;
		
		// Skip the Xing header (it contains empty audio)
		if(mFoundXingHeader && 1 == mMPEGFramesDecoded)
			continue;
		// Adjust the first real audio frame for gapless playback
		else if(mFoundLAMEHeader && 2 == mMPEGFramesDecoded)
			startingSample += mEncoderDelay;

		// The number of samples in this frame
		unsigned sampleCount = 32 * MAD_NSBSAMPLES(&mFrame.header);
		
		// Skip this entire frame if necessary
		if(startingSample > sampleCount) {
			mSamplesToSkipInNextFrame += startingSample - sampleCount;
			continue;
		}
		else
			mSamplesToSkipInNextFrame = 0;
		
		// If a LAME header was found, the total number of audio frames (AKA samples) 
		// is known.  Ensure only that many are output
		if(mFoundLAMEHeader && this->GetTotalFrames() < mSamplesDecoded + (sampleCount - startingSample))
			sampleCount = this->GetTotalFrames() - mSamplesDecoded;

		// If this MPEG frame contains the desired seek frame, synthesize its audio to PCM
		if(mSamplesDecoded + (sampleCount - startingSample) > frame) {
			// Synthesize the frame into PCM
			mad_synth_frame(&mSynth, &mFrame);

			// Skip any audio frames before the sample we are seeking to
			unsigned additionalSamplesToSkip = frame - mSamplesDecoded;
			
			// Output samples in 32-bit float PCM
			unsigned channel, sample;
			for(channel = 0; channel < MAD_NCHANNELS(&mFrame.header); ++channel) {
				float *floatBuffer = static_cast<float *>(mBufferList->mBuffers[channel].mData);
				
				for(sample = startingSample + additionalSamplesToSkip; sample < sampleCount; ++sample) {
					audioSample = audio_linear_round(BIT_RESOLUTION, mSynth.pcm.samples[channel][sample]);
					
					if(0 <= audioSample)
						*floatBuffer++ = (float)(audioSample / (scaleFactor - 1));
					else
						*floatBuffer++ = (float)(audioSample / scaleFactor);
				}
				
				mBufferList->mBuffers[channel].mNumberChannels	= 1;
				mBufferList->mBuffers[channel].mDataByteSize	= (sampleCount - (startingSample + additionalSamplesToSkip)) * sizeof(float);
			}

			// Only a portion of the frame was skipped- the rest was synthesized and stored in our buffers
			mSamplesDecoded		+= (sampleCount - startingSample);
			mCurrentFrame		+= additionalSamplesToSkip;
		}
		// The entire frame was skipped
		else {
			mSamplesDecoded		+= (sampleCount - startingSample);
			mCurrentFrame		+= (sampleCount - startingSample);
		}
	}
	
	return this->GetCurrentFrame();
}
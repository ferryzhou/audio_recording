// version 0.3

#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string>
using namespace std;

#pragma comment(lib,"winmm.lib")

struct AudioDataListener {
	virtual ~AudioDataListener() {}
	virtual void onNewData(void* data, int dataCountInBytes) = 0;
	virtual void onStopped() = 0;
};

struct AudioFormat {
	int bits;
	int samplesPerSecond;
	int channelCount;
};

// default 16 bits, 44.1KHz, stereo, single input device
struct AudioRecordController {
	virtual void setListener(AudioDataListener* listener) = 0;
	// check failed() to get error info
	virtual void start() = 0;
	virtual void stop() = 0;
	virtual bool failed() const = 0;
	virtual const char* getErrorMessage() const = 0;
	virtual AudioFormat getAudioFormat() const = 0;
	virtual void onDeviceStopped() = 0;
};


#define RECORD_BUFFER_SIZE      327680L
#define DATABLOCK_SIZE          32768L
#define MAX_DEVICE_COUNT        2
#define MSG_LEN                 128

class AudioDataListenerFileImpl : public AudioDataListener {
	FILE* audioFile;
	void* memoryBuffer;  
	int memoryBufferPosition;  
	int memoryBufferLength;

	void writeData(const void* data, size_t count) {
		fwrite(data, 1, count, audioFile);
	}

public:
	AudioDataListenerFileImpl(const char* filename) {
		audioFile = fopen("test.raw", "wb");
		memoryBuffer = new BYTE[RECORD_BUFFER_SIZE];
		memoryBufferPosition = 0;
		memoryBufferLength = 0;
	}

	void onNewData(void* data, int dataCountInBytes) {
		if ((memoryBufferPosition + dataCountInBytes) >= RECORD_BUFFER_SIZE) {
			writeData(memoryBuffer, memoryBufferLength);
			memoryBufferPosition = 0L;
			memoryBufferLength = 0L;
		}
		
		memcpy(((BYTE*)memoryBuffer+memoryBufferPosition), data, dataCountInBytes);
		memoryBufferPosition += dataCountInBytes;
		memoryBufferLength += dataCountInBytes;
	}

	void onStopped() {
		writeData(memoryBuffer, memoryBufferLength);
		fclose(audioFile);
	}
};


class AudioRecordControllerWinImpl : public AudioRecordController {
	AudioDataListener* listener;
	std::string errorMessage;

	int deviceId;
	HWAVEIN  inputDevice;
	LPWAVEHDR  inputDeviceBuffer;

	WAVEINCAPS   deviceCapbilities; 
	WAVEFORMATEX waveFormat;
	MMRESULT     result;
	MMTIME    mmtime;

	char winMsg[MSG_LEN+1];

public:
	bool processingBlock; 
	bool resetStarted;

private:
	void initialize();

	void reportError(MMRESULT result, const char* message = NULL) {
		waveInGetErrorTextA(result, winMsg, MSG_LEN);
		if (message != NULL) errorMessage = message;
		errorMessage += string("windows error: ") + winMsg;
	}

	void setFormat();

	void sendNewRequest(LPWAVEHDR deviceBuffer);

public:
	AudioRecordControllerWinImpl();
	void setListener(AudioDataListener* listener) { this->listener = listener; } 
	void start();
	void stop();
	AudioFormat getAudioFormat() const {
		AudioFormat audioFormat;
		audioFormat.bits = waveFormat.wBitsPerSample;
		audioFormat.channelCount = waveFormat.nChannels;
		audioFormat.samplesPerSecond = waveFormat.nSamplesPerSec;
		return audioFormat;
	}
	bool failed() const { return !errorMessage.empty(); }
	const char* getErrorMessage() const { return errorMessage.c_str(); }

	void processNewData(LPWAVEHDR deviceBuffer);

	void onDeviceStopped() { listener->onStopped(); }
};

void trace(const char* msg) { printf("%s\n", msg); }

void processInblock(AudioRecordControllerWinImpl* context, LPWAVEHDR deviceBuffer) {
	if (context->resetStarted) return;
	if (context->failed()) return;
	trace("Process in block ..................");
	context->processingBlock = true;

	context->processNewData(deviceBuffer);

	context->processingBlock = false;
}

void CALLBACK waveInProc(HWAVEIN inputDevice, UINT uMsg, DWORD dwInstance, DWORD dwParam1, DWORD dwParam2) {
	AudioRecordControllerWinImpl* controller = (AudioRecordControllerWinImpl*)dwInstance;
	switch(uMsg) {
		case WIM_CLOSE:
			trace("waveInProc get close message ..........");
			controller->onDeviceStopped();
			break;

		case WIM_DATA:
			processInblock(controller, (LPWAVEHDR)dwParam1);
			break;

		case WIM_OPEN:
			break;

		default:
			break;
	}
}

AudioRecordControllerWinImpl::AudioRecordControllerWinImpl() {
	inputDeviceBuffer = NULL;
}

void AudioRecordControllerWinImpl::initialize() {
	errorMessage = "";
	deviceId = 0;
	inputDevice = NULL;
	mmtime.wType = TIME_SAMPLES;
	resetStarted = false;

	if (inputDeviceBuffer == NULL) {
		inputDeviceBuffer = new WAVEHDR;
		inputDeviceBuffer->lpData = new char[DATABLOCK_SIZE]; 
		inputDeviceBuffer->dwBufferLength = DATABLOCK_SIZE;
		inputDeviceBuffer->dwFlags = 0;
	}
}

#define RETURN_IF_ERROR(result) if (result != MMSYSERR_NOERROR) { reportError(result); return; } 

void AudioRecordControllerWinImpl::setFormat() {
	// attempt 44.1 kHz stereo if device is capable
	if (deviceCapbilities.dwFormats & WAVE_FORMAT_4S16) {
		waveFormat.nChannels      = 2;      // stereo
		waveFormat.nSamplesPerSec = 44100;  // 44.1 kHz (44.1 * 1000)
		trace("recording 44100, stereo ..........");
	} else { 
		waveFormat.nChannels      = deviceCapbilities.wChannels;  // use DevCaps # channels
		waveFormat.nSamplesPerSec = 22050;  // 22.05 kHz (22.05 * 1000)
		trace("recording 22050,  ..........");
		printf("channels: %d", waveFormat.nChannels);
	}

	waveFormat.wFormatTag      = WAVE_FORMAT_PCM; 
	waveFormat.wBitsPerSample  = 16;
	waveFormat.nBlockAlign     = waveFormat.nChannels * waveFormat.wBitsPerSample / 8;   
	waveFormat.nAvgBytesPerSec = waveFormat.nSamplesPerSec * waveFormat.nBlockAlign;     
	waveFormat.cbSize          = 0;
}

void AudioRecordControllerWinImpl::start() {
	initialize();
	trace("Open Device ...........");
	UINT deviceCount = waveInGetNumDevs(); if (deviceCount == 0) { errorMessage = "Can not find input device!"; return; }
	result = waveInGetDevCaps(deviceId, &deviceCapbilities, sizeof(deviceCapbilities)); RETURN_IF_ERROR(result);
	setFormat();
	result = waveInOpen(&inputDevice, deviceId, &waveFormat, (DWORD)(VOID*)waveInProc, (DWORD_PTR)this, CALLBACK_FUNCTION);  RETURN_IF_ERROR(result);
	trace("Successfully opened device .........");

	result = waveInPrepareHeader(inputDevice, inputDeviceBuffer, sizeof(WAVEHDR)); RETURN_IF_ERROR(result);
	result = waveInAddBuffer(inputDevice, inputDeviceBuffer, sizeof(WAVEHDR)); RETURN_IF_ERROR(result);
	result = waveInStart(inputDevice); RETURN_IF_ERROR(result);
	result = waveInGetPosition(inputDevice, &mmtime, sizeof(MMTIME)); RETURN_IF_ERROR(result);

	trace("succuessfully started recording .............");
}

void AudioRecordControllerWinImpl::stop() {
	trace("Stop recording .............");
	while (processingBlock) { trace("waiting for the processing finished");	Sleep(10); }

	trace("waveInReset started .............");
	resetStarted = true;
	result = waveInReset(inputDevice); RETURN_IF_ERROR(result); trace("waveInReset finished .............");
	result = waveInUnprepareHeader(inputDevice, inputDeviceBuffer, sizeof(WAVEHDR)); RETURN_IF_ERROR(result);
	result = waveInClose(inputDevice); RETURN_IF_ERROR(result);

	trace("closed recording\n");
}

void AudioRecordControllerWinImpl::processNewData(LPWAVEHDR deviceBuffer) {
	listener->onNewData(deviceBuffer->lpData, deviceBuffer->dwBytesRecorded);
	sendNewRequest(deviceBuffer);
	//MMTIME    mmtime;
	//mmtime.wType = TIME_BYTES;
	//waveInGetPosition(inputDevice, &mmtime, sizeof(MMTIME));
}

void AudioRecordControllerWinImpl::sendNewRequest(LPWAVEHDR deviceBuffer) {
	result = waveInPrepareHeader(inputDevice, deviceBuffer, sizeof(WAVEHDR)); RETURN_IF_ERROR(result);
	result = waveInAddBuffer(inputDevice, deviceBuffer, sizeof(WAVEHDR)); RETURN_IF_ERROR(result);
}

#include <iostream>

void exitIfError(AudioRecordController* controller) {
	if (controller->failed()) { cout<<"failed to start: "<<controller->getErrorMessage()<<endl; exit(1); }
}

void main() {

	AudioDataListener* fileListener = new AudioDataListenerFileImpl("test.raw");

	AudioRecordController* controller = new AudioRecordControllerWinImpl();
	controller->setListener(fileListener);

	controller->start(); exitIfError(controller);

	Sleep(5000);

	controller->stop(); exitIfError(controller);

	trace("finished ...........");
}

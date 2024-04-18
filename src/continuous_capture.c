#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include "portaudio.h"
#include "pa_ringbuffer.h"
#include "pa_util.h"

#define min(a, b) (((a) < (b)) ? (a) : (b))


#define SAMPLE_RATE 48000
#define NUM_CHANNELS  2
#//define FRAME_SIZE (NUM_CHANNELS * sizeof(float))
#define FRAME_SIZE (sizeof(float))
#define FRAMES_PER_BUFFER 512
#define FILE_NAME          "capture.raw"
#define PA_SAMPLE_TYPE  paFloat32

typedef float SAMPLE;
SAMPLE sampleSilence[FRAMES_PER_BUFFER * NUM_CHANNELS]={(0.0f)};

volatile sig_atomic_t keepRunning = 1; 

/* Custom data structure for passing to callback */
typedef struct {
    PaUtilRingBuffer ringBuffer;
    SAMPLE          *ringBufferData;
    FILE            *file;
} AudioData;


void sigint_handler(int sig){
    keepRunning = 0; 
}

/* Audio Callback Function */
static int recordCallback(const void *inputBuffer, void *outputBuffer,
                          unsigned long framesPerBuffer,
                          const PaStreamCallbackTimeInfo* timeInfo,
                          PaStreamCallbackFlags statusFlags,
                          void *userData) 
{
    AudioData *data = (AudioData*)userData;
    const SAMPLE *rptr = (const SAMPLE*)inputBuffer;
    if(data != NULL && data->ringBufferData != NULL){
        
        ring_buffer_size_t elementsWriteable = PaUtil_GetRingBufferWriteAvailable(&data->ringBuffer);
        //printf("Writable %lu elements", elementsWriteable); fflush(stdout);
        ring_buffer_size_t elementsToWrite = min(elementsWriteable, (ring_buffer_size_t)(framesPerBuffer * NUM_CHANNELS));
        if ( inputBuffer != NULL){ 
            PaUtil_WriteRingBuffer(&data->ringBuffer, rptr, elementsToWrite);
        } else {
            PaUtil_WriteRingBuffer(&data->ringBuffer, sampleSilence,elementsToWrite);
        }
    }

    
    (void) outputBuffer; /* Prevent unused variable warnings. */
    (void) timeInfo;
    (void) statusFlags;
    (void) userData;

    return keepRunning ? paContinue : paComplete;
}

static unsigned NextPowerOf2(unsigned val)
{
    val--;
    val = (val >> 1) | val;
    val = (val >> 2) | val;
    val = (val >> 4) | val;
    val = (val >> 8) | val;
    val = (val >> 16) | val;
    return ++val;
}

int main(void){

    PaStreamParameters  inputParameters;
    PaStream*           stream;
    PaError             err = paNoError;
    AudioData           microphoneData;
    unsigned long       numSamples, numBytes;
    printf("Initiating continuous audio capture. size of sample and float and sample type: %d %d %d\n", sizeof(SAMPLE), sizeof(float), sizeof(PA_SAMPLE_TYPE)); fflush(stdout);
    numSamples = NextPowerOf2((unsigned long) SAMPLE_RATE * 0.5 * NUM_CHANNELS);
    numBytes = numSamples * sizeof(SAMPLE);
    microphoneData.ringBufferData = (SAMPLE*)malloc(numBytes);
    if(microphoneData.ringBufferData == NULL){
        perror("Could not allocate memory for ring buffer");
        exit(1);
    }
    
    if (PaUtil_InitializeRingBuffer(&microphoneData.ringBuffer, sizeof(SAMPLE), numSamples, microphoneData.ringBufferData) < 0)
    {
        printf("Failed to initialize ring buffer. Size is not power of 2 ??\n");
        free(microphoneData.ringBufferData);
        exit(2);
    }

    err = Pa_Initialize();
    if( err != paNoError ){
        fprintf(stderr, "An error occurred: %s\n", Pa_GetErrorText(err));
        if (microphoneData.ringBufferData) free(microphoneData.ringBufferData);   
        exit(3);
    }

    inputParameters.device = Pa_GetDefaultInputDevice();
    if (inputParameters.device == paNoDevice) {
        fprintf(stderr,"Error: No default input device.\n");
        if (microphoneData.ringBufferData) free(microphoneData.ringBufferData); 
        Pa_Terminate();  
        exit(4);
    }
    inputParameters.channelCount = NUM_CHANNELS;                    /* stereo input */
    inputParameters.sampleFormat = PA_SAMPLE_TYPE;
    inputParameters.suggestedLatency = Pa_GetDeviceInfo( inputParameters.device )->defaultLowInputLatency;
    inputParameters.hostApiSpecificStreamInfo = NULL;
    
    /* Record some audio. -------------------------------------------- */
    err = Pa_OpenStream(
              &stream,
              &inputParameters,
              NULL,                  /* &outputParameters, */
              SAMPLE_RATE,
              FRAMES_PER_BUFFER,
              paClipOff,      /* we won't output out of range samples so don't bother clipping them */
              recordCallback,
              &microphoneData );
    
    if( err != paNoError ) {
        fprintf(stderr, "An error occurred: %s\n", Pa_GetErrorText(err));
        if (microphoneData.ringBufferData) free(microphoneData.ringBufferData);   
        Pa_Terminate();
        exit(5);
    }
    /* Open the raw audio 'cache' file... */
    microphoneData.file = fopen(FILE_NAME, "wb");
    if (microphoneData.file == 0) {
        perror("COuld not open file for writing. \n");
        Pa_Terminate();
        free(microphoneData.ringBufferData);
        exit(6);
    }
    /* Setup Signal Handler for CTRL+C */
    signal(SIGINT, sigint_handler);

    err = Pa_StartStream( stream );
    if( err != paNoError ) {
        fprintf(stderr, "An error occurred: %s\n", Pa_GetErrorText(err));
        if (microphoneData.ringBufferData) free(microphoneData.ringBufferData);
        Pa_Terminate();   
        fclose(microphoneData.file);
        exit(7);
    }
    printf("\n=== Started stream. Recording, Press CTRL+C to stop.\n"); fflush(stdout);
    SAMPLE buffer[numBytes];
    while(keepRunning){
        
        size_t itemsToRead = PaUtil_GetRingBufferReadAvailable(&microphoneData.ringBuffer);
        if (itemsToRead > 0) {
            //float buffer[itemsToRead * 2* sizeof(SAMPLE)];

            ring_buffer_size_t itemsRead = PaUtil_ReadRingBuffer(&microphoneData.ringBuffer, buffer, itemsToRead);
            fwrite(buffer, FRAME_SIZE, itemsRead, microphoneData.file);
        }
        Pa_Sleep(80);
    }

    err = Pa_StopStream(stream);
    if( err != paNoError){
        fprintf(stderr, "An error occurred: %s\n", Pa_GetErrorText(err));
        if (microphoneData.ringBufferData) free(microphoneData.ringBufferData);
        Pa_Terminate();
        fclose(microphoneData.file);
    }

    /* Clean up and termination */
    
    fclose(microphoneData.file);
    Pa_CloseStream(stream);
    free(microphoneData.ringBufferData);
    Pa_Terminate();
    printf("Done.\n");
    return 0;
}
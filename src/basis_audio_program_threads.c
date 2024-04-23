#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "portaudio.h"
#include <signal.h>
#include <pthread.h>
#include "pa_ringbuffer.h"
#include "pa_util.h"


#define min(a, b) (((a) < (b)) ? (a) : (b))


typedef float SAMPLE;
#define NUM_CHANNELS          (2)
#define SAMPLE_RATE       (48000)
#define FRAMES_PER_BUFFER   (512)
#define NUM_SECONDS          (60)
/* #define DITHER_FLAG     (paDitherOff)  */
#define DITHER_FLAG           (0)

/* Select sample format. */

#define PA_SAMPLE_TYPE  paFloat32
#define SAMPLE_SIZE (sizeof(float))
#define SAMPLE_SILENCE  (0.0f)
#define PRINTF_S_FORMAT "%.8f"
#define FILE_NAME       "thread_audio.raw"
#define GAIN_FACTOR     2.0f

static unsigned NextPowerOf2(unsigned val);
void sigint_handler(int sig);
void *writing_thread_function(void *arg);

/* Custom data structure for passing to callback */
typedef struct {
    PaUtilRingBuffer ringBuffer;
    SAMPLE          *sampleData;
    FILE            *file;
} AudioData;


volatile sig_atomic_t keepRunning = 1; 


SAMPLE sampleSilence[FRAMES_PER_BUFFER * NUM_CHANNELS]={(0.0f)};

/* This callback function captures data from the mic and simultaneously 
writes that to a ring buffer and to the USB audio device output buffer 
for playback. */
static int streamCallback(const void *inputBuffer, void *outputBuffer,
                          unsigned long framesPerBuffer,
                          const PaStreamCallbackTimeInfo* timeInfo,
                          PaStreamCallbackFlags statusFlags,
                          void *userData) 
{
    AudioData *data = (AudioData*)userData;
    const SAMPLE *captured_data = (const SAMPLE*)inputBuffer;
    SAMPLE *playback_data = (SAMPLE *)outputBuffer;

    if(data != NULL){
        // write captured data to the ring buffer
        if (data->sampleData != NULL){
            ring_buffer_size_t elementsWriteable = PaUtil_GetRingBufferWriteAvailable(&data->ringBuffer);
            //printf("Writable %lu elements", elementsWriteable); fflush(stdout);
            ring_buffer_size_t elementsToWrite = min(elementsWriteable, (ring_buffer_size_t)(framesPerBuffer * NUM_CHANNELS));
            if ( inputBuffer != NULL){ 
                PaUtil_WriteRingBuffer(&data->ringBuffer, captured_data, elementsToWrite);
            } else {
                PaUtil_WriteRingBuffer(&data->ringBuffer, sampleSilence,elementsToWrite);
            }
        }
        // write captured data to the output buffer for playback
        for(uint32_t i= 0; i < framesPerBuffer; i++){
            *playback_data++ = GAIN_FACTOR * ( *captured_data++ );
            *playback_data++ = GAIN_FACTOR * ( *captured_data++ );
        }
    }

    (void) timeInfo;
    (void) statusFlags;

    return keepRunning ? paContinue : paComplete;
}


int main(void)
{
    PaStreamParameters inputParameters, outputParameters;
    PaStream *stream = NULL;
    PaError err = paNoError;
    const PaDeviceInfo* inputInfo;
    const PaDeviceInfo* outputInfo;
    AudioData           microphoneData; // user data
    unsigned long       numSamples, numBytes;
    
    pthread_t thread1;
    int ret1; // return value from thread

    printf("Initiating continuous audio capture. size of sample and float and sample type: %d %d %d\n", sizeof(SAMPLE), sizeof(float), sizeof(PA_SAMPLE_TYPE)); fflush(stdout);
    numSamples = NextPowerOf2((unsigned long) SAMPLE_RATE * 0.5 * NUM_CHANNELS); // half a second of samples can fit
    numBytes = numSamples * sizeof(SAMPLE);
    
    microphoneData.sampleData = (SAMPLE*)malloc(numBytes);
    if(microphoneData.sampleData == NULL){
        perror("Could not allocate memory for ring buffer");
        exit(1);
    }
    
    if (PaUtil_InitializeRingBuffer(&microphoneData.ringBuffer, sizeof(SAMPLE), numSamples, microphoneData.sampleData) < 0)
    {
        printf("Failed to initialize ring buffer. Size is not power of 2 ??\n");
        free(microphoneData.sampleData);
        exit(2);
    }

    /* -- device setup -- */
    err = Pa_Initialize();
    if( err != paNoError ) goto error2;

    inputParameters.device = Pa_GetDefaultInputDevice(); /* default input device */
    printf( "Input device # %d.\n", inputParameters.device );
    inputInfo = Pa_GetDeviceInfo( inputParameters.device );
    printf( "    Name: %s\n", inputInfo->name );
    printf( "      LL: %g s\n", inputInfo->defaultLowInputLatency );
    printf( "      HL: %g s\n", inputInfo->defaultHighInputLatency );

    outputParameters.device = 2; /* USB audio output device */
    printf( "Output device # %d.\n", outputParameters.device );
    outputInfo = Pa_GetDeviceInfo( outputParameters.device );
    printf( "   Name: %s\n", outputInfo->name );
    printf( "     LL: %g s\n", outputInfo->defaultLowOutputLatency );
    printf( "     HL: %g s\n", outputInfo->defaultHighOutputLatency );

    inputParameters.channelCount = NUM_CHANNELS;
    inputParameters.sampleFormat = PA_SAMPLE_TYPE;
    inputParameters.suggestedLatency = inputInfo->defaultHighInputLatency ;
    inputParameters.hostApiSpecificStreamInfo = NULL;

    outputParameters.channelCount = NUM_CHANNELS;
    outputParameters.sampleFormat = PA_SAMPLE_TYPE;
    outputParameters.suggestedLatency = outputInfo->defaultHighOutputLatency;
    outputParameters.hostApiSpecificStreamInfo = NULL;

    /* -- stream setup -- */
    err = Pa_OpenStream(
              &stream,
              &inputParameters,
              &outputParameters,
              SAMPLE_RATE,
              FRAMES_PER_BUFFER,
              paClipOff,      /* we won't output out of range samples so don't bother clipping them */
              streamCallback, 
              &microphoneData ); 
    if( err != paNoError ) goto error2;

    // Starting the thread
    ret1 = pthread_create(&thread1, NULL, writing_thread_function, (void *)&microphoneData);
    if (ret1) {
        fprintf(stderr, "Error - pthread_create() return code: %d\n", ret1);
        exit(EXIT_FAILURE);
    }
    /* Setup Signal Handler for CTRL+C */
    signal(SIGINT, sigint_handler);

    err = Pa_StartStream( stream );
    if( err != paNoError ) goto error1;
    printf("\n=== Started stream. Recording, Press CTRL+C to stop.\n"); fflush(stdout);
    
    // wait for thread to finish (triggered by CRTL + C)
    pthread_join(thread1, NULL);


    err = Pa_StopStream( stream );
    if( err != paNoError ) goto error1;
    printf("Stopped stream.\n");
    if (microphoneData.sampleData) free(microphoneData.sampleData);
    Pa_Terminate();
    printf("Terminated sources.\n"); fflush(stdout);
    return 0;

xrun:
    printf("err = %d\n", err); fflush(stdout);
    if( stream ) {
        Pa_AbortStream( stream );
        Pa_CloseStream( stream );
    }
    if (microphoneData.sampleData) free(microphoneData.sampleData);
    Pa_Terminate();
    if( err & paInputOverflow )
        fprintf( stderr, "Input Overflow.\n" );
    if( err & paOutputUnderflow )
        fprintf( stderr, "Output Underflow.\n" );
    return -2;
error1:
    if (microphoneData.sampleData) free(microphoneData.sampleData);
    Pa_Terminate();
    return -3;
error2:
    if( stream ) {
        Pa_AbortStream( stream );
        Pa_CloseStream( stream );
    }
    Pa_Terminate();
    if (microphoneData.sampleData) free(microphoneData.sampleData);
    fprintf( stderr, "An error occurred while using the portaudio stream\n" );
    fprintf( stderr, "Error number: %d\n", err );
    fprintf( stderr, "Error message: %s\n", Pa_GetErrorText( err ) );
    return -1;
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

void sigint_handler(int sig){
    keepRunning = 0; 
}

void *writing_thread_function(void *arg){

    SAMPLE *file_buffer;
    unsigned long       numSamples, numBytes;
    AudioData *userdata = (AudioData *)arg;

    numSamples = NextPowerOf2((unsigned long) SAMPLE_RATE * 0.5 * NUM_CHANNELS); // half a second of samples can fit
    numBytes = numSamples * sizeof(SAMPLE);
    file_buffer = (SAMPLE*) malloc(numBytes);

    if(file_buffer == NULL){
        perror("Could not allocate memory for file buffer. Data will not be written to file! \n");
        return (void *)(intptr_t)(-2); 
    }
    
    /* Open the raw audio 'cache' file... */
    userdata->file = fopen(FILE_NAME, "wb");
    if (userdata->file == 0) {
        fprintf(stderr, "Error: File has not been opened successfully.\n");
        return (void *)(intptr_t)(-1);
    }

    while(keepRunning){
        
        size_t itemsToRead = PaUtil_GetRingBufferReadAvailable(&userdata->ringBuffer);
        if (itemsToRead > 0) {
            ring_buffer_size_t itemsRead = PaUtil_ReadRingBuffer(&userdata->ringBuffer, file_buffer, itemsToRead);
            fwrite(file_buffer, SAMPLE_SIZE, itemsRead, userdata->file);
        }
        Pa_Sleep(80);
    }

    if (file_buffer) free(file_buffer);
    if(userdata->file) fclose(userdata->file);
    return NULL;
}
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "portaudio.h"
#include <signal.h>
#include <pthread.h>
#include "pa_ringbuffer.h"
#include "pa_util.h"
#include <sndfile.h>
#include <time.h>

#define min(a, b) (((a) < (b)) ? (a) : (b))



#define NUM_CHANNELS          (2)
#define SAMPLE_RATE       (44100)
#define FRAMES_PER_BUFFER   (512)
#define NUM_SECONDS          (60)
/* #define DITHER_FLAG     (paDitherOff)  */
#define DITHER_FLAG           (0)

/* Select sample format. */
typedef float SAMPLE;
#define PA_SAMPLE_TYPE  paFloat32
#define SAMPLE_SIZE (sizeof(float))
#define SAMPLE_SILENCE  (0.0f)
#define PRINTF_S_FORMAT "%.8f"
#define FILE_NAME_RAW       "audio_file.raw"
#define FILE_NAME_WAV       "audio_file.wav"
#define TIMESTAMPS_FILE     "timestamps.txt"
#define GAIN_FACTOR     2.0f

static unsigned NextPowerOf2(unsigned val);
void sigint_handler(int sig);
void *writing_thread_function(void *arg);
static uint8_t convert_raw_to_wav(const  char* raw_file, const char* wav_file);

typedef struct {
    SAMPLE sample;
    struct timespec timestamp;
} TimestampedSample;

TimestampedSample sampleSilence[FRAMES_PER_BUFFER * NUM_CHANNELS] = { {0.0f, {0, 0}} };

/* Custom data structure for passing to callback */
typedef struct {
    PaUtilRingBuffer ringBuffer;
    TimestampedSample          *sampleData;
    FILE            *file;
} AudioData;


volatile sig_atomic_t keepRunning = 1; 



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

    struct timespec current_time;
    clock_gettime(CLOCK_REALTIME, &current_time);
    double frame_start_time = current_time.tv_sec + (current_time.tv_nsec / 1e9);
    double sample_time_interval = 1.0 / SAMPLE_RATE;

    if(data != NULL){
        // write captured data to the ring buffer
        if (data->sampleData != NULL){
            ring_buffer_size_t elementsWriteable = PaUtil_GetRingBufferWriteAvailable(&data->ringBuffer);
            //printf("Writable %lu elements", elementsWriteable); fflush(stdout);
            ring_buffer_size_t elementsToWrite = min(elementsWriteable, (ring_buffer_size_t)(framesPerBuffer * NUM_CHANNELS));

            TimestampedSample timestampedData[framesPerBuffer * NUM_CHANNELS];
            if ( inputBuffer != NULL){ 
                for (unsigned long i = 0; i < elementsToWrite; ++i) {
                    timestampedData[i].sample = captured_data[i];
                    double precise_time = frame_start_time + (i / (double)NUM_CHANNELS) * sample_time_interval;
                    timestampedData[i].timestamp.tv_sec = (time_t)precise_time;
                    timestampedData[i].timestamp.tv_nsec = (long)((precise_time - timestampedData[i].timestamp.tv_sec) * 1e9);
                }
                PaUtil_WriteRingBuffer(&data->ringBuffer, timestampedData, elementsToWrite);
            } else {
                PaUtil_WriteRingBuffer(&data->ringBuffer, sampleSilence,elementsToWrite);
            }
        }
        // write captured data to the output buffer for playback
        for(uint32_t i= 0; i < framesPerBuffer; i++){
            *playback_data++ = GAIN_FACTOR * ( *captured_data++ );  /* left */
            if( NUM_CHANNELS == 2 ) *playback_data++ = GAIN_FACTOR * ( *captured_data++ ); /* right */
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
    int deviceIndex = -1; 
    pthread_t thread1;
    int ret1; // return value from thread

    printf("Initiating continuous audio capture. size of sample and float and sample type: %d %d %d\n", sizeof(SAMPLE), sizeof(float), sizeof(PA_SAMPLE_TYPE)); fflush(stdout);
    numSamples = NextPowerOf2((unsigned long) SAMPLE_RATE * 0.5 * NUM_CHANNELS); // half a second of samples can fit
    numBytes = numSamples * sizeof(TimestampedSample);
    
    microphoneData.sampleData = (TimestampedSample*)malloc(numBytes);
    if(microphoneData.sampleData == NULL){
        perror("Could not allocate memory for ring buffer");
        exit(1);
    }
    
    if (PaUtil_InitializeRingBuffer(&microphoneData.ringBuffer, sizeof(TimestampedSample), numSamples, microphoneData.sampleData) < 0)
    {
        printf("Failed to initialize ring buffer. Size is not power of 2 ??\n");
        free(microphoneData.sampleData);
        exit(2);
    }

    /* -- device setup -- */
    err = Pa_Initialize();
    if( err != paNoError ) goto error2;
    // Get the number of available devices
    int numDevices = Pa_GetDeviceCount();
    if (numDevices < 0)
    {
        fprintf(stderr, "ERROR: Pa_CountDevices returned 0x%x\n", numDevices);
        return 1;
    }

    // Iterate over devices and find the one matching "snd_rpi_i2s_card"
    for (int i = 0; i < numDevices; i++)
    {
        const PaDeviceInfo *deviceInfo = Pa_GetDeviceInfo(i);
        if (deviceInfo != NULL && strstr(deviceInfo->name, "snd_rpi_i2s_card") != NULL)
        {
            deviceIndex = i;
            break;
        }
    }

    if (deviceIndex == -1)
    {
        fprintf(stderr, "Error: Device not found.\n");
        Pa_Terminate();
        return 1;
    }

    inputParameters.device = deviceIndex; /* default input device */
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
    printf("\nStopped stream.\n");
    if (microphoneData.sampleData) free(microphoneData.sampleData);
    printf("Freed allocated source.\n");
    PaError error_type = Pa_Terminate();
    if ( error_type != paNoError){
        fprintf( stdout, "Error number: %d\n", error_type ); fflush(stdout);
        fprintf( stdout, "Error message: %s\n", Pa_GetErrorText( error_type ) ); fflush(stdout);
    }
    printf("Terminated sources.\n"); fflush(stdout);
    uint8_t status = convert_raw_to_wav(FILE_NAME_RAW, FILE_NAME_WAV);
    if ( status != 0) printf("Could not convert .raw to .wav file!Error code: %u\n", status);
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

    TimestampedSample *file_buffer;
    unsigned long       numSamples, numBytes;
    AudioData *userdata = (AudioData *)arg;

    numSamples = NextPowerOf2((unsigned long) SAMPLE_RATE * 0.5 * NUM_CHANNELS); // half a second of samples can fit
    numBytes = numSamples * sizeof(TimestampedSample);
    file_buffer = (TimestampedSample*) malloc(numBytes);

    if(file_buffer == NULL){
        perror("Could not allocate memory for file buffer. Data will not be written to file! \n");
        return (void *)(intptr_t)(-2); 
    }
    
    /* Open the raw audio 'cache' file... */
    userdata->file = fopen(FILE_NAME_RAW, "wb");
    FILE *timestamp_file = fopen(TIMESTAMPS_FILE, "w");
    if (userdata->file == 0 || timestamp_file == 0) {
        fprintf(stderr, "Error: File has not been opened successfully.\n");
        return (void *)(intptr_t)(-1);
    }

    while(keepRunning){
        
        size_t itemsToRead = PaUtil_GetRingBufferReadAvailable(&userdata->ringBuffer);
        if (itemsToRead > 0) {
            ring_buffer_size_t itemsRead = PaUtil_ReadRingBuffer(&userdata->ringBuffer, file_buffer, itemsToRead);
            for (ring_buffer_size_t i = 0; i < itemsRead; ++i) {
                fwrite(&file_buffer[i].sample, SAMPLE_SIZE, 1, userdata->file);
                fprintf(timestamp_file, "%ld.%09ld\n", file_buffer[i].timestamp.tv_sec, file_buffer[i].timestamp.tv_nsec);
            }
        }
        Pa_Sleep(100);
    }

    if (file_buffer) free(file_buffer);
    if(userdata->file) fclose(userdata->file);
    if(timestamp_file) fclose(timestamp_file);
    return NULL;
}

static uint8_t convert_raw_to_wav(const  char* raw_file, const char* wav_file){
    SF_INFO sfinfo; 
    sfinfo.samplerate = SAMPLE_RATE;
    sfinfo.channels = NUM_CHANNELS;
    sfinfo.format = SF_FORMAT_RAW | SF_FORMAT_FLOAT; 

    SNDFILE *infile = sf_open(raw_file, SFM_READ, &sfinfo);
    if (!infile) {
        fprintf(stderr, "Error opening input file.\n");
        return 1;
    }

    // Update format for the WAV output
    sfinfo.format = SF_FORMAT_WAV | SF_FORMAT_FLOAT;

    // Open the WAV file for output
    SNDFILE *outfile = sf_open(wav_file, SFM_WRITE, &sfinfo);
    if (!outfile) {
        fprintf(stderr, "Error opening output file.\n");
        sf_close(infile);
        return 1;
    }

    // Buffer to hold audio samples
    static const int BUFFER_SIZE = 1024;
    SAMPLE buffer[BUFFER_SIZE];

    // Read from raw, write to wav
    int readcount;
    while ((readcount = sf_read_float(infile, buffer, BUFFER_SIZE)) > 0) {
        sf_write_float(outfile, buffer, readcount);
    }

    // Close files
    sf_close(infile);
    sf_close(outfile);

    return 0;
}
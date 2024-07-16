#include <stdio.h>
#include <stdlib.h>
#include <sndfile.h>
#include <fftw3.h>
#include <math.h>

#define BASELINE    1 
#define SPEED_SOUND 343
#define WAV_FILE    "/home/pi/prof_audio_files_left_channel/event_603.72.wav"
#define WAV_FILE_2  "/home/pi/prof_audio_files_right_channel/event_603.72.wav"
#ifndef M_PI 
#define M_PI 3.1415926535 
#endif


int main(int argc, char** argv) {
    char *wav_file, *wav_file_2;
    if (argc == 3) {
        wav_file = argv[1];
        wav_file_2 = argv[2];
    } else if (argc == 1){
        wav_file = WAV_FILE;
        wav_file_2 = WAV_FILE_2;
    } else {
        printf("Usage: ./%s <wav_file> <wav_file2> or ./%s\n", argv[0], argv[0]);
        return 1;
    }

    SNDFILE *file;
    SF_INFO info;
    int num, num_items;
    double *buf, *leftChannel, *rightChannel;
    int f,sr,c; // f == size (num of bytes in each channel)
    int i;
    
    /* Open the WAV file. */
    info.format = 0;
    file = sf_open(wav_file, SFM_READ, &info);
    if (file == NULL) {
        printf("Failed to open the file.\n");
        exit(-1);
    }

    /* Print some of the info, and figure out how much data to read. */
    f = info.frames;
    sr = info.samplerate;
    c = info.channels;
    printf("frames=%d\nsamplerate=%d\nchannels=%d\n", f, sr, c);
  
    num_items = f*c;
    printf("num_items=%d\n",num_items);

    /* Allocate space for the data to be read, then read it. */
    leftChannel = (double *) malloc(num_items*sizeof(double));
    if ( leftChannel == NULL ){
        perror("Memory allocation failed");
        free(leftChannel);
        exit(2);
    }
    num = sf_read_double(file,leftChannel,num_items);
    sf_close(file);
    printf("Read %d items\n",num);

    /* Allocate memory for each channel. */
    file = sf_open(wav_file_2, SFM_READ, &info);
    if (file == NULL) {
        printf("Failed to open the file.\n");
        exit(-1);
    }

    /* Print some of the info, and figure out how much data to read. */
    f = info.frames;
    sr = info.samplerate;
    c = info.channels;
    printf("frames=%d\nsamplerate=%d\nchannels=%d\n", f, sr, c);
  
    num_items = f*c;
    printf("num_items=%d\n",num_items);

    /* Allocate space for the data to be read, then read it. */
    rightChannel = (double *) malloc(num_items*sizeof(double));
    if ( rightChannel == NULL ){
        perror("Memory allocation failed");
        free(rightChannel);
        exit(2);
    }
    num = sf_read_double(file,rightChannel,num_items);
    sf_close(file);
    printf("Read %d items\n",num);
   

    /* At this point, leftChannel and rightChannel contain the separated audio data. */

    // Allocate arrays for FFT input and output
    fftw_complex *in1 = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * f);
    fftw_complex *in2 = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * f);
    fftw_complex *out1 = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * f);
    fftw_complex *out2 = (fftw_complex*) fftw_malloc(sizeof(fftw_complex) * f);

    // Prepare the FFT plans
    fftw_plan p1 = fftw_plan_dft_1d(f, in1, out1, FFTW_FORWARD, FFTW_ESTIMATE);
    fftw_plan p2 = fftw_plan_dft_1d(f, in2, out2, FFTW_FORWARD, FFTW_ESTIMATE);
    fftw_plan p3 = fftw_plan_dft_1d(f, out1, in1, FFTW_BACKWARD, FFTW_ESTIMATE);

    // Copy data into 'in' arrays and execute FFT
    for (int i = 0; i < f; i++) {
        in1[i][0] = leftChannel[i];
        in1[i][1] = 0.0;
        in2[i][0] = rightChannel[i];
        in2[i][1] = 0.0;
    }

    fftw_execute(p1);
    fftw_execute(p2);

    // Perform cross-correlation in frequency domain
    for (int i = 0; i < f; i++) {
        double a = out1[i][0];
        double b = out1[i][1];
        double c = out2[i][0];
        double d = -out2[i][1]; // Complex conjugate of the second channel
        out1[i][0] = a*c - b*d;
        out1[i][1] = a*d + b*c;
    }

    // Inverse FFT to get correlation result
    fftw_execute(p3);

    // Find the delay (peak in the correlation result)
    int delay = 0;
    double max_corr = 0.0;
    for (int i = 0; i < f; i++) {
        double corr = in1[i][0] * in1[i][0] + in1[i][1] * in1[i][1]; // Magnitude squared
        if (corr > max_corr) {
            max_corr = corr;
            delay = i;
        }
    }

    float delay_seconds = delay*1.0/info.samplerate;
    float angle = acos(SPEED_SOUND*delay_seconds/BASELINE);
    angle = angle * 180 / M_PI;
    printf("Delay: %d samples\n", delay);
    printf("Delay: %f seconds\n",  delay_seconds);
    printf("Estimated angle of arrival: %f\n", angle);
    // Clean up
    fftw_destroy_plan(p1);
    fftw_destroy_plan(p2);
    fftw_destroy_plan(p3);
    fftw_free(in1);
    fftw_free(in2);
    fftw_free(out1);
    fftw_free(out2);
    free(leftChannel);
    free(rightChannel);
    return 0;
}

# Classifier Project

This project implements the recording and timestamping of a continuous audio stream using the PortAudio API. 
There are several versions of recording available, the most functionalities are available by running the basis_audio_program_adctimestamps.c file. 
File pa_devs.c lists out the recognized audio hardware of the system, and is recommended to run before any recording to check if your system recognized the Adafruit digital microphone. 

## Prerequisites

- Build each library from the lib directory, and the proceed to build the project. 

## Setup Instructions

### 1. Clone the Project

First, download or clone the project repository to your local machine:

git clone https://github.com/emina-ferzana/audio_project_rpi.git
cd audio_project_rpi
mkdir build
cd build
Cmake ..
make

### 2. Run the Executables

To run the prepared programs, use the following command:

cd build/bin
./name_of_file



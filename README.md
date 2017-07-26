# caer


AER event-based framework, written in C, targeting embedded systems.

# Dependencies:

Linux, MacOS X or Windows (for Windows build instructions see README.Windows)
cmake >= 2.6
gcc >= 5.2 or clang >= 3.6
libcaer >= 2.2.0
mini-xml (mxml) >= 2.7
libuv >= 1.7.5
Boost >= 1.50 (with system, filesystem, program_options)
Optional: libpng >= 1.6 (input/output frame PNG compression)
Optional: tcmalloc >= 2.2 (faster memory allocation)
Optional: allegro5 >= 5.0.11 (visualizer module)
Optional: OpenCV >= 3.1 (cameracalibration, poseestimation modules)

# Installation

1) configure: 

$ cmake <OPTIONS> <MODULES_TO_BUILD> .

The following options are currently supported:
-DUSE_TCMALLOC=1 -- Enables usage of TCMalloc from Google to allocate memory.

The following modules can currently be selected to be built:
-DDVS128=1 -- DVS128 device input.
-DEDVS=1 -- eDVS4337 device input.
-DDAVIS=1 -- DAVIS device input.
-DDYNAPSE=1 -- Dynap-se device input (neuromorphic chip).
-DBAFILTER=1 -- Filter background activity (uncorrelated noise).
-DFRAMEENHANCER=1 -- Demosaic/enhance frames.
-DCAMERACALIBRATION=1 -- Calculate and apply single camera lens calibration.
-DPOSEESTIMATION=1 -- Estimate pose of camera relative to special markers.
-DSTATISTICS=1 -- Print statistics to console.
-DVISUALIZER=1 -- Open windows in which to visualize data.
-DINPUT_FILE=1 -- Get input from an AEDAT file.
-DOUTPUT_FILE=1 -- Write data to an AEDAT 3.X file.
-DINPUT_NETWORK=1 -- Read input from a network stream.
-DOUTPUT_NETWORK=1 -- Send data out via network.
-DROTATE=1 -- Rotate events.
-DMEDIANTRACKER=1 -- Track points of high event activity.
-DRECTANGULARTRACKER=1 -- Track clusters of events.
-DDYNAMICRECTANGULARTRACKER=1 -- Track a variable number of clusters of events.
-DSPIKEFEATURES=1 -- Create frames which represents decay of polarity events.
-DMEANRATEFILTER=1 -- Measure mean rate of spike events.
-DSYNAPSERECONFIG=1 -- Enable Davis240C to Dynap-se mapping 
-DFPGASPIKEGEN=1 -- Enable FPGA spike generator Dynap-se
-DPOISSONSPIKEGEN=1 -- Enable FPGA Poisson spike generator for Dynap-se

To enable all just type:
 cmake -DDVS128=1 -DEDVS=1 -DDAVIS=1 -DDYNAPSE=1 -DBAFILTER=1 -DFRAMEENHANCER=1 -DCAMERACALIBRATION=1  
 -DPOSEESTIMATION=1 -DSTATISTICS=1  -DVISUALIZER=1 -DINPUT_FILE=1 -DOUTPUT_FILE=1 -DINPUT_NETWORK=1  
 -DOUTPUT_NETWORK=1 -DROTATE=1 -DMEDIANTRACKER=1  -DRECTANGULARTRACKER=1 -DDYNAMICRECTANGULARTRACKER=1  
 -DSPIKEFEATURES=1  -DMEANRATEFILTER=1 -DSYNAPSERECONFIG=1 -DFPGASPIKEGEN=1 -DPOISSONSPIKEGEN=1 .

2) build:

$ make

3) install:

$ make install

# Usage

You will need a 'caer-config.xml' file that specifies which and how modules
should be interconnected. A good starting point is 'docs/davis-config.xml', 
or 'docs/dynapse-config.xml', please also read through 'docs/modules.txt' for 
an explanation of the modules system and its configuration syntax.

$ caer-bin (see docs/ for more info on how to use cAER)
$ caer-ctl (command-line run-time control program, optional)



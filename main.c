/*
 * main.c
 *
 *  Created on: Oct 6, 2013
 *      Author: chtekk
 */

#include "main.h"
#include "base/config.h"
#include "base/config_server.h"
#include "base/log.h"
#include "base/mainloop.h"
#include "base/misc.h"
#include "modules/ini/dvs128.h"
#include "modules/ini/davis_fx2.h"
#include "modules/ini/davis_fx3.h"
#include "modules/backgroundactivityfilter/backgroundactivityfilter.h"
#include "modules/statistics/statistics.h"
#include "modules/misc/out/net_tcp_server.h"
#include "modules/misc/out/net_udp.h"

#ifdef ENABLE_VISUALIZER
        #include "modules/visualizer_allegro/visualizer.h"
#endif

#ifdef ENABLE_CAFFEINTERFACE
        #include "modules/caffeinterface/wrapper.h"
#endif

#define MAX_IMG_QTY 8

static bool mainloop_1(void);
static bool mainloop_2(void);

static bool mainloop_1(void) {
    // An eventPacketContainer bundles event packets of different types together,
    // to maintain time-coherence between the different events.
    caerEventPacketContainer container;

    // Input modules grab data from outside sources (like devices, files, ...)
    // and put events into an event packet.
#ifdef DVS128
    container = caerInputDVS128(1);
#endif
#ifdef DAVISFX2
    container = caerInputDAVISFX2(1);
#endif
#ifdef DAVISFX3
    container = caerInputDAVISFX3(1);
#endif

    // Typed EventPackets contain events of a certain type.
    caerSpecialEventPacket special = (caerSpecialEventPacket) caerEventPacketContainerGetEventPacket(container,
                                                                                                     SPECIAL_EVENT);
    caerPolarityEventPacket polarity = (caerPolarityEventPacket) caerEventPacketContainerGetEventPacket(container,
                                                                                                        POLARITY_EVENT);

#if defined(DAVISFX2) || defined(DAVISFX3)
    // Frame and IMU events exist only with DAVIS cameras.
    caerFrameEventPacket frame = (caerFrameEventPacket) caerEventPacketContainerGetEventPacket(container, FRAME_EVENT);
    caerIMU6EventPacket imu = (caerIMU6EventPacket) caerEventPacketContainerGetEventPacket(container, IMU6_EVENT);
#endif

    // Filters process event packets: for example to suppress certain events,
    // like with the Background Activity Filter, which suppresses events that
    // look to be uncorrelated with real scene changes (noise reduction).
    caerBackgroundActivityFilter(2, polarity);

    // Filters can also extract information from event packets: for example
    // to show statistics about the current event-rate.
    caerStatistics(3, (caerEventPacketHeader) polarity, 1000);

#ifdef ENABLE_VISUALIZER
    // A small visualizer exists to show what the output looks like.
    #if defined(DAVISFX2) || defined(DAVISFX3)
        caerVisualizer(4, polarity, frame, imu);
    #else
        caerVisualizer(4, polarity, NULL, NULL);
    #endif
#endif

#ifdef ENABLE_IMAGEGENERATOR
    // save images of accumulated spikes and frames
    int CLASSIFY_IMG_SIZE = 36;
    int DISPLAY_IMG_SIZE = 256;
    int FRAME_W = NULL;
    int FRAME_H = NULL;
    char mainString[16] = "_main.c_";

    /* class_region_sizes:
     * (Not used so far)
     *
     * So far we only classify big faces in the center, that cover almost the whole screen.
     * Instead of classifying only a down-scaled version of the whole quadratic image (created in imagegenerator.c),
     * we might want to cut out some smaller quadratic windows, downscale them to CLASSIFY_IMAGE_SIZE
     * and check whether a face is recognized in that smaller area.
     *
     * class_region_sizes would store the sizes of this cut out sub-images / windows.
     * Could be used to display a different frame based on how big the face was, that was recognized)
     * (assuming only faces in the center are relevant. If other faces should be classified to, one
     * has to add an array with position of face)
     */

    int * class_region_sizes = calloc(sizeof(int),MAX_IMG_QTY);
    if (class_region_sizes == NULL) {
        caerLog(CAER_LOG_ERROR, mainString, "Failed to allocate class_region_sizes.");
        return (false);
    }

    /* classification_results:
     *
     * Stores the result of the classification (true = FACE, false = NON FACE) for each
     * element in file_strings_classify
     */
    double * classification_results = calloc(sizeof(double),MAX_IMG_QTY);
    if (classification_results == NULL) {
        caerLog(CAER_LOG_ERROR, mainString, "Failed to allocate classification_results.");
        return (false);
    }

    /* file_strings_classify:
     *
     * Stores all disk locations of images, which we want to classify (so far only one
     * classify-image per spike-image is generated and put on file_strings_classify[0])
     */
    unsigned char ** file_strings_classify = calloc(sizeof(unsigned char*),MAX_IMG_QTY);
    if (file_strings_classify == NULL) {
        caerLog(CAER_LOG_ERROR, mainString, "Failed to allocate file_strings_classify.");
        return (false);
    }

    /* display_img_ptr:
     *
     * points to the memory location of th image that will be displayed by
     * the imageStreamerVisualizer
     */
    unsigned char ** display_img_ptr = calloc(sizeof(unsigned char*),1);
    if (display_img_ptr == NULL) {
        caerLog(CAER_LOG_ERROR, mainString, "Failed to allocate display_img_ptr.");
        return (false);
    }


    #if defined(DAVISFX2) || defined(DAVISFX3)
        /* frame_img_ptr:
         *
         * (Not used so far.)
         * points to the frame that is generated in imagegenerator.c
         * Could be used if we want to draw something on top of frame based on classification.
         * So far, the frame is drawn by the visualizer module.
         */
        unsigned char ** frame_img_ptr = calloc(sizeof(unsigned char*),1);

        //generate images
        caerImageGenerator(5, polarity, file_strings_classify, (int) MAX_IMG_QTY, CLASSIFY_IMG_SIZE, display_img_ptr, DISPLAY_IMG_SIZE, frame,  frame_img_ptr, &FRAME_W, &FRAME_H);
    #else
        caerImageGenerator(5, polarity, file_strings_classify, (int) MAX_IMG_QTY, CLASSIFY_IMG_SIZE, display_img_ptr, DISPLAY_IMG_SIZE, NULL, NULL, NULL, NULL);
    #endif

#endif

#ifdef ENABLE_NET_STREAM
    // Send polarity packets out via TCP. This is the server mode!
    // External clients connect to cAER, and we send them the data.
    // WARNING: slow clients can dramatically slow this and the whole
    // processing pipeline down!
    caerOutputNetTCPServer(6, 1, polarity); // or (6, 2, polarity, frame) for polarity and frames
    // And also send them via UDP. This is fast, as it doesn't care what is on the other side.
    caerOutputNetUDP(7, 1, polarity); // or (7, 2, polarity, frame) for polarity and frames
#endif


#ifdef ENABLE_CAFFEINTERFACE
    //it also requires image generator
    #ifdef ENABLE_IMAGEGENERATOR
        // this wrapper let you interact with caffe framework
        // for example, we now classify the latest image
        // only run CNN if we have a file to classify
        if(*file_strings_classify != NULL){
            caerCaffeWrapper(8, file_strings_classify, classification_results, (int) MAX_IMG_QTY);
        }
    #endif
#endif

#ifdef ENABLE_IMAGESTREAMERVISUALIZER
    // Open a second window of the OpenGL visualizer
    // display images of accumulated spikes
    //it also requires image generator
    #ifdef ENABLE_IMAGEGENERATOR
        #if defined(DAVISFX2) || defined(DAVISFX3)
            caerImagestreamerVisualizer(9, *display_img_ptr, DISPLAY_IMG_SIZE, classification_results, class_region_sizes, (int) MAX_IMG_QTY);

        #else //without Frames
            caerImagestreamerVisualizer(9, *display_img_ptr, DISPLAY_IMG_SIZE, classificationResults, class_region_sizes, (int) MAX_IMG_QTY);
        #endif

    #endif

#endif

#ifdef ENABLE_IMAGEGENERATOR
    //free all used data structures
    free(class_region_sizes);
    class_region_sizes = NULL;
    free(classification_results);
    classification_results = NULL;

    for (int i = 1; i < MAX_IMG_QTY; ++i){
        if (file_strings_classify[i]!= NULL){
            free(file_strings_classify[i]);
            file_strings_classify[i] = NULL;
        }
    }
    free(file_strings_classify);
    file_strings_classify = NULL;
    free(*display_img_ptr);
    *display_img_ptr = NULL;
    free(display_img_ptr);
    display_img_ptr = NULL;
    free(*frame_img_ptr);
    *frame_img_ptr = NULL;
    free(frame_img_ptr);
    frame_img_ptr = NULL;

#endif
    return (true); // If false is returned, processing of this loop stops.
}

static bool mainloop_2(void) {
    // An eventPacketContainer bundles event packets of different types together,
    // to maintain time-coherence between the different events.
    caerEventPacketContainer container;

    // Input modules grab data from outside sources (like devices, files, ...)
    // and put events into an event packet.
#ifdef DVS128
    container = caerInputDVS128(1);
#endif
#ifdef DAVISFX2
    container = caerInputDAVISFX2(1);
#endif
#ifdef DAVISFX3
    container = caerInputDAVISFX3(1);
#endif

    // Typed EventPackets contain events of a certain type.
    caerPolarityEventPacket polarity = (caerPolarityEventPacket) caerEventPacketContainerGetEventPacket(container,
                                                                                                        POLARITY_EVENT);

    // Filters process event packets: for example to suppress certain events,
    // like with the Background Activity Filter, which suppresses events that
    // look to be uncorrelated with real scene changes (noise reduction).
    caerBackgroundActivityFilter(2, polarity);

#ifdef ENABLE_NET_STREAM
    // Send polarity packets out via TCP.
    caerOutputNetTCPServer(3, 1, polarity);
#endif

    return (true); // If false is returned, processing of this loop stops.
}

int main(int argc, char **argv) {
    // Initialize config storage from file, support command-line overrides.
    // If no init from file needed, pass NULL.
    caerConfigInit("caer-config.xml", argc, argv);

    // Initialize logging sub-system.
    caerLogInit();

    // Initialize visualizer framework (load fonts etc.).
    #ifdef ENABLE_VISUALIZER
        caerVisualizerSystemInit();
    #endif

    // Daemonize the application (run in background).
    // caerDaemonize();

    // Start the configuration server thread for run-time config changes.
    caerConfigServerStart();

    // Finally run the main event processing loops.
    struct caer_mainloop_definition mainLoops[2] = { { 1, &mainloop_1 }, { 2, &mainloop_2 } };
    caerMainloopRun(&mainLoops, 1); // Only start Mainloop 1.

    // After shutting down the mainloops, also shutdown the config server
    // thread if needed.
    caerConfigServerStop();

    return (EXIT_SUCCESS);
}

/*
 * localfiltdownsample.c
 *
 */

#include "base/mainloop.h"
#include "base/module.h"
#include <libcaer/devices/dynapse.h>
#include "modules/ini/dynapse_common.h"

struct HWFilter_state {
	int16_t sourceID;
	// user settings
	bool run;
	bool setCams;
	uint32_t CorrelationThreshold;
	// usb utils
	caerInputDynapseState eventSourceModuleState;
	sshsNode eventSourceConfigNode;
};

typedef struct HWFilter_state *HWFilterState;

static bool caerLocalFiltDownsampleModuleInit(caerModuleData moduleData);
static void caerLocalFiltDownsampleModuleRun(caerModuleData moduleData, caerEventPacketContainer in, caerEventPacketContainer *out);
static void caerLocalFiltDownsampleModuleConfig(caerModuleData moduleData);
static void caerLocalFiltDownsampleModuleExit(caerModuleData moduleData);
static void caerLocalFiltDownsampleModuleReset(caerModuleData moduleData, uint16_t resetCallSourceID);

static struct caer_module_functions caerLocalFiltDownsampleModuleFunctions = { .moduleInit =
	&caerLocalFiltDownsampleModuleInit, .moduleRun = &caerLocalFiltDownsampleModuleRun, .moduleConfig =
	&caerLocalFiltDownsampleModuleConfig, .moduleExit = &caerLocalFiltDownsampleModuleExit, .moduleReset =
	&caerLocalFiltDownsampleModuleReset };

/*
void caerLocalFiltDownsampleModule(uint16_t moduleID, caerSpikeEventPacket spike) {
	caerModuleData moduleData = caerMainloopFindModule(moduleID, "LocalFiltDownsample", CAER_MODULE_PROCESSOR);
	if (moduleData == NULL) {
		return;
	}

	caerModuleSM(&caerLocalFiltDownsampleModuleFunctions, moduleData, sizeof(struct HWFilter_state), 1, spike);
}
*/

static const struct caer_event_stream_in moduleInputs[] = {
	{ .type = SPIKE_EVENT, .number = 1, .readOnly = true}
};


static const struct caer_module_info moduleInfo = {
	.version = 1, .name = "LocalFiltDownsample",
	.description = "Davis240C to dynapse processor mapping with FPGA local support filter and downsampling",
	.type = CAER_MODULE_OUTPUT,
	.memSize = sizeof(struct HWFilter_state),
	.functions = &caerLocalFiltDownsampleModuleFunctions,
	.inputStreams = moduleInputs,
	.inputStreamsSize = CAER_EVENT_STREAM_IN_SIZE(moduleInputs),
	.outputStreams = NULL,
	.outputStreamsSize = NULL
};

caerModuleInfo caerModuleGetInfo(void) {
    return (&moduleInfo);
}

static bool caerLocalFiltDownsampleModuleInit(caerModuleData moduleData) {

	HWFilterState state = moduleData->moduleState;

	int16_t *inputs = caerMainloopGetModuleInputIDs(moduleData->moduleID, NULL);
	if (inputs == NULL) {
		return (false);
	}

	state->sourceID = inputs[0];
	free(inputs);

	state->eventSourceModuleState = caerMainloopGetSourceState(U16T(state->sourceID));

	// create parameters
	sshsNodeCreateBool(moduleData->moduleNode, "Run", false, SSHS_FLAGS_NORMAL, "Start/Stop running");
	sshsNodeCreateBool(moduleData->moduleNode, "setCams", false, SSHS_FLAGS_NORMAL, "Program cams with predefined mapping for visualization");
	sshsNodeCreateInt(moduleData->moduleNode, "CorrelationThreshold", 10000, 0, INT32_MAX-1, SSHS_FLAGS_NORMAL, "Local support threshold in us");

	// update node state
	state->run = sshsNodeGetBool(moduleData->moduleNode, "Run");
	state->setCams = sshsNodeGetBool(moduleData->moduleNode, "setCams");
	state->CorrelationThreshold = (uint32_t)sshsNodeGetInt(moduleData->moduleNode, "CorrelationThreshold");


	// Add config listeners last - let's the user interact with the parameter -
	sshsNodeAddAttributeListener(moduleData->moduleNode, moduleData, &caerModuleConfigDefaultListener);

	// Nothing that can fail here.
	return (true);
}

static void caerLocalFiltDownsampleModuleRun(caerModuleData moduleData, caerEventPacketContainer in, caerEventPacketContainer *out) {

	// Interpret variable arguments (same as above in main function).
	caerSpikeEventPacket spike = (caerSpikeEventPacketConst) caerEventPacketContainerFindEventPacketByTypeConst(in, SPIKE_EVENT);

	// Only process packets with content.
	if (spike == NULL) {
		return;
	}

	HWFilterState state = moduleData->moduleState;

  	// now we can do crazy processing etc..
	// first find out which one is the module producing the spikes. and get usb handle
	// --- start  usb handle / from spike event source id
	/*
	int sourceID = caerEventPacketHeaderGetEventSource(&spike->packetHeader);
	state->eventSourceModuleState = caerMainloopGetSourceState(U16T(sourceID));
	state->eventSourceConfigNode = caerMainloopGetSourceNode(U16T(sourceID));
	if(state->eventSourceModuleState == NULL || state->eventSourceConfigNode == NULL){
		return;
	}
	caerInputDynapseState stateSource = state->eventSourceModuleState;
	if(stateSource->deviceState == NULL){
		return;
	}
	*/
	// --- end usb handle


}

static void caerLocalFiltDownsampleModuleConfig(caerModuleData moduleData) {
	caerModuleConfigUpdateReset(moduleData);

	HWFilterState state = moduleData->moduleState;

	// this will update parameters, from user input

	bool newRun = sshsNodeGetBool(moduleData->moduleNode, "Run");
	bool newSetCams = sshsNodeGetBool(moduleData->moduleNode, "setCams");
	uint32_t newCorrelationThreshold = (uint32_t)sshsNodeGetInt(moduleData->moduleNode, "CorrelationThreshold");

	if (newRun && !state->run) {
	    state->run = true;
	    caerDeviceConfigSet(state->eventSourceModuleState->deviceState, DYNAPSE_CONFIG_LOCALFILTDOWNSAMPLE, 0, true);
	    int foo = 1337;
	    caerDeviceConfigGet(state->eventSourceModuleState->deviceState, DYNAPSE_CONFIG_LOCALFILTDOWNSAMPLE, 0, &foo);
	    caerLog(CAER_LOG_NOTICE, __func__, "Start running got back %d", foo);
	} else if (!newRun && state->run) {
	    state->run = false;
	    caerDeviceConfigSet(state->eventSourceModuleState->deviceState, DYNAPSE_CONFIG_LOCALFILTDOWNSAMPLE, 0, false);
	    int foo = 1337;
	    caerDeviceConfigGet(state->eventSourceModuleState->deviceState, DYNAPSE_CONFIG_LOCALFILTDOWNSAMPLE, 0, &foo);
	    caerLog(CAER_LOG_NOTICE, __func__, "Start running got back %d", foo);
	}
	    

	if (newSetCams && !state->setCams) {
	    state->setCams = true;
	    caerDeviceConfigSet(state->eventSourceModuleState->deviceState, DYNAPSE_CONFIG_CHIP, DYNAPSE_CONFIG_CHIP_ID, 12);
	    caerLog(CAER_LOG_NOTICE, __func__, "Start cams");
	    for (uint32_t i = 0; i < 1024; i++) {
		caerDynapseWriteCam(state->eventSourceModuleState->deviceState, i%256, i, 0, DYNAPSE_CONFIG_CAMTYPE_F_EXC);

	    }
	    caerLog(CAER_LOG_NOTICE, __func__, "Cams done");
	} else if (!newSetCams && state->setCams) {
	    state->setCams = false;
	}
	
	if (state->CorrelationThreshold != newCorrelationThreshold) {
	    caerDeviceConfigSet(state->eventSourceModuleState->deviceState, DYNAPSE_CONFIG_LOCALFILTDOWNSAMPLE, 3, newCorrelationThreshold);
	    state->CorrelationThreshold = newCorrelationThreshold;
	}
	
	    



}

static void caerLocalFiltDownsampleModuleExit(caerModuleData moduleData) {
	// Remove listener, which can reference invalid memory in userData.
	sshsNodeRemoveAttributeListener(moduleData->moduleNode, moduleData, &caerModuleConfigDefaultListener);

	HWFilterState state = moduleData->moduleState;

	// here we should free memory and other shutdown procedures if needed

}

static void caerLocalFiltDownsampleModuleReset(caerModuleData moduleData, uint16_t resetCallSourceID) {
	UNUSED_ARGUMENT(resetCallSourceID);

	HWFilterState state = moduleData->moduleState;

}

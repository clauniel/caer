#include "visualizer.hpp"
#include "base/mainloop.h"
#include "base/module.h"
#include "ext/ringbuffer/ringbuffer.h"
#include "ext/threads_ext.h"
#include "ext/resources/LiberationSans-Bold.h"
#include "modules/statistics/statistics.h"

#include "visualizer_handlers.hpp"
#include "visualizer_renderers.hpp"

#include <boost/algorithm/string.hpp>

#include <atomic>
#include <thread>
#include <mutex>

#if defined(OS_LINUX) && OS_LINUX == 1
#include <X11/Xlib.h>
#endif

#define VISUALIZER_REFRESH_RATE 60
#define VISUALIZER_ZOOM_DEF  2.0f
#define VISUALIZER_ZOOM_INC 0.25f
#define VISUALIZER_ZOOM_MIN 0.50f
#define VISUALIZER_ZOOM_MAX 50.0f
#define VISUALIZER_POSITION_X_DEF 40
#define VISUALIZER_POSITION_Y_DEF 40

#define GLOBAL_FONT_SIZE 20 // in pixels
#define GLOBAL_FONT_SPACING 5 // in pixels

// Calculated at system init.
static uint32_t STATISTICS_WIDTH = 0;
static uint32_t STATISTICS_HEIGHT = 0;

// Track system init.
static std::once_flag visualizerSystemIsInitialized;

struct caer_visualizer_state {
	sshsNode eventSourceConfigNode;
	sshsNode visualizerConfigNode;
	uint32_t renderSizeX;
	uint32_t renderSizeY;
	void *renderState; // Reserved for renderers to put their internal state into.
	sf::RenderWindow *renderWindow;
	sf::Font *font;
	std::atomic_bool running;
	std::atomic_bool windowResize;
	std::atomic_bool windowMove;
	RingBuffer dataTransfer;
	std::thread *renderingThread;
	caerVisualizerRendererInfo renderer;
	caerVisualizerEventHandlerInfo eventHandler;
	bool showStatistics;
	struct caer_statistics_state packetStatistics;
	std::atomic_uint_fast32_t packetSubsampleRendering;
	uint32_t packetSubsampleCount;
};

typedef struct caer_visualizer_state *caerVisualizerState;

static bool caerVisualizerInit(caerModuleData moduleData);
static void caerVisualizerExit(caerModuleData moduleData);
static void caerVisualizerRun(caerModuleData moduleData, caerEventPacketContainer in, caerEventPacketContainer *out);
static void caerVisualizerReset(caerModuleData moduleData, int16_t resetCallSourceID);
static void caerVisualizerConfigListener(sshsNode node, void *userData, enum sshs_node_attribute_events event,
	const char *changeKey, enum sshs_node_attr_value_type changeType, union sshs_node_attr_value changeValue);
static void initSystemOnce(caerModuleData moduleData);
static bool initRenderSize(caerModuleData moduleData, int16_t *inputs, size_t inputsSize);
static void initRenderersHandlers(caerModuleData moduleData);
static bool initGraphics(caerModuleData moduleData);
static void exitGraphics(caerModuleData moduleData);
static void updateDisplaySize(caerModuleData moduleData);
static void updateDisplayLocation(caerModuleData moduleData);
static void saveDisplayLocation(caerModuleData moduleData);
static void handleEvents(caerModuleData moduleData);
static void renderScreen(caerModuleData moduleData);
static int renderThread(void *inModuleData);

static const struct caer_module_functions VisualizerFunctions =
	{ .moduleInit = &caerVisualizerInit, .moduleRun = &caerVisualizerRun, .moduleConfig = nullptr, .moduleExit =
		&caerVisualizerExit, .moduleReset = &caerVisualizerReset };

static const struct caer_event_stream_in VisualizerInputs[] = { { .type = -1, .number = -1, .readOnly = true } };

static const struct caer_module_info VisualizerInfo = { .version = 1, .name = "Visualizer", .description =
	"Visualize data in various ways.", .type = CAER_MODULE_OUTPUT, .memSize = sizeof(struct caer_visualizer_state),
	.functions = &VisualizerFunctions, .inputStreams = VisualizerInputs, .inputStreamsSize = CAER_EVENT_STREAM_IN_SIZE(
		VisualizerInputs), .outputStreams = nullptr, .outputStreamsSize = 0, };

caerModuleInfo caerModuleGetInfo(void) {
	return (&VisualizerInfo);
}

static bool caerVisualizerInit(caerModuleData moduleData) {
	caerVisualizerState state = (caerVisualizerState) moduleData->moduleState;

	// Initialize visualizer framework (global font sizes). Do only once per startup!
	std::call_once(visualizerSystemIsInitialized, &initSystemOnce, moduleData);

	// Wait for input to be ready. All inputs, once they are up and running, will
	// have a valid sourceInfo node to query, especially if dealing with data.
	size_t inputsSize;
	int16_t *inputs = caerMainloopGetModuleInputIDs(moduleData->moduleID, &inputsSize);
	if (inputs == nullptr) {
		return (false);
	}

	sshsNodeCreate(moduleData->moduleNode, "renderer", "None", 0, 100, SSHS_FLAGS_NORMAL,
		"Renderer to use to generate content.");
	sshsNodeRemoveAttribute(moduleData->moduleNode, "rendererListOptions", SSHS_STRING);
	sshsNodeCreate(moduleData->moduleNode, "rendererListOptions", caerVisualizerRendererListOptionsString, 0, 200,
		SSHS_FLAGS_READ_ONLY, "List of available renderers.");
	sshsNodeCreate(moduleData->moduleNode, "eventHandler", "None", 0, 100, SSHS_FLAGS_NORMAL,
		"Event handler to handle mouse and keyboard events.");
	sshsNodeRemoveAttribute(moduleData->moduleNode, "eventHandlerListOptions", SSHS_STRING);
	sshsNodeCreate(moduleData->moduleNode, "eventHandlerListOptions", caerVisualizerEventHandlerListOptionsString, 0,
		200, SSHS_FLAGS_READ_ONLY, "List of available event handlers.");

	sshsNodeCreateInt(moduleData->moduleNode, "subsampleRendering", 1, 1, 100000, SSHS_FLAGS_NORMAL,
		"Speed-up rendering by only taking every Nth EventPacketContainer to render.");
	sshsNodeCreateBool(moduleData->moduleNode, "showStatistics", true, SSHS_FLAGS_NORMAL,
		"Show useful statistics below content (bottom of window).");
	sshsNodeCreateFloat(moduleData->moduleNode, "zoomFactor", VISUALIZER_ZOOM_DEF, VISUALIZER_ZOOM_MIN,
	VISUALIZER_ZOOM_MAX, SSHS_FLAGS_NORMAL, "Content zoom factor.");
	sshsNodeCreateInt(moduleData->moduleNode, "windowPositionX", VISUALIZER_POSITION_X_DEF, 0, UINT16_MAX,
		SSHS_FLAGS_NORMAL, "Position of window on screen (X coordinate).");
	sshsNodeCreateInt(moduleData->moduleNode, "windowPositionY", VISUALIZER_POSITION_Y_DEF, 0, UINT16_MAX,
		SSHS_FLAGS_NORMAL, "Position of window on screen (Y coordinate).");

	// Initialize visualizer. Needs size information from the source.
	if (!initRenderSize(moduleData, inputs, inputsSize)) {
		caerModuleLog(moduleData, CAER_LOG_ERROR, "Failed to initialize render sizes from source.");
		return (false);
	}

	initRenderersHandlers(moduleData);

	state->visualizerConfigNode = moduleData->moduleNode;
	state->eventSourceConfigNode = caerMainloopGetSourceNode(inputs[0]);

	state->packetSubsampleRendering.store(U32T(sshsNodeGetInt(moduleData->moduleNode, "subsampleRendering")));

	// Enable packet statistics.
	if (!caerStatisticsStringInit(&state->packetStatistics)) {
		caerModuleLog(moduleData, CAER_LOG_ERROR, "Failed to initialize statistics string.");
		return (false);
	}

	// Initialize ring-buffer to transfer data to render thread.
	state->dataTransfer = ringBufferInit(64);
	if (state->dataTransfer == nullptr) {
		caerStatisticsStringExit(&state->packetStatistics);

		caerModuleLog(moduleData, CAER_LOG_ERROR, "Failed to initialize transfer ring-buffer.");
		return (false);
	}

	// Initialize graphics on main thread.
	// On OS X, creation (and destruction) of the window, as well as its event
	// handling must happen on the main thread. Only drawing can be separate.
	if (!initGraphics(moduleData)) {
		ringBufferFree(state->dataTransfer);
		caerStatisticsStringExit(&state->packetStatistics);

		caerModuleLog(moduleData, CAER_LOG_ERROR, "Failed to initialize rendering window.");
		return (false);
	}

	// Disable OpenGL context to pass it to thread.
	state->renderWindow->setActive(false);

	// Start separate rendering thread. Decouples presentation from
	// data processing and preparation. Communication over ring-buffer.
	state->running.store(true);

	try {
		state->renderingThread = new std::thread(&renderThread, moduleData);
	}
	catch (const std::system_error &ex) {
		exitGraphics(moduleData);
		ringBufferFree(state->dataTransfer);
		caerStatisticsStringExit(&state->packetStatistics);

		caerModuleLog(moduleData, CAER_LOG_ERROR, "Failed to start rendering thread. Error: '%s' (%d).", ex.what(),
			ex.code().value());
		return (false);
	}

	// Add config listeners last, to avoid having them dangling if Init doesn't succeed.
	sshsNodeAddAttributeListener(moduleData->moduleNode, state, &caerVisualizerConfigListener);

	caerModuleLog(moduleData, CAER_LOG_DEBUG, "Initialized successfully.");

	return (true);
}

static void caerVisualizerExit(caerModuleData moduleData) {
	caerVisualizerState state = (caerVisualizerState) moduleData->moduleState;

	// Remove listener, which can reference invalid memory in userData.
	sshsNodeRemoveAttributeListener(moduleData->moduleNode, state, &caerVisualizerConfigListener);

	// Shut down rendering thread and wait on it to finish.
	state->running.store(false);

	try {
		state->renderingThread->join();
	}
	catch (const std::system_error &ex) {
		// This should never happen!
		caerModuleLog(moduleData, CAER_LOG_CRITICAL, "Failed to join rendering thread. Error: '%s' (%d).", ex.what(),
			ex.code().value());
	}

	delete state->renderingThread;

	// Shutdown graphics on main thread.
	// On OS X, creation (and destruction) of the window, as well as its event
	// handling must happen on the main thread. Only drawing can be separate.
	exitGraphics(moduleData);

	// Now clean up the ring-buffer and its contents.
	caerEventPacketContainer container;
	while ((container = (caerEventPacketContainer) ringBufferGet(state->dataTransfer)) != nullptr) {
		caerEventPacketContainerFree(container);
	}

	ringBufferFree(state->dataTransfer);

	// Then the statistics string.
	caerStatisticsStringExit(&state->packetStatistics);

	caerModuleLog(moduleData, CAER_LOG_DEBUG, "Exited successfully.");
}

static void caerVisualizerRun(caerModuleData moduleData, caerEventPacketContainer in, caerEventPacketContainer *out) {
	UNUSED_ARGUMENT(out);

	caerVisualizerState state = (caerVisualizerState) moduleData->moduleState;

	// Handle events on main thread, always.
	// On OS X, creation (and destruction) of the window, as well as its event
	// handling must happen on the main thread. Only drawing can be separate.
	handleEvents(moduleData);

	// Without a packet container with events, we cannot render anything.
	if (in == nullptr || caerEventPacketContainerGetEventsNumber(in) == 0) {
		return;
	}

	// Keep statistics up-to-date with all events, always.
	CAER_EVENT_PACKET_CONTAINER_ITERATOR_START(in)
			caerStatisticsStringUpdate(caerEventPacketContainerIteratorElement, &state->packetStatistics);
		CAER_EVENT_PACKET_CONTAINER_ITERATOR_END

		// Only render every Nth container (or packet, if using standard visualizer).
	state->packetSubsampleCount++;

	if (state->packetSubsampleCount >= state->packetSubsampleRendering.load(std::memory_order_relaxed)) {
		state->packetSubsampleCount = 0;
	}
	else {
		return;
	}

	if (ringBufferFull(state->dataTransfer)) {
		caerModuleLog(moduleData, CAER_LOG_INFO, "Transfer ring-buffer full.");
		return;
	}

	caerEventPacketContainer containerCopy = caerEventPacketContainerCopyAllEvents(in);
	if (containerCopy == nullptr) {
		caerModuleLog(moduleData, CAER_LOG_ERROR, "Failed to copy event packet container for rendering.");
		return;
	}

	// Will always succeed because of full check above.
	ringBufferPut(state->dataTransfer, containerCopy);
}

static void caerVisualizerReset(caerModuleData moduleData, int16_t resetCallSourceID) {
	UNUSED_ARGUMENT(resetCallSourceID);

	caerVisualizerState state = (caerVisualizerState) moduleData->moduleState;

	// Reset statistics and counters.
	caerStatisticsStringReset(&state->packetStatistics);
	state->packetSubsampleCount = 0;
}

static void caerVisualizerConfigListener(sshsNode node, void *userData, enum sshs_node_attribute_events event,
	const char *changeKey, enum sshs_node_attr_value_type changeType, union sshs_node_attr_value changeValue) {
	UNUSED_ARGUMENT(node);

	caerVisualizerState state = (caerVisualizerState) userData;

	if (event == SSHS_ATTRIBUTE_MODIFIED) {
		if (changeType == SSHS_FLOAT && caerStrEquals(changeKey, "zoomFactor")) {
			// Set resize flag.
			state->windowResize.store(true);
		}
		else if (changeType == SSHS_BOOL && caerStrEquals(changeKey, "showStatistics")) {
			// Set resize flag. This will then also update the showStatistics flag, ensuring
			// statistics are never shown without the screen having been properly resized first.
			state->windowResize.store(true);
		}
		else if (changeType == SSHS_INT && caerStrEquals(changeKey, "subsampleRendering")) {
			state->packetSubsampleRendering.store(U32T(changeValue.iint));
		}
		else if (changeType == SSHS_INT && caerStrEquals(changeKey, "windowPositionX")) {
			// Set move flag.
			state->windowMove.store(true);
		}
		else if (changeType == SSHS_INT && caerStrEquals(changeKey, "windowPositionY")) {
			// Set move flag.
			state->windowMove.store(true);
		}
	}
}

static void initSystemOnce(caerModuleData moduleData) {
	// Call XInitThreads() on Linux.
#if defined(OS_LINUX) && OS_LINUX == 1
	XInitThreads();
#endif

	// Determine biggest possible statistics string.
	size_t maxStatStringLength = (size_t) snprintf(nullptr, 0, CAER_STATISTICS_STRING_TOTAL, UINT64_MAX);

	char maxStatString[maxStatStringLength + 1];
	snprintf(maxStatString, maxStatStringLength + 1, CAER_STATISTICS_STRING_TOTAL, UINT64_MAX);
	maxStatString[maxStatStringLength] = '\0';

	// Load statistics font into memory.
	sf::Font font;
	if (!font.loadFromMemory(LiberationSans_Bold_ttf, LiberationSans_Bold_ttf_len)) {
		caerModuleLog(moduleData, CAER_LOG_ERROR, "Failed to load font for system init.");
		return;
	}

	// Determine statistics string width.
	sf::Text maxStatText(maxStatString, font, GLOBAL_FONT_SIZE);
	STATISTICS_WIDTH = (2 * GLOBAL_FONT_SPACING) + U32T(maxStatText.getLocalBounds().width);

	STATISTICS_HEIGHT = (3 * GLOBAL_FONT_SPACING) + (2 * U32T(maxStatText.getLocalBounds().height));
}

static bool initRenderSize(caerModuleData moduleData, int16_t *inputs, size_t inputsSize) {
	caerVisualizerState state = (caerVisualizerState) moduleData->moduleState;

	// Default sizes if nothing else is specified in sourceInfo node.
	uint32_t sizeX = 32;
	uint32_t sizeY = 32;
	int16_t sourceID = -1;

	// Search for biggest sizes amongst all event packets.
	for (size_t i = 0; i < inputsSize; i++) {
		// Get size information from source.
		sourceID = inputs[i];

		sshsNode sourceInfoNode = caerMainloopGetSourceInfo(sourceID);
		if (sourceInfoNode == nullptr) {
			return (false);
		}

		// Default sizes if nothing else is specified in sourceInfo node.
		uint32_t packetSizeX = 0;
		uint32_t packetSizeY = 0;

		// Get sizes from sourceInfo node. visualizer prefix takes precedence,
		// for APS and DVS images, alternative prefixes are provided, as well
		// as for generic data visualization.
		if (sshsNodeAttributeExists(sourceInfoNode, "visualizerSizeX", SSHS_SHORT)) {
			packetSizeX = U32T(sshsNodeGetShort(sourceInfoNode, "visualizerSizeX"));
			packetSizeY = U32T(sshsNodeGetShort(sourceInfoNode, "visualizerSizeY"));
		}
		else if (sshsNodeAttributeExists(sourceInfoNode, "dataSizeX", SSHS_SHORT)) {
			packetSizeX = U32T(sshsNodeGetShort(sourceInfoNode, "dataSizeX"));
			packetSizeY = U32T(sshsNodeGetShort(sourceInfoNode, "dataSizeY"));
		}

		if (packetSizeX > sizeX) {
			sizeX = packetSizeX;
		}

		if (packetSizeY > sizeY) {
			sizeY = packetSizeY;
		}
	}

	// Set X/Y sizes.
	state->renderSizeX = sizeX;
	state->renderSizeY = sizeY;

	return (true);
}

static void initRenderersHandlers(caerModuleData moduleData) {
	caerVisualizerState state = (caerVisualizerState) moduleData->moduleState;

	// Standard renderer is the NULL renderer.
	state->renderer = &caerVisualizerRendererList[0];

	// Search for renderer in list.
	const std::string rendererChoice = sshsNodeGetStdString(moduleData->moduleNode, "renderer");

	for (size_t i = 0; i < (sizeof(caerVisualizerRendererList) / sizeof(struct caer_visualizer_renderer_info)); i++) {
		if (rendererChoice == caerVisualizerRendererList[i].name) {
			state->renderer = &caerVisualizerRendererList[i];
			break;
		}
	}

	// Standard event handler is the NULL event handler.
	state->eventHandler = &caerVisualizerEventHandlerList[0];

	// Search for event handler in list.
	const std::string eventHandlerChoice = sshsNodeGetStdString(moduleData->moduleNode, "eventHandler");

	for (size_t i = 0; i < (sizeof(caerVisualizerEventHandlerList) / sizeof(struct caer_visualizer_event_handler_info));
		i++) {
		if (eventHandlerChoice == caerVisualizerEventHandlerList[i].name) {
			state->eventHandler = &caerVisualizerEventHandlerList[i];
			break;
		}
	}
}

static bool initGraphics(caerModuleData moduleData) {
	caerVisualizerState state = (caerVisualizerState) moduleData->moduleState;

	// Create OpenGL context. Depending on flag, either an OpenGL 2.1
	// default (compatibility) context, so it can be used with SFML graphics,
	// or an OpenGL 3.3 context with core profile, so it can do 3D everywhere,
	// even on MacOS X where newer OpenGL's only support the core profile.
	sf::ContextSettings openGLSettings;

	openGLSettings.depthBits = 24;
	openGLSettings.stencilBits = 8;

	if (state->renderer->needsOpenGL3) {
		openGLSettings.majorVersion = 3;
		openGLSettings.minorVersion = 3;
		openGLSettings.attributeFlags = sf::ContextSettings::Core;
	}
	else {
		openGLSettings.majorVersion = 2;
		openGLSettings.minorVersion = 1;
		openGLSettings.attributeFlags = sf::ContextSettings::Default;
	}

	// Create display window and set its title.
	state->renderWindow = new sf::RenderWindow(sf::VideoMode(state->renderSizeX, state->renderSizeY),
		moduleData->moduleSubSystemString, sf::Style::Titlebar | sf::Style::Close, openGLSettings);
	if (state->renderWindow == nullptr) {
		caerModuleLog(moduleData, CAER_LOG_ERROR,
			"Failed to create display window with sizeX=%" PRIu32 ", sizeY=%" PRIu32 ".", state->renderSizeX,
			state->renderSizeY);
		return (false);
	}

	// Set scale transform for display window, update sizes.
	updateDisplaySize(moduleData);

	// Set window position.
	updateDisplayLocation(moduleData);

	// Load font here to have it always available on request.
	state->font = new sf::Font();
	if (state->font == nullptr) {
		caerModuleLog(moduleData, CAER_LOG_WARNING,
			"Failed to create display font. Text rendering will not be possible.");
	}
	else {
		if (!state->font->loadFromMemory(LiberationSans_Bold_ttf, LiberationSans_Bold_ttf_len)) {
			caerModuleLog(moduleData, CAER_LOG_WARNING,
				"Failed to load display font. Text rendering will not be possible.");

			delete state->font;
			state->font = nullptr;
		}
	}

	return (true);
}

static void exitGraphics(caerModuleData moduleData) {
	caerVisualizerState state = (caerVisualizerState) moduleData->moduleState;

	// Save visualizer window location in config.
	saveDisplayLocation(moduleData);

	// Close rendering window and free memory.
	state->renderWindow->close();

	delete state->font;
	delete state->renderWindow;
}

static void updateDisplaySize(caerModuleData moduleData) {
	caerVisualizerState state = (caerVisualizerState) moduleData->moduleState;

	state->showStatistics = sshsNodeGetBool(moduleData->moduleNode, "showStatistics");
	float zoomFactor = sshsNodeGetFloat(moduleData->moduleNode, "zoomFactor");

	sf::Vector2u newRenderWindowSize(state->renderSizeX, state->renderSizeY);

	// When statistics are turned on, we need to add some space to the
	// X axis for displaying the whole line and the Y axis for spacing.
	if (state->showStatistics) {
		if (STATISTICS_WIDTH > newRenderWindowSize.x) {
			newRenderWindowSize.x = STATISTICS_WIDTH;
		}

		newRenderWindowSize.y += STATISTICS_HEIGHT;
	}

	// Set view size to render area.
	// TODO: bug with statistics, lower text line cut off.
	state->renderWindow->setView(sf::View(sf::FloatRect(0, 0, newRenderWindowSize.x, newRenderWindowSize.y)));

	// Apply zoom to all content.
	newRenderWindowSize.x *= zoomFactor;
	newRenderWindowSize.y *= zoomFactor;

	// Set window size to zoomed area.
	state->renderWindow->setSize(newRenderWindowSize);
}

static void updateDisplayLocation(caerModuleData moduleData) {
	caerVisualizerState state = (caerVisualizerState) moduleData->moduleState;

	// Set current position to what is in configuration storage.
	const sf::Vector2i newPos(sshsNodeGetInt(moduleData->moduleNode, "windowPositionX"),
		sshsNodeGetInt(moduleData->moduleNode, "windowPositionY"));

	state->renderWindow->setPosition(newPos);
}

static void saveDisplayLocation(caerModuleData moduleData) {
	caerVisualizerState state = (caerVisualizerState) moduleData->moduleState;

	const sf::Vector2i currPos = state->renderWindow->getPosition();

	// Update current position in configuration storage.
	sshsNodePutInt(moduleData->moduleNode, "windowPositionX", currPos.x);
	sshsNodePutInt(moduleData->moduleNode, "windowPositionY", currPos.y);
}

static void handleEvents(caerModuleData moduleData) {
	caerVisualizerState state = (caerVisualizerState) moduleData->moduleState;

	sf::Event event;

	while (state->renderWindow->pollEvent(event)) {
		if (event.type == sf::Event::Closed) {
			// Stop visualizer module on window close.
			sshsNodePutBool(moduleData->moduleNode, "running", false);
		}
		else if (event.type == sf::Event::KeyPressed || event.type == sf::Event::KeyReleased
			|| event.type == sf::Event::TextEntered) {
			// React to key presses, but only if they came from the corresponding display.
			if (event.type == sf::Event::KeyPressed && event.key.code == sf::Keyboard::Key::Up) {
				float currentZoomFactor = sshsNodeGetFloat(moduleData->moduleNode, "zoomFactor");

				currentZoomFactor += VISUALIZER_ZOOM_INC;

				// Clip zoom factor.
				if (currentZoomFactor > VISUALIZER_ZOOM_MAX) {
					currentZoomFactor = VISUALIZER_ZOOM_MAX;
				}

				sshsNodePutFloat(moduleData->moduleNode, "zoomFactor", currentZoomFactor);
			}
			else if (event.type == sf::Event::KeyPressed && event.key.code == sf::Keyboard::Key::Down) {
				float currentZoomFactor = sshsNodeGetFloat(moduleData->moduleNode, "zoomFactor");

				currentZoomFactor -= VISUALIZER_ZOOM_INC;

				// Clip zoom factor.
				if (currentZoomFactor < VISUALIZER_ZOOM_MIN) {
					currentZoomFactor = VISUALIZER_ZOOM_MIN;
				}

				sshsNodePutFloat(moduleData->moduleNode, "zoomFactor", currentZoomFactor);
			}
			else if (event.type == sf::Event::KeyPressed && event.key.code == sf::Keyboard::Key::W) {
				int32_t currentSubsampling = sshsNodeGetInt(moduleData->moduleNode, "subsampleRendering");

				currentSubsampling--;

				// Clip subsampling factor.
				if (currentSubsampling < 1) {
					currentSubsampling = 1;
				}

				sshsNodePutInt(moduleData->moduleNode, "subsampleRendering", currentSubsampling);
			}
			else if (event.type == sf::Event::KeyPressed && event.key.code == sf::Keyboard::Key::E) {
				int32_t currentSubsampling = sshsNodeGetInt(moduleData->moduleNode, "subsampleRendering");

				currentSubsampling++;

				// Clip subsampling factor.
				if (currentSubsampling > 100000) {
					currentSubsampling = 100000;
				}

				sshsNodePutInt(moduleData->moduleNode, "subsampleRendering", currentSubsampling);
			}
			else if (event.type == sf::Event::KeyPressed && event.key.code == sf::Keyboard::Key::Q) {
				bool currentShowStatistics = sshsNodeGetBool(moduleData->moduleNode, "showStatistics");

				sshsNodePutBool(moduleData->moduleNode, "showStatistics", !currentShowStatistics);
			}
			else {
				// Forward event to user-defined event handler.
				if (state->eventHandler->eventHandler != nullptr) {
					(*state->eventHandler->eventHandler)((caerVisualizerPublicState) state, event);
				}
			}
		}
		else if (event.type == sf::Event::MouseButtonPressed || event.type == sf::Event::MouseButtonReleased
			|| event.type == sf::Event::MouseWheelScrolled || event.type == sf::Event::MouseEntered
			|| event.type == sf::Event::MouseLeft || event.type == sf::Event::MouseMoved) {
			if (event.type == sf::Event::MouseWheelScrolled && event.mouseWheelScroll.delta > 0) {
				float currentZoomFactor = sshsNodeGetFloat(moduleData->moduleNode, "zoomFactor");

				currentZoomFactor += (VISUALIZER_ZOOM_INC * (float) event.mouseWheelScroll.delta);

				// Clip zoom factor.
				if (currentZoomFactor > VISUALIZER_ZOOM_MAX) {
					currentZoomFactor = VISUALIZER_ZOOM_MAX;
				}

				sshsNodePutFloat(moduleData->moduleNode, "zoomFactor", currentZoomFactor);
			}
			else if (event.type == sf::Event::MouseWheelScrolled && event.mouseWheelScroll.delta < 0) {
				float currentZoomFactor = sshsNodeGetFloat(moduleData->moduleNode, "zoomFactor");

				// Add because delta is negative for scroll-down.
				currentZoomFactor += (VISUALIZER_ZOOM_INC * (float) event.mouseWheelScroll.delta);

				// Clip zoom factor.
				if (currentZoomFactor < VISUALIZER_ZOOM_MIN) {
					currentZoomFactor = VISUALIZER_ZOOM_MIN;
				}

				sshsNodePutFloat(moduleData->moduleNode, "zoomFactor", currentZoomFactor);
			}
			else {
				// Forward event to user-defined event handler.
				if (state->eventHandler->eventHandler != nullptr) {
					(*state->eventHandler->eventHandler)((caerVisualizerPublicState) state, event);
				}
			}
		}
	}
}

static void renderScreen(caerModuleData moduleData) {
	caerVisualizerState state = (caerVisualizerState) moduleData->moduleState;

	// TODO: rethink this, implement max FPS control, FPS count,
	// and multiple render passes per displayed frame.
	caerEventPacketContainer container = (caerEventPacketContainer) ringBufferGet(state->dataTransfer);

	repeat: if (container != nullptr) {
		// Are there others? Only render last one, to avoid getting backed up!
		caerEventPacketContainer container2 = (caerEventPacketContainer) ringBufferGet(state->dataTransfer);

		if (container2 != nullptr) {
			caerEventPacketContainerFree(container);
			container = container2;
			goto repeat;
		}
	}

	bool drewSomething = false;

	if (container != nullptr) {
		// Update render window with new content. (0, 0) is upper left corner.
		// NULL renderer is supported and simply does nothing (black screen).
		if (state->renderer != nullptr) {
			drewSomething = (*state->renderer->renderer)((caerVisualizerPublicState) state, container);
		}

		// Free packet container copy.
		caerEventPacketContainerFree(container);
	}

	// Handle display resize (zoom and statistics).
	if (state->windowResize.load(std::memory_order_relaxed)) {
		state->windowResize.store(false);

		// Update statistics flag and resize display appropriately.
		updateDisplaySize(moduleData);
	}

	// Handle display move.
	if (state->windowMove.load(std::memory_order_relaxed)) {
		state->windowMove.store(false);

		// Move display location appropriately.
		updateDisplayLocation(moduleData);
	}

	// Render content to display.
	if (drewSomething) {
		// Render statistics string.
		// TODO: implement for OpenGL 3.3 too, using some text rendering library.
		bool doStatistics = (state->showStatistics && state->font != nullptr && !state->renderer->needsOpenGL3);

		if (doStatistics) {
			// Split statistics string in two to use less horizontal space.
			// Put it below the normal render region, so people can access from
			// (0,0) to (x-1,y-1) normally without fear of overwriting statistics.
			sf::Text totalEventsText(state->packetStatistics.currentStatisticsStringTotal, *state->font,
			GLOBAL_FONT_SIZE);
			totalEventsText.setFillColor(sf::Color::White);
			totalEventsText.setPosition(GLOBAL_FONT_SPACING, state->renderSizeY + GLOBAL_FONT_SPACING);
			state->renderWindow->draw(totalEventsText);

			sf::Text validEventsText(state->packetStatistics.currentStatisticsStringValid, *state->font,
			GLOBAL_FONT_SIZE);
			validEventsText.setFillColor(sf::Color::White);
			validEventsText.setPosition(GLOBAL_FONT_SPACING,
				state->renderSizeY + (2 * GLOBAL_FONT_SPACING) + GLOBAL_FONT_SIZE);
			state->renderWindow->draw(validEventsText);
		}

		// Draw to screen.
		state->renderWindow->display();

		// Reset window to all black for next rendering pass.
		state->renderWindow->clear(sf::Color::Black);
	}
}

static int renderThread(void *inModuleData) {
	if (inModuleData == nullptr) {
		return (thrd_error);
	}

	caerModuleData moduleData = (caerModuleData) inModuleData;
	caerVisualizerState state = (caerVisualizerState) moduleData->moduleState;

	// Set thread name.
	thrd_set_name(moduleData->moduleSubSystemString);

	// On OS X, creation (and destruction) of the window, as well as its event
	// handling must happen on the main thread. Only drawing can be separate.
	state->renderWindow->setActive(true);

	// Initialize GLEW. glewInit() should be called after every context change,
	// since we have one context per visualizer, always active only in this one
	// rendering thread, we can just do it here once and always be fine.
	GLenum res = glewInit();
	if (res != GLEW_OK) {
		caerModuleLog(moduleData, CAER_LOG_ERROR, "Failed to initialize GLEW, error: %s.", glewGetErrorString(res));
		return (thrd_error);
	}

	// Initialize window by clearing it to all black.
	state->renderWindow->clear(sf::Color::Black);
	state->renderWindow->display();

	while (state->running.load(std::memory_order_relaxed)) {
		renderScreen(moduleData);
	}

	return (thrd_success);
}

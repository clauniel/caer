#include "sshs_internal.h"
#include "ext/uthash/uthash.h"
#include "ext/uthash/utlist.h"
#include <float.h>

struct sshs_node {
	char *name;
	char *path;
	sshsNode parent;
	sshsNode children;
	sshsNodeAttr attributes;
	sshsNodeListener nodeListeners;
	sshsNodeAttrListener attrListeners;
	mtx_shared_t traversal_lock;
	mtx_t node_lock;
	UT_hash_handle hh;
};

struct sshs_node_attr {
	UT_hash_handle hh;
	union sshs_node_attr_range min;
	union sshs_node_attr_range max;
	int flags;
	char *description;
	union sshs_node_attr_value value;
	enum sshs_node_attr_value_type value_type;
	char key[];
};

struct sshs_node_listener {
	void (*node_changed)(sshsNode node, void *userData, enum sshs_node_node_events event, const char *changeNode);
	void *userData;
	sshsNodeListener next;
};

struct sshs_node_attr_listener {
	void (*attribute_changed)(sshsNode node, void *userData, enum sshs_node_attribute_events event,
		const char *changeKey, enum sshs_node_attr_value_type changeType, union sshs_node_attr_value changeValue);
	void *userData;
	sshsNodeAttrListener next;
};

static void sshsNodeDestroy(sshsNode node);
static int sshsNodeCmp(const void *a, const void *b);
static bool sshsNodeCheckRange(enum sshs_node_attr_value_type type, union sshs_node_attr_value value,
	union sshs_node_attr_range min, union sshs_node_attr_range max);
static void sshsNodeRemoveChild(sshsNode node, const char *childName);
static void sshsNodeRemoveAllChildren(sshsNode node);
static void sshsNodeRemoveSubTree(sshsNode node);
static bool sshsNodeCheckAttributeValueChanged(enum sshs_node_attr_value_type type, union sshs_node_attr_value oldValue,
	union sshs_node_attr_value newValue);
static int sshsNodeAttrCmp(const void *a, const void *b);
static sshsNodeAttr *sshsNodeGetAttributes(sshsNode node, size_t *numAttributes);
static const char *sshsNodeXMLWhitespaceCallback(mxml_node_t *node, int where);
static void sshsNodeToXML(sshsNode node, int outFd, bool recursive);
static mxml_node_t *sshsNodeGenerateXML(sshsNode node, bool recursive);
static mxml_node_t **sshsNodeXMLFilterChildNodes(mxml_node_t *node, const char *nodeName, size_t *numChildren);
static bool sshsNodeFromXML(sshsNode node, int inFd, bool recursive, bool strict);
static void sshsNodeConsumeXML(sshsNode node, mxml_node_t *content, bool recursive);

sshsNode sshsNodeNew(const char *nodeName, sshsNode parent) {
	sshsNode newNode = malloc(sizeof(*newNode));
	SSHS_MALLOC_CHECK_EXIT(newNode);
	memset(newNode, 0, sizeof(*newNode));

	// Allocate full copy of string, so that we control the memory.
	size_t nameLength = strlen(nodeName);
	newNode->name = malloc(nameLength + 1);
	SSHS_MALLOC_CHECK_EXIT(newNode->name);

	// Copy the string.
	strcpy(newNode->name, nodeName);

	newNode->parent = parent;
	newNode->children = NULL;
	newNode->attributes = NULL;
	newNode->nodeListeners = NULL;
	newNode->attrListeners = NULL;

	if (mtx_shared_init(&newNode->traversal_lock) != thrd_success) {
		// Locks are critical for thread-safety.
		char errorMsg[4096];
		snprintf(errorMsg, 4096, "Failed to initialize traversal_lock for node: '%s'.", nodeName);
		(*sshsGetGlobalErrorLogCallback())(errorMsg);
		exit(EXIT_FAILURE);
	}
	if (mtx_init(&newNode->node_lock, mtx_recursive) != thrd_success) {
		// Locks are critical for thread-safety.
		char errorMsg[4096];
		snprintf(errorMsg, 4096, "Failed to initialize node_lock for node: '%s'.", nodeName);
		(*sshsGetGlobalErrorLogCallback())(errorMsg);
		exit(EXIT_FAILURE);
	}

	// Path is based on parent.
	if (parent != NULL) {
		// Either allocate string copy for full path.
		size_t pathLength = strlen(sshsNodeGetPath(parent)) + nameLength + 1; // + 1 for trailing slash
		newNode->path = malloc(pathLength + 1);
		SSHS_MALLOC_CHECK_EXIT(newNode->path);

		// Generate string.
		snprintf(newNode->path, pathLength + 1, "%s%s/", sshsNodeGetPath(parent), nodeName);
	}
	else {
		// Or the root has an empty, constant path.
		newNode->path = malloc(2);
		SSHS_MALLOC_CHECK_EXIT(newNode->path);

		// Generate string.
		strncpy(newNode->path, "/", 2);
	}

	return (newNode);
}

// children, attributes, and listeners must be cleaned up prior to this call.
static void sshsNodeDestroy(sshsNode node) {
	mtx_destroy(&node->node_lock);
	mtx_shared_destroy(&node->traversal_lock);

	free(node->path);
	free(node->name);
	free(node);
}

const char *sshsNodeGetName(sshsNode node) {
	return (node->name);
}

const char *sshsNodeGetPath(sshsNode node) {
	return (node->path);
}

sshsNode sshsNodeGetParent(sshsNode node) {
	return (node->parent);
}

sshsNode sshsNodeAddChild(sshsNode node, const char *childName) {
	mtx_shared_lock_exclusive(&node->traversal_lock);

	// Atomic putIfAbsent: returns null if nothing was there before and the
	// node is the new one, or it returns the old node if already present.
	sshsNode child = NULL, newChild = NULL;
	HASH_FIND_STR(node->children, childName, child);

	if (child == NULL) {
		// Create new child node with appropriate name and parent.
		newChild = sshsNodeNew(childName, node);

		// No node present, let's add it.
		HASH_ADD_KEYPTR(hh, node->children, sshsNodeGetName(newChild), strlen(sshsNodeGetName(newChild)), newChild);
	}

	mtx_shared_unlock_exclusive(&node->traversal_lock);

	// If null was returned, then nothing was in the map beforehand, and
	// thus the new node 'child' is the node that's now in the map.
	if (child == NULL) {
		// Listener support (only on new addition!).
		mtx_lock(&node->node_lock);

		sshsNodeListener l;
		LL_FOREACH(node->nodeListeners, l)
		{
			l->node_changed(node, l->userData, SSHS_CHILD_NODE_ADDED, childName);
		}

		mtx_unlock(&node->node_lock);

		return (newChild);
	}

	return (child);
}

sshsNode sshsNodeGetChild(sshsNode node, const char* childName) {
	mtx_shared_lock_shared(&node->traversal_lock);

	sshsNode child;
	HASH_FIND_STR(node->children, childName, child);

	mtx_shared_unlock_shared(&node->traversal_lock);

	// Either null or an always valid value.
	return (child);
}

static int sshsNodeCmp(const void *a, const void *b) {
	const sshsNode *aa = a;
	const sshsNode *bb = b;

	return (strcmp(sshsNodeGetName(*aa), sshsNodeGetName(*bb)));
}

sshsNode *sshsNodeGetChildren(sshsNode node, size_t *numChildren) {
	mtx_shared_lock_shared(&node->traversal_lock);

	size_t childrenCount = HASH_COUNT(node->children);

	// If none, exit gracefully.
	if (childrenCount == 0) {
		mtx_shared_unlock_shared(&node->traversal_lock);

		*numChildren = 0;
		return (NULL);
	}

	sshsNode *children = malloc(childrenCount * sizeof(*children));
	SSHS_MALLOC_CHECK_EXIT(children);

	size_t i = 0;
	for (sshsNode n = node->children; n != NULL; n = n->hh.next) {
		children[i++] = n;
	}

	mtx_shared_unlock_shared(&node->traversal_lock);

	// Sort by name.
	qsort(children, childrenCount, sizeof(sshsNode), &sshsNodeCmp);

	*numChildren = childrenCount;
	return (children);
}

void sshsNodeAddNodeListener(sshsNode node, void *userData,
	void (*node_changed)(sshsNode node, void *userData, enum sshs_node_node_events event, const char *changeNode)) {
	sshsNodeListener listener = malloc(sizeof(*listener));
	SSHS_MALLOC_CHECK_EXIT(listener);

	listener->node_changed = node_changed;
	listener->userData = userData;

	mtx_lock(&node->node_lock);

	// Search if we don't already have this exact listener, to avoid duplicates.
	bool found = false;

	sshsNodeListener curr;
	LL_FOREACH(node->nodeListeners, curr)
	{
		if (curr->node_changed == node_changed && curr->userData == userData) {
			found = true;
		}
	}

	if (!found) {
		LL_PREPEND(node->nodeListeners, listener);
	}
	else {
		free(listener);
	}

	mtx_unlock(&node->node_lock);
}

void sshsNodeRemoveNodeListener(sshsNode node, void *userData,
	void (*node_changed)(sshsNode node, void *userData, enum sshs_node_node_events event, const char *changeNode)) {
	mtx_lock(&node->node_lock);

	sshsNodeListener curr, curr_tmp;
	LL_FOREACH_SAFE(node->nodeListeners, curr, curr_tmp)
	{
		if (curr->node_changed == node_changed && curr->userData == userData) {
			LL_DELETE(node->nodeListeners, curr);
			free(curr);
		}
	}

	mtx_unlock(&node->node_lock);
}

void sshsNodeRemoveAllNodeListeners(sshsNode node) {
	mtx_lock(&node->node_lock);

	sshsNodeListener curr, curr_tmp;
	LL_FOREACH_SAFE(node->nodeListeners, curr, curr_tmp)
	{
		LL_DELETE(node->nodeListeners, curr);
		free(curr);
	}

	mtx_unlock(&node->node_lock);
}

void sshsNodeAddAttributeListener(sshsNode node, void *userData,
	void (*attribute_changed)(sshsNode node, void *userData, enum sshs_node_attribute_events event,
		const char *changeKey, enum sshs_node_attr_value_type changeType, union sshs_node_attr_value changeValue)) {
	sshsNodeAttrListener listener = malloc(sizeof(*listener));
	SSHS_MALLOC_CHECK_EXIT(listener);

	listener->attribute_changed = attribute_changed;
	listener->userData = userData;

	mtx_lock(&node->node_lock);

	// Search if we don't already have this exact listener, to avoid duplicates.
	bool found = false;

	sshsNodeAttrListener curr;
	LL_FOREACH(node->attrListeners, curr)
	{
		if (curr->attribute_changed == attribute_changed && curr->userData == userData) {
			found = true;
		}
	}

	if (!found) {
		LL_PREPEND(node->attrListeners, listener);
	}
	else {
		free(listener);
	}

	mtx_unlock(&node->node_lock);
}

void sshsNodeRemoveAttributeListener(sshsNode node, void *userData,
	void (*attribute_changed)(sshsNode node, void *userData, enum sshs_node_attribute_events event,
		const char *changeKey, enum sshs_node_attr_value_type changeType, union sshs_node_attr_value changeValue)) {
	mtx_lock(&node->node_lock);

	sshsNodeAttrListener curr, curr_tmp;
	LL_FOREACH_SAFE(node->attrListeners, curr, curr_tmp)
	{
		if (curr->attribute_changed == attribute_changed && curr->userData == userData) {
			LL_DELETE(node->attrListeners, curr);
			free(curr);
		}
	}

	mtx_unlock(&node->node_lock);
}

void sshsNodeRemoveAllAttributeListeners(sshsNode node) {
	mtx_lock(&node->node_lock);

	sshsNodeAttrListener curr, curr_tmp;
	LL_FOREACH_SAFE(node->attrListeners, curr, curr_tmp)
	{
		LL_DELETE(node->attrListeners, curr);
		free(curr);
	}

	mtx_unlock(&node->node_lock);
}

void sshsNodeTransactionLock(sshsNode node) {
	mtx_lock(&node->node_lock);
}

void sshsNodeTransactionUnlock(sshsNode node) {
	mtx_unlock(&node->node_lock);
}

static bool sshsNodeCheckRange(enum sshs_node_attr_value_type type, union sshs_node_attr_value value,
	union sshs_node_attr_range min, union sshs_node_attr_range max) {
	// Check limits: use integer for all ints, double for float/double,
	// and for strings take the length. Bool has no limits!
	switch (type) {
		case SSHS_BOOL:
			// No check for bool. Always true.
			return (true);

		case SSHS_BYTE:
			return (value.ibyte >= min.i && value.ibyte <= max.i);

		case SSHS_SHORT:
			return (value.ishort >= min.i && value.ishort <= max.i);

		case SSHS_INT:
			return (value.iint >= min.i && value.iint <= max.i);

		case SSHS_LONG:
			return (value.ilong >= min.i && value.ilong <= max.i);

		case SSHS_FLOAT:
			return (value.ffloat >= (float) min.d && value.ffloat <= (float) max.d);

		case SSHS_DOUBLE:
			return (value.ddouble >= min.d && value.ddouble <= max.d);

		case SSHS_STRING: {
			size_t stringLength = strlen(value.string);
			return (stringLength >= (size_t) min.i && stringLength <= (size_t) max.i);
		}

		case SSHS_UNKNOWN:
		default:
			return (false); // UNKNOWN TYPE.
	}
}

static inline void sshsNodeFreeAttribute(sshsNodeAttr attr) {
	// Free attribute's string memory, then attribute itself.
	if (attr->value_type == SSHS_STRING) {
		free(attr->value.string);
	}

	free(attr->description);
	free(attr);
}

void sshsNodeCreateAttribute(sshsNode node, const char *key, enum sshs_node_attr_value_type type,
	union sshs_node_attr_value defaultValue, struct sshs_node_attr_ranges range, int flags, const char *description) {
	// Parse range struct.
	union sshs_node_attr_range minValue = range.min;
	union sshs_node_attr_range maxValue = range.max;

	// Strings are special, their length range goes from 0 to SIZE_MAX, but we
	// have to restrict that to from 0 to INT32_MAX for languages like Java
	// that only support integer string lengths. It's also reasonable.
	if (type == SSHS_STRING) {
		if (minValue.i < 0 || minValue.i > INT32_MAX) {
			char errorMsg[1024];
			snprintf(errorMsg, 1024,
				"sshsNodeCreateAttribute(): attribute '%s' of type 'string' has a minimum range value of '%" PRIi64 "' outside allowed limits. "
				"Please make sure the value is positive, between 0 and %" PRIi32 "!", key, minValue.i, INT32_MAX);

			(*sshsGetGlobalErrorLogCallback())(errorMsg);

			// This is a critical usage error that *must* be fixed!
			exit(EXIT_FAILURE);
		}

		if (maxValue.i < 0 || maxValue.i > INT32_MAX) {
			char errorMsg[1024];
			snprintf(errorMsg, 1024,
				"sshsNodeCreateAttribute(): attribute '%s' of type 'string' has a maximum range value of '%" PRIi64 "' outside allowed limits. "
				"Please make sure the value is positive, between 0 and %" PRIi32 "!", key, maxValue.i, INT32_MAX);

			(*sshsGetGlobalErrorLogCallback())(errorMsg);

			// This is a critical usage error that *must* be fixed!
			exit(EXIT_FAILURE);
		}
	}

	// Check that value conforms to range limits.
	if (!sshsNodeCheckRange(type, defaultValue, minValue, maxValue)) {
		// Fail on wrong default value. Must be within range!
		char errorMsg[1024];
		snprintf(errorMsg, 1024,
			"sshsNodeCreateAttribute(): attribute '%s' of type '%s' has default value '%s' that is out of specified range. "
				"Please make sure the default value is within the given range!", key,
			sshsHelperTypeToStringConverter(type), sshsHelperValueToStringConverter(type, defaultValue));

		(*sshsGetGlobalErrorLogCallback())(errorMsg);

		// This is a critical usage error that *must* be fixed!
		exit(EXIT_FAILURE);
	}

	// Restrict NOTIFY_ONLY flag to booleans only, for button-like behavior.
	if ((flags & SSHS_FLAGS_NOTIFY_ONLY) && type != SSHS_BOOL) {
		// Fail on wrong notify-only flag usage.
		char errorMsg[1024];
		snprintf(errorMsg, 1024, "Attribute '%s' of type '%s' has the NOTIFY_ONLY flag set, but is not of type BOOL. "
			"Only booleans may have this flag set!", key, sshsHelperTypeToStringConverter(type));

		(*sshsGetGlobalErrorLogCallback())(errorMsg);

		// This is a critical usage error that *must* be fixed!
		exit(EXIT_FAILURE);
	}

	size_t keyLength = strlen(key);
	sshsNodeAttr newAttr = malloc(sizeof(*newAttr) + keyLength + 1);
	SSHS_MALLOC_CHECK_EXIT(newAttr);
	memset(newAttr, 0, sizeof(*newAttr));

	if (type == SSHS_STRING) {
		// Make a copy of the string so we own the memory internally.
		char *valueCopy = strdup(defaultValue.string);
		SSHS_MALLOC_CHECK_EXIT(valueCopy);

		newAttr->value.string = valueCopy;
	}
	else {
		newAttr->value = defaultValue;
	}

	newAttr->min = minValue;
	newAttr->max = maxValue;
	newAttr->flags = flags;

	char *descriptionCopy = strdup(description);
	SSHS_MALLOC_CHECK_EXIT(descriptionCopy);
	newAttr->description = descriptionCopy;

	newAttr->value_type = type;
	strcpy(newAttr->key, key);

	size_t fullKeyLength = offsetof(struct sshs_node_attr, key) + keyLength
		+ 1- offsetof(struct sshs_node_attr, value_type);

	mtx_lock(&node->node_lock);

	sshsNodeAttr oldAttr;
	HASH_FIND(hh, node->attributes, &newAttr->value_type, fullKeyLength, oldAttr);

	// Add if not present. Else update value (below).
	if (oldAttr == NULL) {
		HASH_ADD(hh, node->attributes, value_type, fullKeyLength, newAttr);

		// Listener support. Call only on change, which is always the case here.
		sshsNodeAttrListener l;
		LL_FOREACH(node->attrListeners, l)
		{
			l->attribute_changed(node, l->userData, SSHS_ATTRIBUTE_ADDED, key, type, defaultValue);
		}
	}
	else {
		// If value was present, update its range and flags always.
		oldAttr->min = minValue;
		oldAttr->max = maxValue;
		oldAttr->flags = flags;

		free(oldAttr->description);
		oldAttr->description = newAttr->description;
		newAttr->description = NULL;

		// Check if the current value is still fine and within range; if it's
		// not, we replace it with the new one.
		if (!sshsNodeCheckRange(type, oldAttr->value, minValue, maxValue)) {
			// Values really changed, update. Remember to free old string
			// memory, as well as newAttr itself (but not newAttr.value).
			if (type == SSHS_STRING) {
				free(oldAttr->value.string);
			}

			oldAttr->value = newAttr->value;

			free(newAttr);

			// Listener support. Call only on change, which is always the case here.
			sshsNodeAttrListener l;
			LL_FOREACH(node->attrListeners, l)
			{
				l->attribute_changed(node, l->userData, SSHS_ATTRIBUTE_MODIFIED, key, type, defaultValue);
			}
		}
		else {
			// Nothing to update, delete newAttr fully.
			sshsNodeFreeAttribute(newAttr);
		}
	}

	mtx_unlock(&node->node_lock);
}

// Remember to 'mtx_unlock(&node->node_lock);' after this!
static inline sshsNodeAttr sshsNodeFindAttribute(sshsNode node, const char *key, enum sshs_node_attr_value_type type) {
	size_t keyLength = strlen(key);
	size_t fullKeyLength = offsetof(struct sshs_node_attr, key) + keyLength
		+ 1- offsetof(struct sshs_node_attr, value_type);

	uint8_t searchKey[fullKeyLength];

	memcpy(searchKey, &type, sizeof(enum sshs_node_attr_value_type));
	strcpy((char *) (searchKey + sizeof(enum sshs_node_attr_value_type)), key);

	mtx_lock(&node->node_lock);

	sshsNodeAttr attr;
	HASH_FIND(hh, node->attributes, searchKey, fullKeyLength, attr);

	return (attr);
}

// We don't care about unlocking anything here, as we exit hard on error anyway.
static inline void sshsNodeVerifyValidAttribute(sshsNodeAttr attr, const char *key, enum sshs_node_attr_value_type type,
	const char *funcName) {
	if (attr == NULL) {
		char errorMsg[1024];
		snprintf(errorMsg, 1024, "%s(): attribute '%s' of type '%s' not present, please create it first.", funcName,
			key, sshsHelperTypeToStringConverter(type));

		(*sshsGetGlobalErrorLogCallback())(errorMsg);

		// This is a critical usage error that *must* be fixed!
		exit(EXIT_FAILURE);
	}
}

void sshsNodeRemoveAttribute(sshsNode node, const char *key, enum sshs_node_attr_value_type type) {
	sshsNodeAttr attr = sshsNodeFindAttribute(node, key, type);

	// Verify that a valid attribute exists, else simply return
	// without doing anything. Attribute was already deleted.
	if (attr == NULL) {
		mtx_unlock(&node->node_lock);
		return;
	}

	// Remove attribute from node.
	HASH_DELETE(hh, node->attributes, attr);

	// Listener support.
	sshsNodeAttrListener l;
	LL_FOREACH(node->attrListeners, l)
	{
		l->attribute_changed(node, l->userData, SSHS_ATTRIBUTE_REMOVED, key, type, attr->value);
	}

	mtx_unlock(&node->node_lock);

	sshsNodeFreeAttribute(attr);
}

void sshsNodeRemoveAllAttributes(sshsNode node) {
	mtx_lock(&node->node_lock);

	sshsNodeAttr currAttr, tmpAttr;
	HASH_ITER(hh, node->attributes, currAttr, tmpAttr)
	{
		// Remove attribute from node.
		HASH_DELETE(hh, node->attributes, currAttr);

		// Listener support.
		sshsNodeAttrListener l;
		LL_FOREACH(node->attrListeners, l)
		{
			l->attribute_changed(node, l->userData, SSHS_ATTRIBUTE_REMOVED, currAttr->key, currAttr->value_type,
				currAttr->value);
		}

		sshsNodeFreeAttribute(currAttr);
	}

	HASH_CLEAR(hh, node->attributes);

	mtx_unlock(&node->node_lock);
}

void sshsNodeClearSubTree(sshsNode startNode, bool clearStartNode) {
	// Clear this node's attributes, if requested.
	if (clearStartNode) {
		sshsNodeRemoveAllAttributes(startNode);
		sshsNodeRemoveAllAttributeListeners(startNode);
	}

	// Recurse down children and remove all attributes.
	size_t numChildren;
	sshsNode *children = sshsNodeGetChildren(startNode, &numChildren);

	for (size_t i = 0; i < numChildren; i++) {
		sshsNodeClearSubTree(children[i], true);
	}

	free(children);
}

// children, attributes, and listeners for the child to be removed
// must be cleaned up prior to this call.
static void sshsNodeRemoveChild(sshsNode node, const char *childName) {
	mtx_shared_lock_exclusive(&node->traversal_lock);

	sshsNode toDelete = NULL;
	HASH_FIND_STR(node->children, childName, toDelete);

	if (toDelete == NULL) {
		mtx_shared_unlock_exclusive(&node->traversal_lock);
		return;
	}

	// Remove attribute from node.
	HASH_DELETE(hh, node->children, toDelete);

	mtx_shared_unlock_exclusive(&node->traversal_lock);

	mtx_lock(&node->node_lock);

	// Listener support.
	sshsNodeListener l;
	LL_FOREACH(node->nodeListeners, l)
	{
		l->node_changed(node, l->userData, SSHS_CHILD_NODE_REMOVED, childName);
	}

	mtx_unlock(&node->node_lock);

	sshsNodeDestroy(toDelete);
}

// children, attributes, and listeners for the children to be removed
// must be cleaned up prior to this call.
static void sshsNodeRemoveAllChildren(sshsNode node) {
	mtx_shared_lock_exclusive(&node->traversal_lock);
	mtx_lock(&node->node_lock);

	sshsNode currChild, tmpChild;
	HASH_ITER(hh, node->children, currChild, tmpChild)
	{
		// Remove child from node.
		HASH_DELETE(hh, node->children, currChild);

		// Listener support.
		sshsNodeListener l;
		LL_FOREACH(node->nodeListeners, l)
		{
			l->node_changed(node, l->userData, SSHS_CHILD_NODE_REMOVED, sshsNodeGetName(currChild));
		}

		sshsNodeDestroy(currChild);
	}

	HASH_CLEAR(hh, node->children);

	mtx_unlock(&node->node_lock);
	mtx_shared_unlock_exclusive(&node->traversal_lock);
}

static void sshsNodeRemoveSubTree(sshsNode node) {
	// Recurse down first, we remove from the bottom up.
	size_t numChildren;
	sshsNode *children = sshsNodeGetChildren(node, &numChildren);

	for (size_t i = 0; i < numChildren; i++) {
		sshsNodeRemoveSubTree(children[i]);
	}

	free(children);

	// Delete node listeners and children.
	sshsNodeRemoveAllChildren(node);
	sshsNodeRemoveAllNodeListeners(node);
}

// Eliminates this node and any children. Nobody can have a reference, or
// be in the process of getting one, to this node or any of its children.
// You need to make sure of this in your application!
void sshsNodeRemoveNode(sshsNode node) {
	// Now we can clear the subtree from all attribute related data.
	sshsNodeClearSubTree(node, true);

	// And finally remove the node related data and the node itself.
	sshsNodeRemoveSubTree(node);

	// If this is the root node (parent == NULL), it isn't fully removed.
	if (sshsNodeGetParent(node) != NULL) {
		// Unlink this node from the parent.
		// This also destroys the memory associated with the node.
		// Any later access is illegal!
		sshsNodeRemoveChild(sshsNodeGetParent(node), sshsNodeGetName(node));
	}
}

bool sshsNodeAttributeExists(sshsNode node, const char *key, enum sshs_node_attr_value_type type) {
	sshsNodeAttr attr = sshsNodeFindAttribute(node, key, type);

	mtx_unlock(&node->node_lock);

	// If attr == NULL, the specified attribute was not found.
	if (attr == NULL) {
		errno = ENOENT;
		return (false);
	}

	// The specified attribute exists.
	return (true);
}

bool sshsNodePutAttribute(sshsNode node, const char *key, enum sshs_node_attr_value_type type,
	union sshs_node_attr_value value) {
	sshsNodeAttr attr = sshsNodeFindAttribute(node, key, type);

	// Verify that a valid attribute exists.
	sshsNodeVerifyValidAttribute(attr, key, type, "sshsNodePutAttribute");

	// Value must be present, so update old one, after checking range and flags.
	if (attr->flags & SSHS_FLAGS_READ_ONLY) {
		// Read-only flag set, cannot put new value!
		mtx_unlock(&node->node_lock);
		errno = EPERM;
		return (false);
	}

	if (!sshsNodeCheckRange(type, value, attr->min, attr->max)) {
		// New value out of range, cannot put new value!
		mtx_unlock(&node->node_lock);
		errno = ERANGE;
		return (false);
	}

	// Key and valueType have to be the same, so only update the value
	// itself with the new one, and save the old one for later.
	union sshs_node_attr_value attrValueOld = attr->value;

	if ((attr->flags & SSHS_FLAGS_NOTIFY_ONLY) == 0) {
		if (type == SSHS_STRING) {
			// Make a copy of the string so we own the memory internally.
			char *valueCopy = strdup(value.string);
			SSHS_MALLOC_CHECK_EXIT(valueCopy);

			attr->value.string = valueCopy;
		}
		else {
			attr->value = value;
		}
	}

	// Let's check if anything changed with this update and call
	// the appropriate listeners if needed.
	if (sshsNodeCheckAttributeValueChanged(type, attrValueOld, value)) {
		// Listener support. Call only on change, which is always the case here.
		sshsNodeAttrListener l;
		LL_FOREACH(node->attrListeners, l)
		{
			l->attribute_changed(node, l->userData, SSHS_ATTRIBUTE_MODIFIED, key, type, value);
		}
	}

	mtx_unlock(&node->node_lock);

	// Free oldAttr's string memory, not used anymore.
	if ((attr->flags & SSHS_FLAGS_NOTIFY_ONLY) == 0) {
		if (type == SSHS_STRING) {
			free(attrValueOld.string);
		}
	}

	return (true);
}

static bool sshsNodeCheckAttributeValueChanged(enum sshs_node_attr_value_type type, union sshs_node_attr_value oldValue,
	union sshs_node_attr_value newValue) {
	// Check that the two values changed, that there is a difference between then.
	switch (type) {
		case SSHS_BOOL:
			return (oldValue.boolean != newValue.boolean);

		case SSHS_BYTE:
			return (oldValue.ibyte != newValue.ibyte);

		case SSHS_SHORT:
			return (oldValue.ishort != newValue.ishort);

		case SSHS_INT:
			return (oldValue.iint != newValue.iint);

		case SSHS_LONG:
			return (oldValue.ilong != newValue.ilong);

		case SSHS_FLOAT:
			return (oldValue.ffloat != newValue.ffloat);

		case SSHS_DOUBLE:
			return (oldValue.ddouble != newValue.ddouble);

		case SSHS_STRING:
			return (strcmp(oldValue.string, newValue.string) != 0);

		case SSHS_UNKNOWN:
		default:
			return (false);
	}
}

union sshs_node_attr_value sshsNodeGetAttribute(sshsNode node, const char *key, enum sshs_node_attr_value_type type) {
	sshsNodeAttr attr = sshsNodeFindAttribute(node, key, type);

	// Verify that a valid attribute exists.
	sshsNodeVerifyValidAttribute(attr, key, type, "sshsNodeGetAttribute");

	// Copy the value while still holding the lock, to ensure accessing it is
	// still possible and the value behind it valid.
	union sshs_node_attr_value value = attr->value;

	// For strings, make a copy on the heap to give out.
	if (type == SSHS_STRING) {
		char *valueCopy = strdup(value.string);
		SSHS_MALLOC_CHECK_EXIT(valueCopy);

		value.string = valueCopy;
	}

	mtx_unlock(&node->node_lock);

	// Return the final value.
	return (value);
}

static inline bool strEndsWith(const char *str, const char *suffix) {
	if (str == NULL || suffix == NULL) {
		return (false);
	}

	size_t strLength = strlen(str);
	size_t suffixLength = strlen(suffix);

	if (suffixLength > strLength) {
		return (false);
	}

	return (strncmp(str + strLength - suffixLength, suffix, suffixLength) == 0);
}

static int sshsNodeAttrCmp(const void *a, const void *b) {
	const sshsNodeAttr *aa = a;
	const sshsNodeAttr *bb = b;

	// If key ends with "ListOptions", it gets put _before_ any other key.
	bool aaIsListOptions = strEndsWith((*aa)->key, "ListOptions");
	bool bbIsListOptions = strEndsWith((*bb)->key, "ListOptions");

	if (aaIsListOptions && !bbIsListOptions) {
		return (-1);
	}
	else if (!aaIsListOptions && bbIsListOptions) {
		return (1);
	}
	else {
		// Normal compare.
		return (strcmp((*aa)->key, (*bb)->key));
	}
}

// Remember to 'mtx_unlock(&node->node_lock);' after this!
static sshsNodeAttr *sshsNodeGetAttributes(sshsNode node, size_t *numAttributes) {
	mtx_lock(&node->node_lock);

	size_t attributeCount = HASH_COUNT(node->attributes);

	// If none, exit gracefully.
	if (attributeCount == 0) {
		*numAttributes = 0;
		errno = ENOENT;
		return (NULL);
	}

	sshsNodeAttr *attributes = malloc(attributeCount * sizeof(*attributes));
	SSHS_MALLOC_CHECK_EXIT(attributes);

	size_t i = 0;
	for (sshsNodeAttr a = node->attributes; a != NULL; a = a->hh.next) {
		attributes[i++] = a;
	}

	// Sort by name.
	qsort(attributes, attributeCount, sizeof(sshsNodeAttr), &sshsNodeAttrCmp);

	*numAttributes = attributeCount;
	return (attributes);
}

// Only used to update read-only attributes, special call for module internal use only.
bool sshsNodeUpdateReadOnlyAttribute(sshsNode node, const char *key, enum sshs_node_attr_value_type type,
	union sshs_node_attr_value value) {
	sshsNodeAttr attr = sshsNodeFindAttribute(node, key, type);

	// Verify that a valid attribute exists.
	sshsNodeVerifyValidAttribute(attr, key, type, "sshsNodePutReadOnlyAttribute");

	// Value must be present, so update old one, after checking range and flags.
	if ((attr->flags & SSHS_FLAGS_READ_ONLY) == 0) {
		// Read-only flag not set, cannot update read-only value!
		mtx_unlock(&node->node_lock);
		errno = EPERM;
		return (false);
	}

	if (!sshsNodeCheckRange(type, value, attr->min, attr->max)) {
		// New value out of range, cannot put new value!
		mtx_unlock(&node->node_lock);
		errno = ERANGE;
		return (false);
	}

	// Key and valueType have to be the same, so only update the value
	// itself with the new one, and save the old one for later.
	union sshs_node_attr_value attrValueOld = attr->value;

	if (type == SSHS_STRING) {
		// Make a copy of the string so we own the memory internally.
		char *valueCopy = strdup(value.string);
		SSHS_MALLOC_CHECK_EXIT(valueCopy);

		attr->value.string = valueCopy;
	}
	else {
		attr->value = value;
	}

	// Let's check if anything changed with this update and call
	// the appropriate listeners if needed.
	if (sshsNodeCheckAttributeValueChanged(type, attrValueOld, value)) {
		// Listener support. Call only on change, which is always the case here.
		sshsNodeAttrListener l;
		LL_FOREACH(node->attrListeners, l)
		{
			l->attribute_changed(node, l->userData, SSHS_ATTRIBUTE_MODIFIED, key, type, value);
		}
	}

	mtx_unlock(&node->node_lock);

	// Free oldAttr's string memory, not used anymore.
	if (type == SSHS_STRING) {
		free(attrValueOld.string);
	}

	return (true);
}

void sshsNodeCreateBool(sshsNode node, const char *key, bool defaultValue, int flags, const char *description) {
	sshsNodeCreateAttribute(node, key, SSHS_BOOL, SSHS_VALUE_BOOL(defaultValue), SSHS_RANGES_LONG(-1, -1), flags,
		description);
}

bool sshsNodePutBool(sshsNode node, const char *key, bool value) {
	return (sshsNodePutAttribute(node, key, SSHS_BOOL, SSHS_VALUE_BOOL(value)));
}

bool sshsNodeGetBool(sshsNode node, const char *key) {
	return (sshsNodeGetAttribute(node, key, SSHS_BOOL).boolean);
}

void sshsNodeCreateByte(sshsNode node, const char *key, int8_t defaultValue, int8_t minValue, int8_t maxValue,
	int flags, const char *description) {
	sshsNodeCreateAttribute(node, key, SSHS_BYTE, SSHS_VALUE_BYTE(defaultValue), SSHS_RANGES_LONG(minValue, maxValue),
		flags, description);
}

bool sshsNodePutByte(sshsNode node, const char *key, int8_t value) {
	return (sshsNodePutAttribute(node, key, SSHS_BYTE, SSHS_VALUE_BYTE(value)));
}

int8_t sshsNodeGetByte(sshsNode node, const char *key) {
	return (sshsNodeGetAttribute(node, key, SSHS_BYTE).ibyte);
}

void sshsNodeCreateShort(sshsNode node, const char *key, int16_t defaultValue, int16_t minValue, int16_t maxValue,
	int flags, const char *description) {
	sshsNodeCreateAttribute(node, key, SSHS_SHORT, SSHS_VALUE_SHORT(defaultValue), SSHS_RANGES_LONG(minValue, maxValue),
		flags, description);
}

bool sshsNodePutShort(sshsNode node, const char *key, int16_t value) {
	return (sshsNodePutAttribute(node, key, SSHS_SHORT, SSHS_VALUE_SHORT(value)));
}

int16_t sshsNodeGetShort(sshsNode node, const char *key) {
	return (sshsNodeGetAttribute(node, key, SSHS_SHORT).ishort);
}

void sshsNodeCreateInt(sshsNode node, const char *key, int32_t defaultValue, int32_t minValue, int32_t maxValue,
	int flags, const char *description) {
	sshsNodeCreateAttribute(node, key, SSHS_INT, SSHS_VALUE_INT(defaultValue), SSHS_RANGES_LONG(minValue, maxValue),
		flags, description);
}

bool sshsNodePutInt(sshsNode node, const char *key, int32_t value) {
	return (sshsNodePutAttribute(node, key, SSHS_INT, SSHS_VALUE_INT(value)));
}

int32_t sshsNodeGetInt(sshsNode node, const char *key) {
	return (sshsNodeGetAttribute(node, key, SSHS_INT).iint);
}

void sshsNodeCreateLong(sshsNode node, const char *key, int64_t defaultValue, int64_t minValue, int64_t maxValue,
	int flags, const char *description) {
	sshsNodeCreateAttribute(node, key, SSHS_LONG, SSHS_VALUE_LONG(defaultValue), SSHS_RANGES_LONG(minValue, maxValue),
		flags, description);
}

bool sshsNodePutLong(sshsNode node, const char *key, int64_t value) {
	return (sshsNodePutAttribute(node, key, SSHS_LONG, SSHS_VALUE_LONG(value)));
}

int64_t sshsNodeGetLong(sshsNode node, const char *key) {
	return (sshsNodeGetAttribute(node, key, SSHS_LONG).ilong);
}

void sshsNodeCreateFloat(sshsNode node, const char *key, float defaultValue, float minValue, float maxValue, int flags,
	const char *description) {
	sshsNodeCreateAttribute(node, key, SSHS_FLOAT, SSHS_VALUE_FLOAT(defaultValue),
		SSHS_RANGES_DOUBLE((double ) minValue, (double )maxValue), flags, description);
}

bool sshsNodePutFloat(sshsNode node, const char *key, float value) {
	return (sshsNodePutAttribute(node, key, SSHS_FLOAT, SSHS_VALUE_FLOAT(value)));
}

float sshsNodeGetFloat(sshsNode node, const char *key) {
	return (sshsNodeGetAttribute(node, key, SSHS_FLOAT).ffloat);
}

void sshsNodeCreateDouble(sshsNode node, const char *key, double defaultValue, double minValue, double maxValue,
	int flags, const char *description) {
	sshsNodeCreateAttribute(node, key, SSHS_DOUBLE, SSHS_VALUE_DOUBLE(defaultValue),
		SSHS_RANGES_DOUBLE(minValue, maxValue), flags, description);
}

bool sshsNodePutDouble(sshsNode node, const char *key, double value) {
	return (sshsNodePutAttribute(node, key, SSHS_DOUBLE, SSHS_VALUE_DOUBLE(value)));
}

double sshsNodeGetDouble(sshsNode node, const char *key) {
	return (sshsNodeGetAttribute(node, key, SSHS_DOUBLE).ddouble);
}

void sshsNodeCreateString(sshsNode node, const char *key, const char *defaultValue, size_t minLength, size_t maxLength,
	int flags, const char *description) {
	sshsNodeCreateAttribute(node, key, SSHS_STRING, SSHS_VALUE_STR((char * ) defaultValue),
		SSHS_RANGES_LONG((int64_t ) minLength, (int64_t ) maxLength), flags, description);
}

bool sshsNodePutString(sshsNode node, const char *key, const char *value) {
	return (sshsNodePutAttribute(node, key, SSHS_STRING, SSHS_VALUE_STR((char * ) value)));
}

// This is a copy of the string on the heap, remember to free() when done!
char *sshsNodeGetString(sshsNode node, const char *key) {
	return (sshsNodeGetAttribute(node, key, SSHS_STRING).string);
}

void sshsNodeExportNodeToXML(sshsNode node, int outFd) {
	sshsNodeToXML(node, outFd, false);
}

void sshsNodeExportSubTreeToXML(sshsNode node, int outFd) {
	sshsNodeToXML(node, outFd, true);
}

#define INDENT_MAX_LEVEL 20
#define INDENT_SPACES 4
static char spaces[(INDENT_MAX_LEVEL * INDENT_SPACES) + 1] =
	"                                                                                ";

static const char *sshsNodeXMLWhitespaceCallback(mxml_node_t *node, int where) {
	const char *name = mxmlGetElement(node);
	size_t level = 0;

	// Calculate indentation level always.
	for (mxml_node_t *parent = mxmlGetParent(node); parent != NULL; parent = mxmlGetParent(parent)) {
		level++;
	}

	// Clip indentation level to maximum.
	if (level > INDENT_MAX_LEVEL) {
		level = INDENT_MAX_LEVEL;
	}

	if (strcmp(name, "sshs") == 0) {
		switch (where) {
			case MXML_WS_AFTER_OPEN:
				return ("\n");
				break;

			case MXML_WS_AFTER_CLOSE:
				return ("\n");
				break;

			default:
				break;
		}
	}
	else if (strcmp(name, "node") == 0) {
		switch (where) {
			case MXML_WS_BEFORE_OPEN:
				return (&spaces[((INDENT_MAX_LEVEL - level) * INDENT_SPACES)]);
				break;

			case MXML_WS_AFTER_OPEN:
				return ("\n");
				break;

			case MXML_WS_BEFORE_CLOSE:
				return (&spaces[((INDENT_MAX_LEVEL - level) * INDENT_SPACES)]);
				break;

			case MXML_WS_AFTER_CLOSE:
				return ("\n");
				break;

			default:
				break;
		}
	}
	else if (strcmp(name, "attr") == 0) {
		switch (where) {
			case MXML_WS_BEFORE_OPEN:
				return (&spaces[((INDENT_MAX_LEVEL - level) * INDENT_SPACES)]);
				break;

			case MXML_WS_AFTER_CLOSE:
				return ("\n");
				break;

			default:
				break;
		}
	}

	return (NULL);
}

static void sshsNodeToXML(sshsNode node, int outFd, bool recursive) {
	mxml_node_t *root = mxmlNewElement(MXML_NO_PARENT, "sshs");
	mxmlElementSetAttr(root, "version", "1.0");
	mxmlAdd(root, MXML_ADD_AFTER, MXML_ADD_TO_PARENT, sshsNodeGenerateXML(node, recursive));

	// Disable wrapping
	mxmlSetWrapMargin(0);

	// Output to file descriptor.
	mxmlSaveFd(root, outFd, &sshsNodeXMLWhitespaceCallback);

	mxmlDelete(root);
}

static mxml_node_t *sshsNodeGenerateXML(sshsNode node, bool recursive) {
	mxml_node_t *this = mxmlNewElement(MXML_NO_PARENT, "node");

	// First this node's name and full path.
	mxmlElementSetAttr(this, "name", sshsNodeGetName(node));
	mxmlElementSetAttr(this, "path", sshsNodeGetPath(node));

	size_t numAttributes;
	sshsNodeAttr *attributes = sshsNodeGetAttributes(node, &numAttributes);

	// Then it's attributes (key:value pairs).
	for (size_t i = 0; i < numAttributes; i++) {
		// If an attribute is marked NO_EXPORT, we skip it.
		if ((attributes[i]->flags & SSHS_FLAGS_NO_EXPORT)) {
			continue;
		}

		const char *type = sshsHelperTypeToStringConverter(attributes[i]->value_type);
		char *value = sshsHelperValueToStringConverter(attributes[i]->value_type, attributes[i]->value);
		SSHS_MALLOC_CHECK_EXIT(value);

		mxml_node_t *attr = mxmlNewElement(this, "attr");
		mxmlElementSetAttr(attr, "key", attributes[i]->key);
		mxmlElementSetAttr(attr, "type", type);
		mxmlNewText(attr, 0, value);

		free(value);
	}

	mtx_unlock(&node->node_lock);

	free(attributes);

	// And lastly recurse down to the children.
	if (recursive) {
		size_t numChildren;
		sshsNode *children = sshsNodeGetChildren(node, &numChildren);

		for (size_t i = 0; i < numChildren; i++) {
			mxml_node_t *child = sshsNodeGenerateXML(children[i], recursive);

			if (mxmlGetFirstChild(child) != NULL) {
				mxmlAdd(this, MXML_ADD_AFTER, MXML_ADD_TO_PARENT, child);
			}
			else {
				// Free memory if not adding.
				mxmlDelete(child);
			}
		}

		free(children);
	}

	return (this);
}

bool sshsNodeImportNodeFromXML(sshsNode node, int inFd, bool strict) {
	return (sshsNodeFromXML(node, inFd, false, strict));
}

bool sshsNodeImportSubTreeFromXML(sshsNode node, int inFd, bool strict) {
	return (sshsNodeFromXML(node, inFd, true, strict));
}

static mxml_node_t **sshsNodeXMLFilterChildNodes(mxml_node_t *node, const char *nodeName, size_t *numChildren) {
	// Go through once to count the number of matching children.
	size_t matchedChildren = 0;

	for (mxml_node_t *current = mxmlGetFirstChild(node); current != NULL; current = mxmlGetNextSibling(current)) {
		const char *name = mxmlGetElement(current);

		if (name != NULL && strcmp(name, nodeName) == 0) {
			matchedChildren++;
		}
	}

	// If none, exit gracefully.
	if (matchedChildren == 0) {
		*numChildren = 0;
		return (NULL);
	}

	// Now allocate appropriate memory for list.
	mxml_node_t **filteredNodes = malloc(matchedChildren * sizeof(mxml_node_t *));
	SSHS_MALLOC_CHECK_EXIT(filteredNodes);

	// Go thorough again and collect the matching nodes.
	size_t i = 0;
	for (mxml_node_t *current = mxmlGetFirstChild(node); current != NULL; current = mxmlGetNextSibling(current)) {
		const char *name = mxmlGetElement(current);

		if (name != NULL && strcmp(name, nodeName) == 0) {
			filteredNodes[i++] = current;
		}
	}

	*numChildren = matchedChildren;
	return (filteredNodes);
}

static bool sshsNodeFromXML(sshsNode node, int inFd, bool recursive, bool strict) {
	mxml_node_t *root = mxmlLoadFd(NULL, inFd, MXML_OPAQUE_CALLBACK);

	if (root == NULL) {
		(*sshsGetGlobalErrorLogCallback())("Failed to load XML from file descriptor.");
		return (false);
	}

	// Check name and version for compliance.
	if ((strcmp(mxmlGetElement(root), "sshs") != 0) || (strcmp(mxmlElementGetAttr(root, "version"), "1.0") != 0)) {
		mxmlDelete(root);
		(*sshsGetGlobalErrorLogCallback())("Invalid SSHS v1.0 XML content.");
		return (false);
	}

	size_t numChildren = 0;
	mxml_node_t **children = sshsNodeXMLFilterChildNodes(root, "node", &numChildren);

	if (numChildren != 1) {
		mxmlDelete(root);
		free(children);
		(*sshsGetGlobalErrorLogCallback())("Multiple or no root child nodes present.");
		return (false);
	}

	mxml_node_t *rootNode = children[0];

	free(children);

	// Strict mode: check if names match.
	if (strict) {
		const char *rootNodeName = mxmlElementGetAttr(rootNode, "name");

		if (rootNodeName == NULL || strcmp(rootNodeName, sshsNodeGetName(node)) != 0) {
			mxmlDelete(root);
			(*sshsGetGlobalErrorLogCallback())("Names don't match (required in 'strict' mode).");
			return (false);
		}
	}

	sshsNodeConsumeXML(node, rootNode, recursive);

	mxmlDelete(root);

	return (true);
}

static void sshsNodeConsumeXML(sshsNode node, mxml_node_t *content, bool recursive) {
	size_t numAttrChildren = 0;
	mxml_node_t **attrChildren = sshsNodeXMLFilterChildNodes(content, "attr", &numAttrChildren);

	for (size_t i = 0; i < numAttrChildren; i++) {
		// Check that the proper attributes exist.
		const char *key = mxmlElementGetAttr(attrChildren[i], "key");
		const char *type = mxmlElementGetAttr(attrChildren[i], "type");

		if (key == NULL || type == NULL) {
			continue;
		}

		// Get the needed values.
		const char *value = mxmlGetOpaque(attrChildren[i]);

		if (!sshsNodeStringToAttributeConverter(node, key, type, value)) {
			// Ignore read-only/range errors.
			if (errno == EPERM || errno == ERANGE) {
				continue;
			}

			char errorMsg[1024];
			snprintf(errorMsg, 1024, "Failed to convert attribute '%s' of type '%s' with value '%s' from XML.", key,
				type, value);

			(*sshsGetGlobalErrorLogCallback())(errorMsg);
		}
	}

	free(attrChildren);

	if (recursive) {
		size_t numNodeChildren = 0;
		mxml_node_t **nodeChildren = sshsNodeXMLFilterChildNodes(content, "node", &numNodeChildren);

		for (size_t i = 0; i < numNodeChildren; i++) {
			// Check that the proper attributes exist.
			const char *childName = mxmlElementGetAttr(nodeChildren[i], "name");

			if (childName == NULL) {
				continue;
			}

			// Get the child node.
			sshsNode childNode = sshsNodeGetChild(node, childName);

			// If not existing, try to create.
			if (childNode == NULL) {
				childNode = sshsNodeAddChild(node, childName);
			}

			// And call recursively.
			sshsNodeConsumeXML(childNode, nodeChildren[i], recursive);
		}

		free(nodeChildren);
	}
}

// For more precise failure reason, look at errno.
bool sshsNodeStringToAttributeConverter(sshsNode node, const char *key, const char *typeStr, const char *valueStr) {
	// Parse the values according to type and put them in the node.
	enum sshs_node_attr_value_type type;
	type = sshsHelperStringToTypeConverter(typeStr);

	if (type == SSHS_UNKNOWN) {
		errno = EINVAL;
		return (false);
	}

	union sshs_node_attr_value value;
	bool conversionSuccess = sshsHelperStringToValueConverter(type, valueStr, &value);

	if (!conversionSuccess) {
		errno = EINVAL;
		return (false);
	}

	// IFF attribute already exists, we update it using sshsNodePut(), else
	// we create the attribute with maximum range and a default description.
	// These XMl-loaded attributes are also marked NO_EXPORT.
	// This happens on XML load only. More restrictive ranges and flags can be
	// enabled later by calling sshsNodeCreate*() again as needed.
	bool result = false;

	if (sshsNodeAttributeExists(node, key, type)) {
		result = sshsNodePutAttribute(node, key, type, value);
	}
	else {
		// Create never fails!
		result = true;

		switch (type) {
			case SSHS_BOOL:
				sshsNodeCreateBool(node, key, value.boolean, SSHS_FLAGS_NORMAL | SSHS_FLAGS_NO_EXPORT,
					"XML loaded value.");
				break;

			case SSHS_BYTE:
				sshsNodeCreateByte(node, key, value.ibyte, INT8_MIN, INT8_MAX, SSHS_FLAGS_NORMAL | SSHS_FLAGS_NO_EXPORT,
					"XML loaded value.");
				break;

			case SSHS_SHORT:
				sshsNodeCreateShort(node, key, value.ishort, INT16_MIN, INT16_MAX,
					SSHS_FLAGS_NORMAL | SSHS_FLAGS_NO_EXPORT, "XML loaded value.");
				break;

			case SSHS_INT:
				sshsNodeCreateInt(node, key, value.iint, INT32_MIN, INT32_MAX, SSHS_FLAGS_NORMAL | SSHS_FLAGS_NO_EXPORT,
					"XML loaded value.");
				break;

			case SSHS_LONG:
				sshsNodeCreateLong(node, key, value.ilong, INT64_MIN, INT64_MAX,
					SSHS_FLAGS_NORMAL | SSHS_FLAGS_NO_EXPORT, "XML loaded value.");
				break;

			case SSHS_FLOAT:
				sshsNodeCreateFloat(node, key, value.ffloat, -FLT_MAX, FLT_MAX,
					SSHS_FLAGS_NORMAL | SSHS_FLAGS_NO_EXPORT, "XML loaded value.");
				break;

			case SSHS_DOUBLE:
				sshsNodeCreateDouble(node, key, value.ddouble, -DBL_MAX, DBL_MAX,
					SSHS_FLAGS_NORMAL | SSHS_FLAGS_NO_EXPORT, "XML loaded value.");
				break;

			case SSHS_STRING:
				sshsNodeCreateString(node, key, value.string, 0, INT32_MAX, SSHS_FLAGS_NORMAL | SSHS_FLAGS_NO_EXPORT,
					"XML loaded value.");
				break;

			case SSHS_UNKNOWN:
				errno = EINVAL;
				result = false;
				break;
		}
	}

	// Free string copy from helper.
	if (type == SSHS_STRING) {
		free(value.string);
	}

	return (result);
}

const char **sshsNodeGetChildNames(sshsNode node, size_t *numNames) {
	size_t numChildren;
	sshsNode *children = sshsNodeGetChildren(node, &numChildren);

	if (children == NULL) {
		*numNames = 0;
		errno = ENOENT;
		return (NULL);
	}

	const char **childNames = malloc(numChildren * sizeof(*childNames));
	SSHS_MALLOC_CHECK_EXIT(childNames);

	// Copy pointers to name string over. Safe because nodes are never deleted.
	for (size_t i = 0; i < numChildren; i++) {
		childNames[i] = sshsNodeGetName(children[i]);
	}

	free(children);

	*numNames = numChildren;
	return (childNames);
}

const char **sshsNodeGetAttributeKeys(sshsNode node, size_t *numKeys) {
	size_t numAttributes;
	sshsNodeAttr *attributes = sshsNodeGetAttributes(node, &numAttributes);

	if (attributes == NULL) {
		mtx_unlock(&node->node_lock);

		*numKeys = 0;
		errno = ENOENT;
		return (NULL);
	}

	const char **attributeKeys = malloc(numAttributes * sizeof(*attributeKeys));
	SSHS_MALLOC_CHECK_EXIT(attributeKeys);

	// Copy pointers to key string over. Safe because attributes are never deleted.
	for (size_t i = 0; i < numAttributes; i++) {
		attributeKeys[i] = attributes[i]->key;
	}

	mtx_unlock(&node->node_lock);

	free(attributes);

	*numKeys = numAttributes;
	return (attributeKeys);
}

enum sshs_node_attr_value_type *sshsNodeGetAttributeTypes(sshsNode node, const char *key, size_t *numTypes) {
	size_t numAttributes;
	sshsNodeAttr *attributes = sshsNodeGetAttributes(node, &numAttributes);

	if (attributes == NULL) {
		mtx_unlock(&node->node_lock);

		*numTypes = 0;
		errno = ENOENT;
		return (NULL);
	}

	// There are at most 8 types for one specific attribute key.
	enum sshs_node_attr_value_type *attributeTypes = malloc(8 * sizeof(*attributeTypes));
	SSHS_MALLOC_CHECK_EXIT(attributeTypes);

	// Check each attribute if it matches, and save its type if true.
	size_t typeCounter = 0;

	for (size_t i = 0; i < numAttributes; i++) {
		if (strcmp(key, attributes[i]->key) == 0) {
			attributeTypes[typeCounter++] = attributes[i]->value_type;
		}
	}

	mtx_unlock(&node->node_lock);

	free(attributes);

	// If we found nothing, return nothing.
	if (typeCounter == 0) {
		free(attributeTypes);
		attributeTypes = NULL;
	}

	*numTypes = typeCounter;
	return (attributeTypes);
}

struct sshs_node_attr_ranges sshsNodeGetAttributeRanges(sshsNode node, const char *key,
	enum sshs_node_attr_value_type type) {
	sshsNodeAttr attr = sshsNodeFindAttribute(node, key, type);

	// Verify that a valid attribute exists.
	sshsNodeVerifyValidAttribute(attr, key, type, "sshsNodeGetAttributeMinRange");

	union sshs_node_attr_range minRange = attr->min;
	union sshs_node_attr_range maxRange = attr->max;

	mtx_unlock(&node->node_lock);

	struct sshs_node_attr_ranges result = { .min = minRange, .max = maxRange };

	return (result);
}

int sshsNodeGetAttributeFlags(sshsNode node, const char *key, enum sshs_node_attr_value_type type) {
	sshsNodeAttr attr = sshsNodeFindAttribute(node, key, type);

	// Verify that a valid attribute exists.
	sshsNodeVerifyValidAttribute(attr, key, type, "sshsNodeGetAttributeFlags");

	int flags = attr->flags;

	mtx_unlock(&node->node_lock);

	return (flags);
}

char *sshsNodeGetAttributeDescription(sshsNode node, const char *key, enum sshs_node_attr_value_type type) {
	sshsNodeAttr attr = sshsNodeFindAttribute(node, key, type);

	// Verify that a valid attribute exists.
	sshsNodeVerifyValidAttribute(attr, key, type, "sshsNodeGetAttributeDescription");

	char *descriptionCopy = strdup(attr->description);
	SSHS_MALLOC_CHECK_EXIT(descriptionCopy);

	mtx_unlock(&node->node_lock);

	return (descriptionCopy);
}

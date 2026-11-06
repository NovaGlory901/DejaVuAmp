// miniaudio (vendored in shared/native/third_party/miniaudio, shared on purpose) with the same
// switches everywhere it is included: its structures change with them, so every file that includes
// it must include it through here. Only src/miniaudio_impl.cpp holds the implementation.
#pragma once

#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#include "miniaudio.h"

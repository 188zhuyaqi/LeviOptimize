// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The SDK describes the native compression layout but does not export its
// virtual implementations. We only construct our complete derived peer, never
// a bare CompressedNetworkPeer. Suppress intermediate base vtable stores so
// constructor/destructor code does not import unavailable codec entry points.
// This changes neither field layout nor virtual slots of existing BDS objects.
class __declspec(novtable) CompressedNetworkPeer;

#include "mc/network/CompressedNetworkPeer.h"

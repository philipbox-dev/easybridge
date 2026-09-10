#pragma once
// ============================================================
// Blacklist — node IDs blocked at the lowest level
// Packets from blacklisted nodes are dropped after header parse,
// BEFORE any decryption or relay processing.
// The blocked node receives no ACK — appears as "not delivered".
// ============================================================
#include <stdint.h>
#include <stdbool.h>

#define BLACKLIST_MAX 16

void blacklist_init(void);
void blacklist_add(uint32_t node_id);
void blacklist_remove(uint32_t node_id);
bool blacklist_contains(uint32_t node_id);
void blacklist_clear(void);
// Persist to NVS
void blacklist_save(void);
void blacklist_load(void);

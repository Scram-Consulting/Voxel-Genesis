#pragma once

#include "BlockType.h"

// ============================================================================
// INVENTARIO DEL JUGADOR
// ============================================================================
// Extraído de main.cpp: solo depende de BlockType, así que es testeable sin
// arrancar OpenGL.

struct InventorySlot {
    BlockType blockType;
    int count;

    InventorySlot() : blockType(BLOCK_AIR), count(0) {}

    bool isEmpty() const { return blockType == BLOCK_AIR || count <= 0; }

    bool canStack(BlockType type) const {
        return isEmpty() || (blockType == type && count < MAX_STACK_SIZE);
    }

    // Devuelve cuántos items se guardaron realmente: el resto se descarta al
    // topar con MAX_STACK_SIZE y el llamador debe decidir qué hacer con ellos.
    int add(BlockType type, int amount = 1) {
        if (amount <= 0) return 0;

        if (isEmpty()) {
            blockType = type;
            count = (amount > MAX_STACK_SIZE) ? MAX_STACK_SIZE : amount;
            return count;
        }
        if (blockType == type) {
            int space = MAX_STACK_SIZE - count;
            int added = (amount > space) ? space : amount;
            count += added;
            return added;
        }
        return 0;  // slot ocupado por otro tipo
    }

    bool remove(int amount = 1) {
        if (count >= amount) {
            count -= amount;
            if (count <= 0) {
                blockType = BLOCK_AIR;
                count = 0;
            }
            return true;
        }
        return false;
    }
};

struct Inventory {
    static const int SLOTS = 45;  // 45 slots (5 filas de 9)
    InventorySlot slots[SLOTS];
    int selectedSlot;

    Inventory() : selectedSlot(0) {
        // Inventario vacío al inicio: el jugador consigue items minando/crafteando
    }

    void clear() {
        for (int i = 0; i < SLOTS; i++) {
            slots[i].blockType = BLOCK_AIR;
            slots[i].count = 0;
        }
        selectedSlot = 0;
    }

    // Reparte amount entre los slots disponibles (stackea en los existentes
    // antes de ocupar uno vacío). Devuelve false si no cupo todo: antes
    // devolvía true aunque el clamp a MAX_STACK_SIZE descartara items en
    // silencio, y el llamador daba por perdida la diferencia sin saberlo.
    bool addItem(BlockType type, int amount = 1) {
        if (type == BLOCK_AIR || amount <= 0) return false;

        int remaining = amount;
        for (int i = 0; i < SLOTS && remaining > 0; i++) {
            if (slots[i].canStack(type)) {
                remaining -= slots[i].add(type, remaining);
            }
        }
        return remaining == 0;
    }

    bool removeItem(BlockType type, int amount = 1) {
        for (int i = 0; i < SLOTS; i++) {
            if (slots[i].blockType == type && slots[i].count >= amount) {
                slots[i].remove(amount);
                return true;
            }
        }
        return false;
    }

    // Total de items de un tipo repartidos por todos los slots
    int countItem(BlockType type) const {
        int total = 0;
        for (int i = 0; i < SLOTS; i++) {
            if (slots[i].blockType == type) total += slots[i].count;
        }
        return total;
    }

    BlockType getSelectedBlock() const {
        if (selectedSlot >= 0 && selectedSlot < SLOTS) {
            return slots[selectedSlot].blockType;
        }
        return BLOCK_AIR;
    }

    bool hasSelectedBlock() const {
        if (selectedSlot >= 0 && selectedSlot < SLOTS) {
            return !slots[selectedSlot].isEmpty();
        }
        return false;
    }

    void consumeSelected() {
        if (selectedSlot >= 0 && selectedSlot < SLOTS) {
            slots[selectedSlot].remove(1);
        }
    }
};

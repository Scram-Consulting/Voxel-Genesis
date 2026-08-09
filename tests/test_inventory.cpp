#include <doctest/doctest.h>

#include "Inventory.h"

// ============================================================================
// InventorySlot
// ============================================================================

TEST_CASE("InventorySlot: nace vacio") {
    InventorySlot slot;
    CHECK(slot.isEmpty());
    CHECK(slot.blockType == BLOCK_AIR);
    CHECK(slot.count == 0);
}

TEST_CASE("InventorySlot: add sobre slot vacio") {
    InventorySlot slot;
    CHECK(slot.add(BLOCK_STONE, 5) == 5);
    CHECK(slot.blockType == BLOCK_STONE);
    CHECK(slot.count == 5);
    CHECK_FALSE(slot.isEmpty());
}

TEST_CASE("InventorySlot: add acumula sobre el mismo tipo") {
    InventorySlot slot;
    slot.add(BLOCK_STONE, 5);
    CHECK(slot.add(BLOCK_STONE, 3) == 3);
    CHECK(slot.count == 8);
}

TEST_CASE("InventorySlot: add rechaza un tipo distinto") {
    InventorySlot slot;
    slot.add(BLOCK_STONE, 5);
    CHECK(slot.add(BLOCK_DIRT, 3) == 0);
    CHECK(slot.blockType == BLOCK_STONE);
    CHECK(slot.count == 5);
}

// Regresión: add() devolvía void y recortaba a MAX_STACK_SIZE en silencio, así
// que el llamador daba por guardados items que en realidad se perdían.
TEST_CASE("InventorySlot: add informa cuantos items caben de verdad") {
    InventorySlot slot;
    slot.add(BLOCK_STONE, 95);
    CHECK(slot.add(BLOCK_STONE, 20) == 5);   // solo caben 5 más
    CHECK(slot.count == MAX_STACK_SIZE);
    CHECK(slot.add(BLOCK_STONE, 10) == 0);   // ya está lleno
    CHECK(slot.count == MAX_STACK_SIZE);
}

TEST_CASE("InventorySlot: add sobre slot vacio recorta al tamano de stack") {
    InventorySlot slot;
    CHECK(slot.add(BLOCK_STONE, 500) == MAX_STACK_SIZE);
    CHECK(slot.count == MAX_STACK_SIZE);
}

TEST_CASE("InventorySlot: add ignora cantidades no positivas") {
    InventorySlot slot;
    CHECK(slot.add(BLOCK_STONE, 0) == 0);
    CHECK(slot.add(BLOCK_STONE, -5) == 0);
    CHECK(slot.isEmpty());
}

TEST_CASE("InventorySlot: canStack") {
    InventorySlot slot;
    CHECK(slot.canStack(BLOCK_STONE));       // vacío acepta cualquier cosa

    slot.add(BLOCK_STONE, 50);
    CHECK(slot.canStack(BLOCK_STONE));
    CHECK_FALSE(slot.canStack(BLOCK_DIRT));

    slot.add(BLOCK_STONE, 50);               // ahora está lleno
    CHECK_FALSE(slot.canStack(BLOCK_STONE));
}

TEST_CASE("InventorySlot: remove vacia el slot al llegar a cero") {
    InventorySlot slot;
    slot.add(BLOCK_STONE, 3);

    CHECK(slot.remove(1));
    CHECK(slot.count == 2);

    CHECK(slot.remove(2));
    CHECK(slot.isEmpty());
    CHECK(slot.blockType == BLOCK_AIR);   // invariante: count==0 implica AIR
}

TEST_CASE("InventorySlot: remove falla si no hay suficientes") {
    InventorySlot slot;
    slot.add(BLOCK_STONE, 3);

    CHECK_FALSE(slot.remove(5));
    CHECK(slot.count == 3);   // no se modifica en el fallo
}

// ============================================================================
// Inventory
// ============================================================================

TEST_CASE("Inventory: nace vacio") {
    Inventory inv;
    CHECK(inv.selectedSlot == 0);
    CHECK(inv.countItem(BLOCK_STONE) == 0);
    CHECK_FALSE(inv.hasSelectedBlock());
    CHECK(inv.getSelectedBlock() == BLOCK_AIR);
}

TEST_CASE("Inventory: addItem coloca items y los cuenta") {
    Inventory inv;
    CHECK(inv.addItem(BLOCK_STONE, 10));
    CHECK(inv.countItem(BLOCK_STONE) == 10);
    CHECK(inv.slots[0].blockType == BLOCK_STONE);
}

TEST_CASE("Inventory: addItem apila en el slot existente antes de ocupar otro") {
    Inventory inv;
    inv.addItem(BLOCK_STONE, 10);
    inv.addItem(BLOCK_STONE, 10);

    CHECK(inv.countItem(BLOCK_STONE) == 20);
    CHECK(inv.slots[0].count == 20);
    CHECK(inv.slots[1].isEmpty());
}

// Regresión: addItem() devolvía true tras un único add() que recortaba a
// MAX_STACK_SIZE, perdiendo el excedente sin avisar. Ahora reparte entre slots.
TEST_CASE("Inventory: addItem reparte el excedente en varios slots") {
    Inventory inv;
    CHECK(inv.addItem(BLOCK_STONE, 250));

    CHECK(inv.countItem(BLOCK_STONE) == 250);
    CHECK(inv.slots[0].count == MAX_STACK_SIZE);
    CHECK(inv.slots[1].count == MAX_STACK_SIZE);
    CHECK(inv.slots[2].count == 50);
}

TEST_CASE("Inventory: addItem devuelve false cuando el inventario esta lleno") {
    Inventory inv;
    const int capacity = Inventory::SLOTS * MAX_STACK_SIZE;

    CHECK(inv.addItem(BLOCK_STONE, capacity));
    CHECK(inv.countItem(BLOCK_STONE) == capacity);

    CHECK_FALSE(inv.addItem(BLOCK_STONE, 1));   // ya no cabe nada
    CHECK(inv.countItem(BLOCK_STONE) == capacity);
}

TEST_CASE("Inventory: addItem rechaza aire y cantidades no positivas") {
    Inventory inv;
    CHECK_FALSE(inv.addItem(BLOCK_AIR, 5));
    CHECK_FALSE(inv.addItem(BLOCK_STONE, 0));
    CHECK_FALSE(inv.addItem(BLOCK_STONE, -3));
    CHECK(inv.slots[0].isEmpty());
}

TEST_CASE("Inventory: removeItem quita del primer slot con suficientes") {
    Inventory inv;
    inv.addItem(BLOCK_STONE, 10);

    CHECK(inv.removeItem(BLOCK_STONE, 4));
    CHECK(inv.countItem(BLOCK_STONE) == 6);

    CHECK_FALSE(inv.removeItem(BLOCK_STONE, 100));   // no hay tantos en un slot
    CHECK(inv.countItem(BLOCK_STONE) == 6);

    CHECK_FALSE(inv.removeItem(BLOCK_DIRT, 1));      // tipo ausente
}

TEST_CASE("Inventory: clear deja todo vacio") {
    Inventory inv;
    inv.addItem(BLOCK_STONE, 50);
    inv.addItem(BLOCK_DIRT, 30);
    inv.selectedSlot = 5;

    inv.clear();

    CHECK(inv.selectedSlot == 0);
    CHECK(inv.countItem(BLOCK_STONE) == 0);
    CHECK(inv.countItem(BLOCK_DIRT) == 0);
    for (int i = 0; i < Inventory::SLOTS; i++) {
        CHECK(inv.slots[i].isEmpty());
    }
}

TEST_CASE("Inventory: slot seleccionado") {
    Inventory inv;
    inv.addItem(BLOCK_STONE, 5);

    inv.selectedSlot = 0;
    CHECK(inv.hasSelectedBlock());
    CHECK(inv.getSelectedBlock() == BLOCK_STONE);

    inv.consumeSelected();
    CHECK(inv.slots[0].count == 4);

    inv.selectedSlot = 10;   // slot vacío
    CHECK_FALSE(inv.hasSelectedBlock());
    CHECK(inv.getSelectedBlock() == BLOCK_AIR);
}

TEST_CASE("Inventory: un slot fuera de rango no rompe nada") {
    Inventory inv;
    inv.addItem(BLOCK_STONE, 5);

    inv.selectedSlot = Inventory::SLOTS + 10;
    CHECK(inv.getSelectedBlock() == BLOCK_AIR);
    CHECK_FALSE(inv.hasSelectedBlock());
    inv.consumeSelected();   // no debe escribir fuera del array
    CHECK(inv.countItem(BLOCK_STONE) == 5);

    inv.selectedSlot = -1;
    CHECK(inv.getSelectedBlock() == BLOCK_AIR);
    CHECK_FALSE(inv.hasSelectedBlock());
    inv.consumeSelected();
    CHECK(inv.countItem(BLOCK_STONE) == 5);
}

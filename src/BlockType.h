#pragma once

// ============================================================================
// TIPOS DE BLOQUES
// ============================================================================
// Extraído de main.cpp para que los tests (y PalettedStorage.h) puedan usar el
// enum real sin arrastrar el resto del motor.

enum BlockType {
    BLOCK_AIR = 0,
    BLOCK_GRASS,
    BLOCK_DIRT,
    BLOCK_STONE,
    BLOCK_WOOD,
    BLOCK_LEAVES,
    BLOCK_SAND,
    BLOCK_WATER,
    BLOCK_TALLGRASS,
    BLOCK_BEDROCK,
    BLOCK_COBBLESTONE,
    BLOCK_PLANKS,
    BLOCK_BRICKS,
    BLOCK_GLASS,
    BLOCK_COAL_ORE,
    BLOCK_DIAMOND_ORE,
    BLOCK_GRAVEL,         // Grava - genera en ríos y bajo tierra
    BLOCK_ORANGE_FLOWER,  // Flor naranja - genera en praderas
    BLOCK_SNOW,           // Nieve - genera en biomas fríos
    BLOCK_SCRAP_METAL,    // Desecho de metales
    BLOCK_LAVA,           // Lava - genera en cuevas profundas
    // Nuevos minerales agregados al final para no romper mundos existentes
    BLOCK_IRON_ORE,       // Hierro - poco común
    BLOCK_GOLD_ORE,       // Oro - poco común en ríos y océanos
    BLOCK_SILVER_ORE,     // Plata - poco común en ríos y océanos
    // Nuevos items crafteables
    BLOCK_DIRT_POWDER,    // Polvo de tierra - crafteable desde tierra
    BLOCK_STICK,          // Palo - crafteable desde tablones
    BLOCK_HOE,            // Hoz - herramienta crafteable
    // Nuevos items que dropean los minerales
    BLOCK_COAL_ITEM,      // Carbón (item) - dropea del mineral de carbón
    BLOCK_RAW_ZINC,       // Zinc crudo - dropea del desecho de metales
    BLOCK_RAW_COPPER      // Cobre crudo - dropea del desecho de metales
};

// Último valor válido del enum: usado para validar datos leídos de archivos.
// ⚠️ Actualizar si se añaden bloques al final del enum.
constexpr int BLOCK_TYPE_MAX = BLOCK_RAW_COPPER;

// Límite de items por slot (inventario y validación de saves)
constexpr int MAX_STACK_SIZE = 100;

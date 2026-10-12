#include <catch2/catch_all.hpp>

#include "libslic3r/GCode/ShopHotUnload.hpp"

#include <string>

using namespace Slic3r;

// The QIDI Plus 4 change G-code before the shop's hot unload: M104 to the top temperature without waiting,
// then UNLOAD_T straight away, and the purge at the higher of the two filaments' top temperatures.
static const std::string old_qidi_change =
    "CUT_FILAMENT T=0\n"
    "MOVE_TO_TRASH\n"
    "M400\n"
    "M104 S260\n"
    "M106 S0\n"
    "UNLOAD_T0\n"
    "G92 E0\n"
    "T1\n"
    "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=260 WAIT=1\n"
    "; FLUSH_START\n"
    "G1 E30 F300\n"
    "; FLUSH_END\n"
    "M104 S240\n"
    "M109 S240\n"
    "TOOL_CHANGE_END\n";

TEST_CASE("Unload temperature: print + 30 C, between the top temperature and 10 C above it", "[ShopHotUnload]")
{
    CHECK(shop_unload_temperature(240, 260) == 270); // SUNLU matte PETG
    CHECK(shop_unload_temperature(250, 280) == 280); // Generic PETG: the top temperature
    CHECK(shop_unload_temperature(220, 240) == 250); // PLA: capped 10 C above the top temperature
    CHECK(shop_unload_temperature(200, 260) == 260); // never below the top temperature
}

TEST_CASE("Old QIDI change G-code gets a hot unload and a hot purge", "[ShopHotUnload]")
{
    const std::string out = shop_hot_unload(old_qidi_change, 270);
    const size_t      wait = out.find("SET_HEATER_TEMPERATURE HEATER=extruder TARGET=270 WAIT=1 ; shop: unload hot\nUNLOAD_T0\n");
    CHECK(wait != std::string::npos);
    // The purge after loading the next filament is raised to the unload temperature...
    CHECK(out.find("T1\nSET_HEATER_TEMPERATURE HEATER=extruder TARGET=270 WAIT=1\n; FLUSH_START") != std::string::npos);
    // ...and the return to print temperature after the purge is left alone.
    CHECK(out.find("; FLUSH_END\nM104 S240\nM109 S240\n") != std::string::npos);
    // M104 before the unload (no wait) is not counted as hot and not changed.
    CHECK(out.find("M104 S260\n") != std::string::npos);
}

TEST_CASE("Change G-code that already unloads hot is left unchanged", "[ShopHotUnload]")
{
    const std::string shop =
        "M106 S0\n"
        "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=270 WAIT=1\n"
        "UNLOAD_T0\n"
        "T1\n"
        "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=280 WAIT=1\n"
        "; FLUSH_START\n"
        "; FLUSH_END\n"
        "M104 S250\n";
    CHECK(shop_hot_unload(shop, 270) == shop);
    CHECK(shop_hot_unload("T1\nM104 S250\n", 270) == "T1\nM104 S250\n"); // no UNLOAD_T: not a QIDI Box change
}

TEST_CASE("Without FLUSH_END markers the purge ends at the next filament's temperature wait", "[ShopHotUnload]")
{
    const std::string in  = "UNLOAD_T2\nT0\nM109 S255\nG1 E40\nM104 S230\n";
    const std::string out = shop_hot_unload(in, 270);
    CHECK(out == "SET_HEATER_TEMPERATURE HEATER=extruder TARGET=270 WAIT=1 ; shop: unload hot\nUNLOAD_T2\nT0\nM109 S270\nG1 E40\nM104 S230\n");
}

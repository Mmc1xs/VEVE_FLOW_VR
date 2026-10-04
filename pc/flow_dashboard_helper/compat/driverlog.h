#pragma once

// flow_steamvr_driver's NVENC encoder (shared with the helper) logs through DriverLog; in the
// helper it goes to flow_dashboard_helper.log (defined in main.cpp).
void DriverLog( const char *format, ... );

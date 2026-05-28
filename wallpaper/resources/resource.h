#pragma once

// Resource IDs shared between app.rc and the C++ side (tray icon load, etc.).
// IDI_APP_ICON must be the lowest-numbered ICON resource so Explorer uses it
// as the exe's file icon — Windows picks the first icon in alphabetical-by-ID
// order, so keep this 101 and the tray icon higher.
#define IDI_APP_ICON   101
#define IDI_TRAY_ICON  102

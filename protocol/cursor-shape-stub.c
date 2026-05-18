/* The generated cursor-shape-protocol.c references zwp_tablet_tool_v2_interface
   because cursor-shape-v1 includes a get_tablet_tool_v2 request. We only use
   the pointer half of the protocol (get_pointer / set_shape), but the symbol
   is still pulled in through the request signature table at link time.

   Rather than dragging in the entire wayland-protocols tablet-v2 protocol,
   provide a minimal stub. The struct is only chased if get_tablet_tool_v2
   is ever called, which we never do. */

#include <wayland-client.h>

const struct wl_interface zwp_tablet_tool_v2_interface = {
    "zwp_tablet_tool_v2",  /* name    */
    1,                     /* version */
    0, (void*)0,           /* method_count, methods */
    0, (void*)0,           /* event_count,  events  */
};

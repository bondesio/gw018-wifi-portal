#ifndef GW018_PORTAL_H
#define GW018_PORTAL_H

/* AP HTTP and station Zigbee sockets are separate listeners on port 80. */

/* Called by the physical-button task. A second click closes setup mode. */
void gw018_portal_toggle(void);
void gw018_portal_tick(void);

#endif

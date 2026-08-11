#pragma once

/* Start a background task that watches the BOOT button (GPIO0). Holding it
 * continuously for 10 seconds triggers a FACTORY RESET (erases the whole
 * NVS: node table, NetKey/AppKey, all other config; firmware is NOT
 * touched) and then reboots.
 * Call once at boot, as early as possible. */
void bmt_factory_reset_init(void);
